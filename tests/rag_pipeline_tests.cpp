#include "agent.h"
#include "rag_retrieval.h"
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

namespace {
struct Call { std::string messages, tools; bool json; };
std::vector<Call> calls;
std::deque<std::string> replies;
void require(bool ok, const std::string& message) { if (!ok) throw std::runtime_error(message); }
struct Fixture {
    std::filesystem::path directory, config;
    Fixture() {
        directory = std::filesystem::temp_directory_path() / ("rag-day23-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directory(directory);
        config = directory / "agent.json";
        std::ofstream(config) << "{\"model\":\"mock-model\",\"base_instruction\":\"TEST_BASE\",\"input_policy\":\"TEST_INPUT\",\"output_policy\":\"TEST_OUTPUT\",\"short_term_memory_messages\":5,\"summary_every_requests\":100,\"input_price_per_million\":0,\"cached_input_price_per_million\":0,\"output_price_per_million\":0,\"long_term_memory_db\":\"" << (directory / "memory.db").generic_string() << "\",\"project_invariants_db\":\"" << (directory / "invariants.db").generic_string() << "\"}";
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
    enqueueAnswer();
    Agent::RagSource source{"manual.pdf", "Dispatch", "upload:42", "upload", 3, 0, 0, .72};
    const std::string context = "{\"file\":\"manual.pdf\",\"section\":\"Dispatch\",\"page\":3,\"text\":\"Dispatch documentation\"}";
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
    enqueueAnswer();
    require(agent.handleChatMessage(chat->id, "Where are reminders scheduled?", answer, error), error);
    require(calls.size() == 4 && agent.visibleConversation().back().ragSources.empty() && calls[1].messages.find("ReminderScheduler") == std::string::npos, "Empty RAG result answers original without unrelated context");
    calls.clear(); replies = {"{\"query\":\"\"}"};
    auto count = agent.visibleConversation().size();
    require(!agent.rewriteRetrievalQuery(chat->id, original, rewritten, error) && agent.visibleConversation().size() == count, "Invalid rewrite fails without new turn");
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
int main() {
    const char* oldKey = std::getenv("OPENAI_API_KEY"); const std::string savedKey = oldKey ? oldKey : "";
    _putenv_s("OPENAI_API_KEY", "rag-test-key");
    int result = 0;
    try {
        testConfig(); testThresholdAndEmpty(); testRerankAndMetadata(); testLimits(); testRewriteAndFinalQuestion();
        std::cout << "PASS: config, threshold/empty, reranking/metadata, candidate limits, rewrite/original question/ordinary flow\n";
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; result = 1; }
    _putenv_s("OPENAI_API_KEY", savedKey.c_str());
    return result;
}
