#include "agent.h"
#include "rag_retrieval.h"
#include "json_value.h"
#include "socket_platform.h"
#include <sqlite3.h>
#include <curl/curl.h>
#include <atomic>
#include <thread>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include "../web_server.cpp"

namespace {
struct Call { std::string messages, tools; bool json; };
std::vector<Call> calls;
std::deque<std::string> replies;
void require(bool ok, const std::string& message) { if (!ok) throw std::runtime_error(message); }
struct Fixture {
    std::filesystem::path directory, config;
    Fixture(int summaryEvery = 100, int rawLimit = 5) {
        directory = std::filesystem::temp_directory_path() / ("rag-day23-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directory(directory);
        config = directory / "agent.json";
        std::ofstream(config) << "{\"model\":\"mock-model\",\"base_instruction\":\"TEST_BASE\",\"input_policy\":\"TEST_INPUT\",\"output_policy\":\"TEST_OUTPUT\",\"short_term_memory_messages\":" << rawLimit << ",\"summary_every_requests\":" << summaryEvery << ",\"input_price_per_million\":0,\"cached_input_price_per_million\":0,\"output_price_per_million\":0,\"long_term_memory_db\":\"" << (directory / "memory.db").generic_string() << "\",\"project_invariants_db\":\"" << (directory / "invariants.db").generic_string() << "\"}";
    }
    ~Fixture() {
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(directory, ec))
            if (entry.is_regular_file()) std::filesystem::remove(entry.path(), ec);
        std::filesystem::remove(directory, ec);
    }
};
document_index::SearchHit hit(double score, const std::string& section, const std::string& content = {}) {
    document_index::SearchHit h; h.score = score; h.chunk.section = section; h.chunk.content = content;
    h.chunk.file = "other.md"; h.chunk.chunkId = section; h.chunk.sourceType = "project";
    return h;
}
void testConfig() {
    rag::Config config; std::string error;
    require(rag::Config::load(std::string(PROJECT_SOURCE_ROOT) + "/rag_config.json", config, error), error);
    require(config.preFilterTopK == 10 && config.postFilterTopK == 4 && config.similarityThreshold == .45, "Config defaults");
    Fixture fixture; auto path = fixture.directory / "rag.json";
    std::ofstream(path) << "{\"pre_filter_top_k\":7,\"post_filter_top_k\":2,\"similarity_threshold\":0.6}";
    require(rag::Config::load(path.string(), config, error) && config.preFilterTopK == 7 && config.postFilterTopK == 2 && config.similarityThreshold == .6, "Custom config");
    std::ofstream(path) << "{\"pre_filter_top_k\":2,\"post_filter_top_k\":4,\"similarity_threshold\":0.45}";
    require(!rag::Config::load(path.string(), config, error), "Reject inverted limits");
}
void testThresholdAndEmpty() {
    rag::Config config;
    auto result = rag::selectCandidates("McpManager", {hit(.449, "McpManager", "McpManager"), hit(.45, "boundary"), hit(.7, "valid"), hit(std::numeric_limits<double>::quiet_NaN(), "nan")}, config);
    require(result.size() == 2 && result[1].score == .45, "Threshold must precede heuristic bonus and include boundary");
    require(rag::selectCandidates("McpManager", {hit(.44, "McpManager")}, config).empty(), "Empty filtered results");
}
void testRerankAndMetadata() {
    rag::Config config;
    auto entity = hit(.60, "McpManager::callTool", "McpManager callTool dispatch");
    entity.chunk.file = "mcp_manager.cpp"; entity.chunk.sourceType = "upload";
    entity.chunk.page = 3; entity.chunk.lineStart = 11; entity.chunk.lineEnd = 28;
    auto result = rag::selectCandidates("McpManager callTool mcp_manager.cpp", {hit(.68, "generic"), entity, hit(.95, "semantic")}, config);
    require(result[0].chunk.section == "semantic" && result[1].chunk.section == entity.chunk.section, "Cosine dominance and entity reranking");
    const auto& selected = result[1];
    require(selected.score == .60 && selected.relevanceScore > .68 && selected.relevanceScore <= .72 + 1e-12, "Bounded bonus, original cosine preserved");
    require(selected.chunk.file == entity.chunk.file && selected.chunk.chunkId == entity.chunk.chunkId && selected.chunk.sourceType == "upload" && selected.chunk.page == 3 && selected.chunk.lineStart == 11 && selected.chunk.lineEnd == 28, "Source metadata preserved");
    require(result[0].chunk.sourceType == "project", "Project and uploads both retained");
    entity.chunk.embedding = {1234.5f, 6789.0f};
    const auto context = rag::buildContext({entity});
    require(context.find("embedding") == std::string::npos && context.find("1234.5") == std::string::npos &&
            context.find("manual.pdf") == std::string::npos && context.find("mcp_manager.cpp") != std::string::npos &&
            context.find("\"page\":3") != std::string::npos && context.find("\"line_end\":28") != std::string::npos,
            "Context serializes source metadata and text without vectors");
}
void testLimits() {
    rag::Config config; std::vector<document_index::SearchHit> candidates;
    for (int i = 0; i < 10; ++i) candidates.push_back(hit(.7 - i*.001, "candidate" + std::to_string(i)));
    candidates.push_back(hit(.69, "McpManager", "McpManager"));
    auto result = rag::selectCandidates("McpManager", candidates, config);
    require(result.size() == 4, "Post filter top 4");
    require(std::none_of(result.begin(), result.end(), [](const auto& h) { return h.chunk.section == "McpManager"; }), "Only semantic top 10 reranked");
}
void enqueueAnswer() { replies = {"final answer", "{\"accepted\":true}", "{\"facts\":[]}"}; }
void testRewriteAndFinalQuestion() {
    Fixture fixture; Agent agent(fixture.config.string()); std::string error, rewritten, answer;
    require(agent.isReady(), agent.initializationError());
    auto chats = agent.chats(); auto chat = std::find_if(chats.begin(), chats.end(), [](const auto& c) { return c.mode == "assistant"; });
    require(chat != chats.end(), "Assistant chat exists");
    calls.clear(); replies = {"{\"query\":\"McpManager callTool dispatch\"}"};
    const std::string original = "Where does tool dispatch happen?";
    require(agent.rewriteRetrievalQuery(chat->id, original, rewritten, error), error);
    require(rewritten == "McpManager callTool dispatch" && calls.size() == 1 && calls[0].json && calls[0].tools.empty(), "Separate JSON rewrite without tools");
    require(calls[0].messages.find(original) != std::string::npos, "Rewrite receives original question");
    require(agent.visibleConversation().empty() && agent.completedRequestCount() == 0 && agent.rawHistoryMessageCount() == 0, "Rewrite does not create history or memory turn");
    Agent::RagSource source{"manual.pdf", "Dispatch", "upload:42", "upload", 3, 0, 0, .72};
    auto retrieved = hit(.72, "Dispatch", "Dispatch documentation");
    retrieved.chunk.file = "manual.pdf"; retrieved.chunk.chunkId = "upload:42"; retrieved.chunk.sourceType = "upload"; retrieved.chunk.page = 3; retrieved.relevanceScore = .72;
    const std::string context = rag::buildContext({retrieved});
    replies = {"{\"answer\":\"final answer\",\"sources\":[{\"chunk_id\":\"upload:42\"}]}", "{\"accepted\":true}", "{\"facts\":[]}"};
    require(agent.handleChatMessage(chat->id, original, answer, error, context, {source}), error);
    require(calls[1].messages.find(original) != std::string::npos && calls[1].messages.find("Dispatch documentation") != std::string::npos, "Answer receives original plus text context");
    require(calls[1].messages.find(rewritten) == std::string::npos, "Rewritten question not used as final question");
    const auto& transcript = agent.visibleConversation();
    require(transcript.size() == 2 && transcript[0].content == original && transcript[1].ragSources.size() == 1, "Original question and sources retained");
    require(transcript[1].ragSources[0].page == 3 && transcript[1].ragSources[0].relevanceScore == .72 && transcript[1].ragSources[0].chunkId == "upload:42", "Answer metadata preserved");
    calls.clear(); enqueueAnswer();
    require(agent.handleChatMessage(chat->id, "plain question", answer, error), error);
    require(calls.size() == 3 && calls[0].messages.find("Dispatch documentation") == std::string::npos && agent.visibleConversation().back().ragSources.empty(), "RAG OFF / empty context uses ordinary flow and clears sources");
    calls.clear(); replies = {"{\"query\":\"ReminderScheduler\"}"};
    require(agent.rewriteRetrievalQuery(chat->id, "Where are reminders scheduled?", rewritten, error), error);
    const auto emptyHits = rag::selectCandidates(rewritten, {hit(.2, "ReminderScheduler")}, rag::Config{});
    require(emptyHits.empty(), "No weak candidate enters answer context");
    replies = {"{\"facts\":[]}"};
    require(agent.handleChatMessage(chat->id, "Where are reminders scheduled?", answer, error, {}, {}, true), error);
    require(calls.size() == 2 && answer == rag::dontKnowAnswer() && agent.visibleConversation().back().ragSources.empty(), "Empty RAG result is a backend refusal, with no answer LLM call");
    calls.clear(); replies = {"{\"query\":\"\"}"};
    auto count = agent.visibleConversation().size();
    require(!agent.rewriteRetrievalQuery(chat->id, original, rewritten, error) && agent.visibleConversation().size() == count, "Invalid rewrite fails without new turn");
}
struct EnvGuard {
    std::string name, saved;
    EnvGuard(const char* key, const std::string& value) : name(key) {
        const char* old = std::getenv(key); saved = old ? old : ""; _putenv_s(key, value.c_str());
    }
    ~EnvGuard() { _putenv_s(name.c_str(), saved.c_str()); }
};
// Real HTTP embedding transport against a deterministic local test stub.
// This never contacts the installed Ollama or OpenAI.
class EmbeddingStub {
    net::SocketRuntime runtime;
    net::Socket listener = net::invalidSocket;
    std::atomic<bool> stopped{false};
    std::thread worker;
public:
    std::atomic<int> requests{0};
    std::string url;
    EmbeddingStub() {
        require(runtime.ready(), "Socket startup");
        listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        require(listener != net::invalidSocket && bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 && listen(listener, 4) == 0, "Embedding stub bind");
        net::SocketLength size = sizeof(address);
        require(getsockname(listener, reinterpret_cast<sockaddr*>(&address), &size) == 0, "Embedding stub port");
        url = "http://127.0.0.1:" + std::to_string(ntohs(address.sin_port));
        worker = std::thread([this] {
            while (!stopped) {
                if (net::waitReadable(listener, 100) != 1) continue;
                auto client = accept(listener, nullptr, nullptr); if (client == net::invalidSocket) continue;
                net::setSocketTimeout(client, SO_RCVTIMEO, 2000);
                std::string request; char buffer[2048]; size_t bodyAt = std::string::npos, length = 0;
                while (request.size() < 100000) {
                    auto n = net::receive(client, buffer, sizeof(buffer)); if (n <= 0) break;
                    request.append(buffer, static_cast<size_t>(n));
                    bodyAt = request.find("\r\n\r\n");
                    if (bodyAt != std::string::npos) {
                        auto at = request.find("Content-Length:");
                        if (at == std::string::npos) break;
                        length = static_cast<size_t>(std::stoul(request.substr(at + 15)));
                        if (request.size() >= bodyAt + 4 + length) break;
                    }
                }
                const std::string body = bodyAt == std::string::npos ? "" : request.substr(bodyAt + 4, length);
                const std::string response = body.find("unrelated") != std::string::npos ? "{\"embeddings\":[[0,1]]}" : "{\"embeddings\":[[1,0]]}";
                ++requests;
                const std::string http = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " + std::to_string(response.size()) + "\r\n\r\n" + response;
                size_t sent = 0;
                while (sent < http.size()) { auto n = net::sendBytes(client, http.data() + sent, http.size() - sent); if (n <= 0) break; sent += static_cast<size_t>(n); }
                net::closeSocket(client);
            }
        });
    }
    ~EmbeddingStub() { stopped = true; if (worker.joinable()) worker.join(); net::closeSocket(listener); }
};
void seedIndex(const std::string& path) {
    sqlite3* db = nullptr; require(sqlite3_open(path.c_str(), &db) == SQLITE_OK, "Open scenario index");
    const char* sql = "INSERT INTO document_chunks(strategy,source,file,title,section,chunk_id,content,content_hash,embedding,embedding_dim,indexed_at,source_type,page,line_start,line_end) VALUES"
        "('structural','project_repo','mcp_manager.cpp','MCP','McpManager::callTool','project:1','Code dispatch documentation','h1',X'0000803F00000000',2,'now','project',0,10,20),"
        "('structural','upload:7','architecture.pdf','Architecture','Option 2 file cache','upload:7:1','Uploaded guide describes option 2, BatchUploader and SQLite as the default.','h2',X'0000803F00000000',2,'now','upload',14,0,0);";
    auto rc = sqlite3_exec(db, sql, nullptr, nullptr, nullptr); sqlite3_close(db); require(rc == SQLITE_OK, "Seed both source types");
}
std::string memoryValue(const Agent& agent, const std::string& key) {
    for (const auto& fact : agent.workingMemoryFacts()) if (fact.key == key) return fact.value;
    return {};
}
struct ScenarioTurn { std::string question, delta; bool rag = true; bool empty = false; };
void runLongScenario(int number, const std::vector<ScenarioTurn>& turns) {
    Fixture fixture(4, 6);
    EnvGuard indexPath("DOCUMENT_INDEX_DB", (fixture.directory / "index.db").string());
    EmbeddingStub stub; EnvGuard ollama("OLLAMA_URL", stub.url);
    document_index::DocumentIndexer index; index.statusJson(); seedIndex((fixture.directory / "index.db").string());
    Agent agent(fixture.config.string()); require(agent.isReady(), agent.initializationError());
    auto chats = agent.chats(); auto chat = std::find_if(chats.begin(), chats.end(), [](const auto& c) { return c.mode == "assistant"; });
    require(chat != chats.end(), "Scenario chat"); std::string error; require(agent.selectChat(chat->id, error), error);
    const std::string summary = "SCENARIO_SUMMARY: Goal build RAG chat; option 1 SQLite cache, option 2 file cache. Documents: architecture.pdf, BatchUploader, McpManager::callTool.";
    int expectedRetrievals = 0;
    for (size_t i = 0; i < turns.size(); ++i) {
        calls.clear(); replies.clear(); const auto& turn = turns[i];
        const std::string oldGoal = memoryValue(agent, "task.goal"), oldConstraints = memoryValue(agent, "task.constraints");
        const std::string previousAnswer = i ? "SCENARIO_ANSWER_" + std::to_string(i) : "";
        std::string rewritten, context, answer; std::vector<Agent::RagSource> sources;
        if (turn.rag) {
            const std::string query = turn.empty ? "unrelated" : "McpManager callTool BatchUploader option 2 file cache";
            replies.push_back("{\"query\":\"" + query + "\"}");
            require(agent.rewriteRetrievalQuery(chat->id, turn.question, rewritten, error), error);
            const auto& rewritePrompt = calls[0].messages;
            if (!oldGoal.empty()) require(rewritePrompt.find(oldGoal) != std::string::npos, "Goal in contextual rewrite");
            if (!oldConstraints.empty()) require(rewritePrompt.find(app_json::jsonEscape(oldConstraints)) != std::string::npos, "Constraints in rewrite");
            if (i && !turns[i-1].empty) require(rewritePrompt.find(previousAnswer) != std::string::npos, "Recent raw turn in rewrite");
            if (i >= 4) require(rewritePrompt.find("SCENARIO_SUMMARY") != std::string::npos && rewritePrompt.find("option 2 file cache") != std::string::npos, "Existing summary loads into rewrite and resolves option reference");
            if (i >= 8) require(rewritePrompt.find(turns[0].question) == std::string::npos, "Old raw messages replaced by summary");
            bool ready = false; require(index.ensureReady(ready, error) && ready, "Reuse existing index");
            std::vector<document_index::SearchHit> candidates;
            require(index.retrieve(rewritten, 10, candidates, error), error); ++expectedRetrievals;
            auto selected = rag::selectCandidates(rewritten, std::move(candidates), rag::Config{});
            require(selected.empty() == turn.empty, "Threshold / relevant scenario sources");
            context = rag::buildContext(selected);
            require(context.find("embedding") == std::string::npos, "No vectors in context");
            for (const auto& h : selected) sources.push_back({h.chunk.file, h.chunk.section, h.chunk.chunkId, h.chunk.sourceType, h.chunk.page, h.chunk.lineStart, h.chunk.lineEnd, h.relevanceScore});
            if (!turn.empty) require(sources.size() == 2 && context.find("architecture.pdf") != std::string::npos && context.find("mcp_manager.cpp") != std::string::npos, "Both project and uploaded sources");
        }
        if (!turn.rag || !turn.empty) {
            std::string draft = "SCENARIO_ANSWER_" + std::to_string(i + 1);
            if (turn.rag) {
                std::string cited = "[";
                for (const auto& source : sources) { if (cited.size() > 1) cited += ','; cited += "{\"chunk_id\":\"" + source.chunkId + "\"}"; }
                draft = "{\"answer\":\"" + draft + "\",\"sources\":" + cited + "]}";
            }
            replies.push_back(draft); replies.push_back("{\"accepted\":true}");
        }
        if ((i + 1) % 4 == 0) replies.push_back(summary);
        replies.push_back(turn.delta);
        const size_t finalCall = calls.size();
        require(agent.handleChatMessage(chat->id, turn.question, answer, error, context, sources, turn.rag), error);
        require(replies.empty() && agent.warnings().empty(), "All scripted phases completed without warning");
        const auto& prompt = calls[finalCall].messages;
        app_json::JsonValue json; require(app_json::JsonParser(prompt).parse(json, error), error);
        if (!turn.empty) require(json.array.back().text.empty() && json.array.back().member("content") && json.array.back().member("content")->text == turn.question, "Final question unchanged");
        if (!oldGoal.empty()) require(prompt.find(oldGoal) != std::string::npos, "Working task state in answer");
        if (i >= 4) require(prompt.find("SCENARIO_SUMMARY") != std::string::npos, "Summary in final context");
        if (!context.empty()) {
            const auto workAt = prompt.find("Current working task state"); const auto ragAt = prompt.find("retrieved_chunks");
            require((oldGoal.empty() || (workAt != std::string::npos && workAt < ragAt)) && prompt.find("user constraints") != std::string::npos, "Context order and document constraints isolation");
        } else require(prompt.find("retrieved_chunks") == std::string::npos, "No chunks when OFF or no relevant results");
        if (turn.empty) require(answer == rag::dontKnowAnswer() && calls.size() == finalCall + 1, "No-source refusal bypasses answer generation");
        const auto& last = agent.visibleConversation().back();
        require(last.ragEnabled == turn.rag && last.ragSources.size() == sources.size(), "Sources state survives history rerender");
        require(last.ragQuotes.size() == sources.size(), "Backend quotes persisted with every cited source");
        app_json::JsonValue state;
        require(app_json::JsonParser("{" + buildAgentStateFields(agent) + "}").parse(state, error), error);
        const auto* conversation = state.member("conversation");
        require(conversation && !conversation->array.empty(), "HTTP conversation payload");
        const auto& rendered = conversation->array.back();
        require(rendered.member("rag_enabled") && rendered.member("rag_enabled")->boolean == turn.rag &&
                rendered.member("rag_sources") && rendered.member("rag_sources")->array.size() == sources.size(),
                "HTTP Sources metadata includes enabled/empty state for UI");
        if (!sources.empty()) {
            auto uploaded = std::find_if(last.ragSources.begin(), last.ragSources.end(), [](const auto& s) { return s.sourceType == "upload"; });
            require(uploaded != last.ragSources.end() && uploaded->page == 14 && uploaded->chunkId == "upload:7:1", "Upload source metadata");
        }
        require(stub.requests == expectedRetrievals, "One retrieval per enabled question, none when OFF");
        if ((i + 1) % 4 == 0) require(agent.rawHistoryMessageCount() == 6 && agent.hasConversationSummary(), "Summary retains N raw messages");
    }
    require(memoryValue(agent, "task.goal") == "Production RAG chat", "Final goal retained/updated");
    require(memoryValue(agent, "task.constraints").find("No SQLite") != std::string::npos && memoryValue(agent, "task.constraints").find("Use SQLite") == std::string::npos, "Superseded constraint removed");
    require(memoryValue(agent, "task.clarifications").find("option 2") != std::string::npos && memoryValue(agent, "task.open_questions").empty(), "Clarifications retained and questions resolved");
    // Persisted work memory survives reopening and remains isolated by chat ID.
    { Agent reopened(fixture.config.string()); require(reopened.isReady() && reopened.selectChat(chat->id, error), error);
      require(memoryValue(reopened, "task.goal") == "Production RAG chat", "Load existing work state from SQLite");
      auto taskChat = std::find_if(chats.begin(), chats.end(), [](const auto& c) { return c.mode == "task"; });
      require(taskChat != chats.end() && reopened.selectChat(taskChat->id, error) && memoryValue(reopened, "task.goal").empty(), "Working state chat isolation"); }
    std::cout << "PASS scenario " << number << ": " << turns.size() << " turns, " << expectedRetrievals << " real SQLite/HTTP retrievals, 3 summary updates\n";
}
void testLongScenarios() {
    const std::string unchanged = "{\"task_state\":{},\"working_facts\":[],\"long_term_facts\":[]}";
    std::vector<ScenarioTurn> architecture = {
        {"Build a RAG chat. Keep this as our goal.", "{\"task_state\":{\"goal\":\"RAG chat\"}}"},
        {"Option 1 is SQLite cache; option 2 is file cache.", "{\"task_state\":{\"clarifications\":[\"option 1 SQLite cache; option 2 file cache\"]}}"},
        {"Use SQLite for now, and keep structural chunking.", "{\"task_state\":{\"constraints\":[\"Use SQLite\",\"Structural chunking\"]},\"working_facts\":[{\"key\":\"task.stack\",\"value\":\"SQLite\"}],\"long_term_facts\":[]}"},
        {"Keep the existing Agent and chat.", "{\"task_state\":{\"constraints\":[\"Use SQLite\",\"Structural chunking\",\"Existing Agent and chat\"]}}"},
        {"Call the upload component BatchUploader.", "{\"task_state\":{\"terms\":[\"BatchUploader\",\"bge-m3\"]}}"},
        {"Why is the second option better?", unchanged},
        {"Change the decision: no SQLite. Retain the other constraints.", "{\"task_state\":{\"constraints\":[\"No SQLite\",\"Structural chunking\",\"Existing Agent and chat\"]},\"working_delete_keys\":[\"task.stack\"]}"},
        {"Use that second option, as discussed earlier.", "{\"task_state\":{\"clarifications\":[\"option 1 SQLite cache; option 2 file cache\",\"Selected option 2\"]}}"},
        {"Refine the goal to a production RAG chat.", "{\"task_state\":{\"goal\":\"Production RAG chat\"}}"},
        {"We still need to decide the cache format.", "{\"task_state\":{\"open_questions\":[\"Cache format\"]}}"},
        {"Use JSON files for it; that resolves the format question.", "{\"task_state\":{\"open_questions\":[],\"clarifications\":[\"option 1 SQLite cache; option 2 file cache\",\"Selected option 2 with JSON files\"]}}"},
        {"Now implement the second option as before, without SQLite.", unchanged}
    };
    runLongScenario(1, architecture);
    auto documents = architecture;
    documents[0].question = "Review uploaded architecture.pdf and mcp_manager.cpp for a RAG chat.";
    documents[1].question = "The guide calls option 1 SQLite cache and option 2 file cache. Compare them.";
    documents[4].question = "Name the upload component BatchUploader as the document does.";
    documents[5].question = "How does that component interact with callTool?";
    documents[7].question = "Explain the second approach using the same uploaded guide.";
    documents[9].question = "What does the guide say about an unrelated subject?"; documents[9].empty = true; documents[9].delta = unchanged;
    documents[10].rag = false;
    documents[11].question = "Use those sources again and implement that option under our current constraints.";
    runLongScenario(2, documents);
}
void testInvalidTaskDelta() {
    Fixture fixture; Agent agent(fixture.config.string()); std::string error, answer;
    const auto chats = agent.chats(); const auto chat = std::find_if(chats.begin(), chats.end(), [](const auto& c) { return c.mode == "assistant"; });
    require(chat != chats.end(), "Task delta chat");
    replies = {"accepted", "{\"accepted\":true}", "{\"task_state\":{\"goal\":\"Existing goal\",\"constraints\":[\"No SQLite\",\"No SQLite\"]}}"};
    require(agent.handleChatMessage(chat->id, "Goal is Existing goal, no SQLite", answer, error), error);
    require(memoryValue(agent, "task.constraints") == "[\"No SQLite\"]", "Deduplicate category snapshot");
    for (const auto& invalid : {"{\"task_state\":{\"goal\":\"Bad goal\",\"constraints\":42}}",
                               "{\"task_state\":{\"unknown\":[\"unexpected\"]}}"}) {
        replies = {"accepted", "{\"accepted\":true}", invalid};
        require(agent.handleChatMessage(chat->id, "Unchanged question", answer, error), error);
        require(!agent.warnings().empty() && memoryValue(agent, "task.goal") == "Existing goal" && memoryValue(agent, "task.constraints") == "[\"No SQLite\"]", "Invalid delta retains entire current state");
    }
}
// Optional manual control run against the existing index and installed Ollama.
// All answer/rewrite calls still use this executable's explicitly scripted LLM.
void testGroundedAgentGuards() {
    Fixture fixture; Agent agent(fixture.config.string()); std::string error, answer;
    auto chats = agent.chats(); auto assistant = std::find_if(chats.begin(),chats.end(),[](const auto& c) { return c.mode == "assistant"; });
    auto task = std::find_if(chats.begin(),chats.end(),[](const auto& c) { return c.mode == "task"; });
    require(assistant != chats.end() && task != chats.end(), "Two existing chat modes");
    auto evidence = hit(.8,"dispatch","dispatch();"); evidence.chunk.file = "dispatch.cpp"; evidence.chunk.chunkId = "dispatch:1"; evidence.relevanceScore = .8;
    const auto context = rag::buildContext({evidence});
    const std::string memory = "{\"working_facts\":[],\"long_term_facts\":[],\"task_state\":{}}";
    const std::string valid = "{\"answer\":\"Calls dispatch().\",\"sources\":[{\"chunk_id\":\"dispatch:1\"}]}";
    calls.clear(); replies = {memory};
    require(agent.handleChatMessage(assistant->id,"Unanswerable",answer,error,{}, {},true),error);
    require(answer == rag::dontKnowAnswer() && calls.size() == 1 && agent.visibleConversation().back().ragQuotes.empty(), "No-context answer is backend-only; memory phase remains intact");
    for (const auto& invalid : {"{\"answer\":\"Invented fact\",\"sources\":[{\"chunk_id\":\"fake\"}]}",
                               "{\"answer\":\"Invented quote\",\"sources\":[{\"chunk_id\":\"dispatch:1\"}],\"quotes\":[{\"chunk_id\":\"dispatch:1\",\"text\":\"not in source\"}]}"}) {
        calls.clear(); replies = {invalid,memory};
        require(agent.handleChatMessage(assistant->id,"Explain dispatch",answer,error,context,{},true),error);
        require(answer == rag::dontKnowAnswer() && agent.visibleConversation().back().ragSources.empty() && agent.visibleConversation().back().ragQuotes.empty() && calls.size() == 2, "Invalid source/quote cannot reach confirmed UI");
    }
    calls.clear(); replies = {valid,"{\"accepted\":false,\"revised_answer\":{\"answer\":\"Fake revision\",\"sources\":[{\"chunk_id\":\"not-retrieved\"}]}}",memory};
    require(agent.handleChatMessage(assistant->id,"Explain dispatch again",answer,error,context,{},true),error);
    require(answer == rag::dontKnowAnswer() && agent.visibleConversation().back().ragSources.empty(), "Output-policy revisions revalidate grounding");
    calls.clear(); replies = {valid,"{\"accepted\":false,\"revised_answer\":{\"answer\":\"dispatch(); is called\",\"sources\":[{\"chunk_id\":\"dispatch:1\"}]}}",memory};
    require(agent.handleChatMessage(assistant->id,"Explain dispatch once more",answer,error,context,{},true),error);
    require(answer == "dispatch(); is called" && agent.visibleConversation().back().ragQuotes[0].text == "dispatch();", "Supported revision retains backend evidence");
    calls.clear(); replies = {"ordinary answer","{\"accepted\":true}",memory};
    require(agent.handleChatMessage(assistant->id,"Implement a task normally",answer,error),error);
    require(answer == "ordinary answer" && !calls[0].json && calls[0].messages.find("GROUNDED RAG MODE") == std::string::npos && !agent.visibleConversation().back().ragEnabled, "RAG OFF and assistant task requests use the ordinary flow");
    calls.clear(); replies = {memory};
    require(agent.handleChatMessage(task->id,"What is dispatch?",answer,error,{}, {},true),error);
    require(answer == rag::dontKnowAnswer() && agent.taskState().plan.empty(), "No evidence does not invent or transition a task plan");
    const std::string plan = "## Goal\nExplain dispatch\n## Scope and constraints\nUse provided code\n## Execution steps\n1. Describe dispatch();\n## Validation criteria\nCheck against quoted source";
    replies = {"{\"answer\":\"" + app_json::jsonEscape(plan) + "\",\"sources\":[{\"chunk_id\":\"dispatch:1\"}]}","{\"accepted\":true}",memory};
    require(agent.handleChatMessage(task->id,"What is dispatch?",answer,error,context,{},true),error);
    require(agent.taskState().plan == plan && agent.visibleConversation().back().ragQuotes.size() == 1, "Task mode keeps persisted planning with grounded JSON");
    replies = {plan,"{\"accepted\":true}",memory};
    require(agent.handleChatMessage(task->id,"continue",answer,error),error);
    require(agent.taskState().state == "PLANNING" && !agent.taskState().plan.empty(), "Task wording is phase input, not an intent classifier or approval");
    replies = {"Paused project summary"}; require(agent.pauseTask(error),error);
    calls.clear(); replies.clear();
    const auto count = agent.completedRequestCount();
    require(!agent.handleChatMessage(task->id,"Question while paused",answer,error,{}, {},true) &&
            agent.taskState().state == "PAUSED" && agent.completedRequestCount() == count && calls.empty(),
            "No-context RAG cannot bypass Task State Machine pause");
}

int runControlQuestions(const std::string& inputPath, const std::string& outputPath) {
    using Json = app_json::JsonValue;
    std::ifstream file(inputPath); std::string input((std::istreambuf_iterator<char>(file)), {}), error;
    Json fixtureJson; require(app_json::JsonParser(input).parse(fixtureJson,error), error);
    const auto* questions = fixtureJson.member("questions"); require(questions && questions->array.size() == 10, "Ten control questions");
    Fixture fixture; Agent agent(fixture.config.string()); require(agent.isReady(),agent.initializationError());
    EnvGuard model("OLLAMA_EMBED_MODEL","bge-m3");
    EnvGuard endpoint("OLLAMA_URL","http://127.0.0.1:11434");
    auto chats = agent.chats(); const auto chat = std::find_if(chats.begin(),chats.end(),[](const auto& c) { return c.mode == "assistant"; });
    require(chat != chats.end(), "Control chat");
    document_index::DocumentIndexer index; rag::Config config;
    require(rag::Config::load(std::string(PROJECT_SOURCE_ROOT) + "/rag_config.json",config,error),error);
    std::string report = "{\"llm_transport\":\"mock/scripted (no live OpenAI call)\",\"embedding_transport\":\"installed local Ollama bge-m3\",\"index\":\"existing document_index.db\",\"questions\":[";
    int passed = 0;
    for (const auto& question : questions->array) {
        calls.clear(); replies.clear();
        const auto text = question.member("question")->text, query = question.member("retrieval_query")->text;
        const bool unknown = question.member("expected_behavior")->text == "dont_know";
        replies.push_back("{\"query\":\"" + app_json::jsonEscape(query) + "\"}");
        std::string rewritten, answer;
        require(agent.rewriteRetrievalQuery(chat->id,text,rewritten,error),error);
        std::vector<document_index::SearchHit> candidates;
        require(index.retrieve(rewritten,config.preFilterTopK,candidates,error),error);
        auto selected = rag::selectCandidates(rewritten,std::move(candidates),config);
        std::vector<Agent::RagSource> sources;
        for (const auto& h : selected) sources.push_back({h.chunk.file,h.chunk.section,h.chunk.chunkId,h.chunk.sourceType,h.chunk.page,h.chunk.lineStart,h.chunk.lineEnd,h.relevanceScore});
        if (!selected.empty()) {
            std::string modelAnswer = "";
            for (const auto& fact : question.member("expected_facts")->array) modelAnswer += fact.text + "\n";
            std::string cited = "[";
            if (!unknown) for (const auto& id : question.member("expected_chunk_ids")->array) {
                if (cited.size() > 1) cited += ',';
                cited += "{\"chunk_id\":\"" + app_json::jsonEscape(id.text) + "\"}";
            }
            replies.push_back("{\"answer\":\"" + app_json::jsonEscape(modelAnswer) + "\",\"sources\":" + cited + "],\"insufficient_context\":" + (unknown ? "true" : "false") + "}");
            if (!unknown) replies.push_back("{\"accepted\":true}");
        }
        replies.push_back("{\"working_facts\":[],\"long_term_facts\":[],\"task_state\":{}}");
        const auto context = rag::buildContext(selected);
        require(agent.handleChatMessage(chat->id,text,answer,error,context,sources,true),error);
        // A rejected scripted draft bypasses the review, which must not become a memory reply.
        const auto& result = agent.visibleConversation().back();
        rag::GroundingContext evidence; require(rag::readGroundingContext(context,evidence,error),error);
        bool ok = rag::validateEvidence({answer,result.ragSources,result.ragQuotes},evidence);
        if (unknown) ok = ok && answer == rag::dontKnowAnswer() && result.ragSources.empty() && result.ragQuotes.empty();
        else {
            ok = ok && !result.ragSources.empty() && !result.ragQuotes.empty();
            std::string quotes; for (const auto& q : result.ragQuotes) quotes += q.text;
            for (const auto& fact : question.member("expected_facts")->array)
                ok = ok && answer.find(fact.text) != answer.npos && quotes.find(fact.text) != quotes.npos;
            for (const auto& id : question.member("expected_chunk_ids")->array)
                ok = ok && std::any_of(result.ragSources.begin(),result.ragSources.end(),[&](const auto& s) { return s.chunkId == id.text; });
        }
        if (ok) ++passed;
        std::string fields = app_json::serializeJson(question); fields.pop_back();
        if (report.back() != '[') report += ',';
        report += fields + ",\"actual_answer\":\"" + app_json::jsonEscape(answer) + "\",\"actual_sources\":" + serializeRagSources(result.ragSources) +
            ",\"actual_quotes\":" + serializeRagQuotes(result.ragQuotes) + ",\"retrieved_candidates_after_filter\":" + serializeRagSources(sources) +
            ",\"backend_skipped_answer_generation\":" + (selected.empty() ? "true" : "false") + ",\"passed\":" + (ok ? "true" : "false") + "}";
        std::cout << "Control " << question.member("id")->text << ": " << (ok ? "PASS" : "FAIL") << ", retained chunks=" << selected.size() << '\n';
        // Start each scripted turn clean even when intentionally testing rejection.
        replies.clear();
    }
    report += "],\"passed\":" + std::to_string(passed) + ",\"total\":10}";
    std::ofstream output(outputPath,std::ios::binary); output << report; require(static_cast<bool>(output),"Write control report");
    return passed == 10 ? 0 : 1;
}
}
ApiClient::ApiClient() : initialized_(true) {}
ApiClient::~ApiClient() = default;
bool ApiClient::isReady() const { return initialized_; }
bool ApiClient::sendChatCompletion(const std::string&, const std::string&, const std::string& messages,
    std::string& answer, std::string& error, bool json, ApiTokenUsage* usage, std::string* finish,
    const std::string& tools, ApiChatResponse* completion) const {
    calls.push_back({messages, tools, json});
    if (replies.empty()) { error = "Unscripted LLM call"; return false; }
    answer = replies.front(); replies.pop_front(); error.clear();
    if (usage) *usage = {}; if (finish) *finish = "stop";
    if (completion) { *completion = {}; completion->content = answer; }
    return true;
}
int main(int argc, char** argv) {
    const char* oldKey = std::getenv("OPENAI_API_KEY"); const std::string savedKey = oldKey ? oldKey : "";
    _putenv_s("OPENAI_API_KEY", "rag-test-key");
    int result = 0;
    try {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        if (argc == 4 && std::string(argv[1]) == "--control") result = runControlQuestions(argv[2],argv[3]);
        else { testConfig(); testThresholdAndEmpty(); testRerankAndMetadata(); testLimits(); testRewriteAndFinalQuestion(); testLongScenarios(); testInvalidTaskDelta(); testGroundedAgentGuards();
            std::cout << "PASS: config, threshold/empty, reranking/metadata, rewrite, memory, grounded generation and revisions, task/assistant modes\n"; }
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; result = 1; }
    _putenv_s("OPENAI_API_KEY", savedKey.c_str());
    curl_global_cleanup();
    return result;
}
