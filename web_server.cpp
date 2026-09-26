#include "web_server.h"

#include "agent.h"
#include "mcp_client.h"
#include "reminder_store.h"
#include "reminder_scheduler.h"
#include "reminder_events.h"
#include "socket_platform.h"

#include <cctype>
#include <atomic>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <sstream>
#include <string>

#ifndef _WIN32
#include <csignal>
#endif

namespace {
constexpr size_t kMaxHttpHeaderBytes = 64 * 1024;
constexpr size_t kMaxHttpBodyBytes = 1024 * 1024;

#ifdef _WIN32
std::atomic<bool> gStopRequested{false};

BOOL WINAPI handleConsoleSignal(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT ||
        signal == CTRL_CLOSE_EVENT || signal == CTRL_SHUTDOWN_EVENT) {
        gStopRequested.store(true);
        return TRUE;
    }
    return FALSE;
}
bool stopRequested() { return gStopRequested.load(); }
#else
volatile std::sig_atomic_t gStopRequested = 0;
void handlePosixSignal(int) { gStopRequested = 1; }
bool stopRequested() { return gStopRequested != 0; }
#endif

class ServerSignals {
public:
    ServerSignals() {
#ifdef _WIN32
        gStopRequested.store(false);
        SetConsoleCtrlHandler(handleConsoleSignal, TRUE);
#else
        gStopRequested = 0;
        struct sigaction action{};
        action.sa_handler = handlePosixSignal;
        sigemptyset(&action.sa_mask);
        installedInt_ = sigaction(SIGINT, &action, &previousInt_) == 0;
        if (installedInt_) installedTerm_ = sigaction(SIGTERM, &action, &previousTerm_) == 0;
        ready_ = installedInt_ && installedTerm_;
#endif
    }
    ~ServerSignals() {
#ifdef _WIN32
        SetConsoleCtrlHandler(handleConsoleSignal, FALSE);
#else
        if (installedInt_) sigaction(SIGINT, &previousInt_, nullptr);
        if (installedTerm_) sigaction(SIGTERM, &previousTerm_, nullptr);
#endif
    }
    bool ready() const { return ready_; }
private:
    bool ready_ = true;
#ifndef _WIN32
    struct sigaction previousInt_{}, previousTerm_{};
    bool installedInt_ = false, installedTerm_ = false;
#endif
};

bool validWebSocketOrigin(std::string origin, std::string host) {
    if (origin.empty()) return true; // Non-browser clients do not send Origin.
    for (char& c : origin) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
#ifdef _WIN32
    (void)host;
    return origin == "http://127.0.0.1:8080" || origin == "http://localhost:8080";
#else
    // The UI may be opened via the VPS IP or DNS name, not only localhost.
    if (host.empty() || host.find_first_of("/\\?#@ \t\r\n") != std::string::npos) return false;
    for (char& c : host) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return origin == "http://" + host;
#endif
}

std::string jsonEscape(const std::string& text) {
    std::string result;
    for (unsigned char c : text) {
        switch (c) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default: result += c < 0x20 ? "?" : std::string(1, static_cast<char>(c));
        }
    }
    return result;
}

bool readJsonString(const std::string& json, size_t& start, std::string& result) {
    result.clear();
    if (start >= json.size() || json[start++] != '"') return false;
    const auto readHex4 = [&json](size_t& position, unsigned& code) {
        if (json.size() - position < 4) return false;
        code = 0;
        for (size_t digitIndex = 0; digitIndex < 4; ++digitIndex) {
            const unsigned char c = static_cast<unsigned char>(json[position++]);
            unsigned digit = 0;
            if (c >= '0' && c <= '9') digit = c - '0';
            else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
            else return false;
            code = code * 16 + digit;
        }
        return true;
    };
    const auto appendUtf8 = [&result](unsigned code) {
        if (code <= 0x7F) result += static_cast<char>(code);
        else if (code <= 0x7FF) {
            result += static_cast<char>(0xC0 | (code >> 6));
            result += static_cast<char>(0x80 | (code & 0x3F));
        } else if (code <= 0xFFFF) {
            result += static_cast<char>(0xE0 | (code >> 12));
            result += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            result += static_cast<char>(0x80 | (code & 0x3F));
        } else {
            result += static_cast<char>(0xF0 | (code >> 18));
            result += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
            result += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            result += static_cast<char>(0x80 | (code & 0x3F));
        }
    };
    while (start < json.size()) {
        const unsigned char c = static_cast<unsigned char>(json[start++]);
        if (c == '"') return true;
        if (c < 0x20) return false;
        if (c != '\\') { result += static_cast<char>(c); continue; }
        if (start >= json.size()) return false;
        switch (json[start++]) {
        case '"': result += '"'; break;
        case '\\': result += '\\'; break;
        case '/': result += '/'; break;
        case 'b': result += '\b'; break;
        case 'f': result += '\f'; break;
        case 'n': result += '\n'; break;
        case 'r': result += '\r'; break;
        case 't': result += '\t'; break;
        case 'u': {
            unsigned code = 0;
            if (!readHex4(start, code)) return false;
            if (code >= 0xD800 && code <= 0xDBFF) {
                if (json.size() - start < 6 || json[start] != '\\' || json[start + 1] != 'u') return false;
                start += 2;
                unsigned low = 0;
                if (!readHex4(start, low) || low < 0xDC00 || low > 0xDFFF) return false;
                code = 0x10000 + ((code - 0xD800) << 10) + low - 0xDC00;
            } else if (code >= 0xDC00 && code <= 0xDFFF) return false;
            appendUtf8(code);
            break;
        }
        default: return false;
        }
    }
    return false;
}

bool extractJsonStringField(const std::string& json, const std::string& field,
                            std::string& value, bool* present = nullptr) {
    value.clear();
    if (present) *present = false;
    size_t position = 0;
    int depth = 0;
    while (position < json.size()) {
        const char c = json[position];
        if (c == '{' || c == '[') { ++depth; ++position; continue; }
        if (c == '}' || c == ']') { --depth; ++position; continue; }
        if (c != '"') { ++position; continue; }
        std::string text;
        if (!readJsonString(json, position, text)) return false;
        size_t separator = position;
        while (separator < json.size() &&
               std::isspace(static_cast<unsigned char>(json[separator]))) ++separator;
        if (depth != 1 || text != field || separator >= json.size() ||
            json[separator] != ':') continue;
        if (present) *present = true;
        position = separator + 1;
        while (position < json.size() &&
               std::isspace(static_cast<unsigned char>(json[position]))) ++position;
        return readJsonString(json, position, value);
    }
    return false;
}

[[maybe_unused]] std::string extractJsonStringField(const std::string& json,
                                                   const std::string& field) {
    std::string value;
    return extractJsonStringField(json, field, value) ? value : std::string{};
}

std::string buildAgentStateFields(const Agent& agent) {
    const Agent::TokenStatistics& usage = agent.tokenStatistics();
    const Agent::CostStatistics cost = agent.costStatistics();
    const auto usd = [](double value) {
        std::ostringstream stream;
        stream << std::fixed << std::setprecision(8) << value;
        return stream.str();
    };
    std::string warningsJson = "[";
    bool first = true;
    for (const auto& warning : agent.warnings()) {
        if (!first) warningsJson += ',';
        first = false;
        warningsJson += "\"" + jsonEscape(warning) + "\"";
    }
    warningsJson += ']';

    const auto factsJson = [](const std::vector<LongTermMemoryFact>& facts) {
        std::string result = "[";
        bool firstFact = true;
        for (const auto& fact : facts) {
            if (!firstFact) result += ',';
            firstFact = false;
            result += "{\"key\":\"" + jsonEscape(fact.key) +
                      "\",\"value\":\"" + jsonEscape(fact.value) + "\"}";
        }
        return result + ']';
    };
    const auto invariantsJson = [](const std::vector<ProjectInvariant>& invariants) {
        std::string result = "[";
        bool firstInvariant = true;
        for (const auto& invariant : invariants) {
            if (!firstInvariant) result += ',';
            firstInvariant = false;
            result += "{\"key\":\"" + jsonEscape(invariant.key) +
                      "\",\"value\":\"" + jsonEscape(invariant.value) +
                      "\",\"description\":\"" + jsonEscape(invariant.description) + "\"}";
        }
        return result + ']';
    };

    std::string chatsJson = "[";
    first = true;
    for (const auto& chat : agent.chats()) {
        if (!first) chatsJson += ',';
        first = false;
        chatsJson += "{\"id\":\"" + jsonEscape(chat.id) +
                     "\",\"name\":\"" + jsonEscape(chat.name) + "\"}";
    }
    chatsJson += ']';

    std::string conversationJson = "[";
    first = true;
    for (const auto& message : agent.visibleConversation()) {
        if (!first) conversationJson += ',';
        first = false;
        conversationJson += "{\"id\":\"" + jsonEscape(message.id) +
                            "\",\"role\":\"" + jsonEscape(message.role) +
                            "\",\"content\":\"" + jsonEscape(message.content) +
                            "\",\"short_term\":" +
                            std::string(message.role == "user" ? "true" : "false") +
                            ",\"working\":" + std::string(message.workingSaved ? "true" : "false") +
                            ",\"long_term\":" + std::string(message.longTermSaved ? "true" : "false") + "}";
    }
    conversationJson += ']';

    return
        "\"input_rejected\":" + std::string(agent.inputRejected() ? "true" : "false") +
        ",\"input_suspicious\":" + std::string(agent.inputSuspicious() ? "true" : "false") +
        ",\"warnings\":" + warningsJson +
        ",\"chats\":" + chatsJson +
        ",\"active_chat_id\":\"" + jsonEscape(agent.activeChatId()) + "\"" +
        ",\"active_chat_name\":\"" + jsonEscape(agent.activeChatName()) + "\"" +
        ",\"personalization\":\"" + jsonEscape(agent.personalization()) + "\"" +
        ",\"task_state\":{\"state\":\"" + jsonEscape(agent.taskState().state) +
        "\",\"resume_state\":\"" + jsonEscape(agent.taskState().resumeState) +
        "\",\"plan\":\"" + jsonEscape(agent.taskState().plan) +
        "\",\"validation_report\":\"" + jsonEscape(agent.taskState().validationReport) +
        "\",\"execution_completed\":" + std::string(agent.taskState().executionCompleted ? "true" : "false") +
        ",\"validation_passed\":" + std::string(agent.taskState().validationPassed ? "true" : "false") +
        ",\"paused\":" + std::string(agent.taskState().state == "PAUSED" ? "true" : "false") + "}" +
        ",\"project_summary\":\"" + jsonEscape(agent.projectSummary()) + "\"" +
        ",\"project_invariants\":" + invariantsJson(agent.projectInvariants()) +
        ",\"working_memory\":" + factsJson(agent.workingMemoryFacts()) +
        ",\"long_term_memory\":" + factsJson(agent.longTermMemoryFacts()) +
        ",\"memory\":{\"raw_messages\":" +
        std::to_string(agent.rawHistoryMessageCount()) +
        ",\"raw_message_limit\":" + std::to_string(agent.rawMessageLimit()) +
        ",\"pending_summary_messages\":" + std::to_string(agent.pendingSummaryMessageCount()) +
        ",\"summary_present\":" + std::string(agent.hasConversationSummary() ? "true" : "false") +
        ",\"completed_requests\":" + std::to_string(agent.completedRequestCount()) +
        ",\"summary_every_requests\":" + std::to_string(agent.summaryEveryRequests()) +
        ",\"long_term_facts\":" + std::to_string(agent.longTermFactCount()) +
        ",\"working_facts\":" + std::to_string(agent.workingMemoryFacts().size()) +
        ",\"invariant_facts\":" + std::to_string(agent.projectInvariants().size()) +
        "}" +
        ",\"conversation\":" + conversationJson +
        ",\"usage\":{\"input_tokens\":" + std::to_string(usage.inputTokens) +
        ",\"cached_input_tokens\":" + std::to_string(usage.cachedInputTokens) +
        ",\"output_tokens\":" + std::to_string(usage.outputTokens) +
        ",\"total_tokens\":" + std::to_string(usage.totalTokens) +
        ",\"cost_usd\":{\"input\":" + usd(cost.inputUsd) +
        ",\"output\":" + usd(cost.outputUsd) +
        ",\"total\":" + usd(cost.totalUsd) + "}" +
        ",\"summary\":{\"input_tokens\":" + std::to_string(usage.summaryInputTokens) +
        ",\"cached_input_tokens\":" + std::to_string(usage.summaryCachedInputTokens) +
        ",\"output_tokens\":" + std::to_string(usage.summaryOutputTokens) +
        ",\"total_tokens\":" + std::to_string(usage.summaryTotalTokens) +
        ",\"cost_usd\":{\"total\":" + usd(cost.summaryUsd) + "}}}";
}

std::string buildMcpStateJson(const McpClient& mcpClient,
                              const std::string& requestError = {}) {
    std::string toolsJson = "[";
    bool first = true;
    for (const auto& tool : mcpClient.tools()) {
        if (!first) toolsJson += ',';
        first = false;
        toolsJson += "{\"name\":\"" + jsonEscape(tool.name) +
                     "\",\"description\":\"" + jsonEscape(tool.description) +
                     "\",\"inputSchema\":" +
                     (tool.inputSchemaJson.empty() ? "{}" : tool.inputSchemaJson) + "}";
    }
    toolsJson += ']';
    std::string callsJson = "[";
    first = true;
    for (const auto& call : mcpClient.calls()) {
        if (!first) callsJson += ',';
        first = false;
        callsJson += "{\"name\":\"" + jsonEscape(call.name) +
            "\",\"arguments\":\"" + jsonEscape(call.argumentsJson) +
            "\",\"success\":" + std::string(call.success ? "true" : "false") +
            ",\"result\":" + (call.resultJson.empty() ? "null" : call.resultJson) +
            ",\"error\":\"" + jsonEscape(call.error) + "\"}";
    }
    callsJson += ']';
    return "{\"status\":\"" + jsonEscape(mcpClient.status()) +
           "\",\"server\":{\"name\":\"" + jsonEscape(mcpClient.serverName()) +
           "\",\"url\":\"" + jsonEscape(mcpClient.serverUrl()) +
           "\"},\"tools\":" + toolsJson + ",\"calls\":" + callsJson +
           ",\"error\":\"" + jsonEscape(requestError.empty()
                ? mcpClient.lastError() : requestError) + "\"}";
}

std::string buildReminderStateJson(const ReminderStore& store, const std::string& requestError = {},
                                 const std::vector<ReminderNotification>& notifications = {}) {
    std::vector<Reminder> reminders;
    std::vector<ReminderNotification> unused;
    std::string error;
    store.snapshot(reminders, unused, error);
    if (!requestError.empty()) error = requestError;
    std::string result = "{\"reminders\":[";
    bool first = true;
    for (const auto& reminder : reminders) {
        if (!first) result += ',';
        first = false;
        result += "{\"id\":" + std::to_string(reminder.id) +
            ",\"text\":\"" + jsonEscape(reminder.text) +
            "\",\"run_at\":\"" + reminderUtcTime(reminder.runAt) +
            "\",\"status\":\"" + jsonEscape(reminder.status) +
            "\",\"created_at\":\"" + reminderUtcTime(reminder.createdAt) +
            "\",\"triggered_at\":" + (reminder.triggeredAt
                ? "\"" + reminderUtcTime(reminder.triggeredAt) + "\"" : "null") + "}";
    }
    result += "],\"notifications\":[";
    first = true;
    for (const auto& notification : notifications) {
        if (!first) result += ',';
        first = false;
        result += "{\"id\":" + std::to_string(notification.id) +
            ",\"reminder_id\":" + std::to_string(notification.reminderId) +
            ",\"text\":\"" + jsonEscape(notification.text) +
            "\",\"triggered_at\":\"" + reminderUtcTime(notification.triggeredAt) + "\"}";
    }
    return result + "],\"error\":\"" + jsonEscape(error) + "\"}";
}

bool parseContentLength(const std::string& text, size_t& value) {
    size_t position = 0;
    while (position < text.size() &&
           std::isspace(static_cast<unsigned char>(text[position]))) ++position;
    if (position == text.size() ||
        !std::isdigit(static_cast<unsigned char>(text[position]))) return false;

    size_t parsed = 0;
    while (position < text.size() &&
           std::isdigit(static_cast<unsigned char>(text[position]))) {
        const unsigned digit = static_cast<unsigned>(text[position] - '0');
        if (parsed > (std::numeric_limits<size_t>::max() - digit) / 10) return false;
        parsed = parsed * 10 + digit;
        ++position;
    }
    while (position < text.size() &&
           std::isspace(static_cast<unsigned char>(text[position]))) ++position;
    if (position != text.size() || parsed > kMaxHttpBodyBytes) return false;
    value = parsed;
    return true;
}

struct HttpRequest {
    std::string method; std::string path; std::string body;
    std::map<std::string, std::string> headers;
};

bool sendAll(net::Socket socket, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        const auto count = net::sendBytes(socket, data.data() + sent, data.size() - sent);
        if (count < 0 && net::interruptedSocketError() && !stopRequested()) continue;
        if (count <= 0) return false;
        sent += static_cast<size_t>(count);
    }
    return true;
}

bool readHttpRequest(net::Socket client, HttpRequest& request) {
    std::string raw;
    char buffer[4096];
    size_t headerEnd = std::string::npos;
    while ((headerEnd = raw.find("\r\n\r\n")) == std::string::npos) {
        const auto count = net::receive(client, buffer, sizeof(buffer));
        if (count < 0 && net::interruptedSocketError() && !stopRequested()) continue;
        if (count <= 0 || raw.size() > kMaxHttpHeaderBytes) return false;
        raw.append(buffer, static_cast<size_t>(count));
    }
    if (headerEnd > kMaxHttpHeaderBytes) return false;
    std::istringstream headers(raw.substr(0, headerEnd));
    headers >> request.method >> request.path;
    size_t contentLength = 0;
    std::string line;
    std::getline(headers, line);
    while (std::getline(headers, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string name = line.substr(0, colon);
        for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        std::string value = line.substr(colon + 1);
        const auto begin = value.find_first_not_of(" \t");
        const auto end = value.find_last_not_of(" \t");
        value = begin == std::string::npos ? "" : value.substr(begin, end - begin + 1);
        request.headers[name] = value;
        if (name == "content-length" &&
            !parseContentLength(line.substr(colon + 1), contentLength)) return false;
    }
    request.body = raw.substr(headerEnd + 4);
    while (request.body.size() < contentLength) {
        const auto count = net::receive(client, buffer, sizeof(buffer));
        if (count < 0 && net::interruptedSocketError() && !stopRequested()) continue;
        if (count <= 0) return false;
        request.body.append(buffer, static_cast<size_t>(count));
    }
    request.body.resize(contentLength);
    return true;
}

std::string readFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return file ? std::string(std::istreambuf_iterator<char>(file), {}) : std::string{};
}

void sendHttpResponse(net::Socket client, int status, const std::string& contentType, const std::string& body) {
    const std::string statusText = status == 200 ? "OK" : status == 400 ? "Bad Request" :
        status == 404 ? "Not Found" : status == 502 ? "Bad Gateway" : "Internal Server Error";
    sendAll(client, "HTTP/1.1 " + std::to_string(status) + " " + statusText + "\r\n"
            "Content-Type: " + contentType + "; charset=utf-8\r\n"
            "Content-Length: " + std::to_string(body.size()) + "\r\n"
            "Cache-Control: no-store\r\nConnection: close\r\n\r\n" + body);
}
}

int runWebServer(Agent& agent, McpClient& mcpClient) {
    net::SocketRuntime runtime;
    if (!runtime.ready()) return 1;
    net::Socket server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server == net::invalidSocket) return 1;
    sockaddr_in address{};
    address.sin_family = AF_INET; address.sin_port = htons(8080);
#ifdef _WIN32
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    constexpr const char* bindAddress = "127.0.0.1";
#else
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    constexpr const char* bindAddress = "0.0.0.0";
    const int reuse = 1;
    if (setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0) {
        net::closeSocket(server); return 1;
    }
#endif
    if (bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || listen(server, SOMAXCONN) != 0) {
        std::cerr << "Could not start server on http://" << bindAddress << ":8080\n";
        net::closeSocket(server); return 1;
    }
    ServerSignals signals;
    if (!signals.ready()) { net::closeSocket(server); return 1; }
    ReminderStore reminderStore;
    ReminderEvents reminderEvents;
    std::mutex reminderEventMutex;
    const auto publishReminders = [&] {
        std::lock_guard<std::mutex> lock(reminderEventMutex);
        reminderEvents.broadcast("{\"type\":\"update\"," + buildReminderStateJson(reminderStore).substr(1));
    };
    ReminderScheduler reminderScheduler(reminderStore, [&](const std::vector<ReminderNotification>& triggered) {
        std::lock_guard<std::mutex> lock(reminderEventMutex);
        reminderEvents.broadcast("{\"type\":\"triggered\"," + buildReminderStateJson(reminderStore, {}, triggered).substr(1));
    });
    reminderScheduler.start();
    std::cout << "Listening on http://" << bindAddress << ":8080\n"
              << "Open http://127.0.0.1:8080 locally, or http://<server-ip>:8080 on Linux\n"
              << "Press Ctrl+C to stop the server\n";
    int exitCode = 0;
    while (!stopRequested()) {
        const int ready = net::waitReadable(server, 250);
        if (ready == 0) continue;
        if (ready < 0) {
            if (stopRequested()) break;
            if (net::interruptedSocketError()) continue;
            std::cerr << "Socket select failed: " << net::lastSocketError() << '\n';
            exitCode = 1; break;
        }
        net::Socket client = accept(server, nullptr, nullptr);
        if (client == net::invalidSocket) continue;
        if (!net::setSocketTimeout(client, SO_RCVTIMEO, 10000) ||
            !net::setSocketTimeout(client, SO_SNDTIMEO, 10000)) {
            net::closeSocket(client); continue;
        }
        HttpRequest request;
        if (!readHttpRequest(client, request)) {
            sendHttpResponse(client, 400, "application/json", "{\"error\":\"Invalid request\"}");
        } else if (request.method == "GET" && request.path == "/api/reminders/events") {
            const auto lower = [](std::string value) {
                for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                return value;
            };
            const std::string origin = request.headers["origin"];
            if (lower(request.headers["upgrade"]) != "websocket" ||
                lower(request.headers["connection"]).find("upgrade") == std::string::npos ||
                request.headers["sec-websocket-version"] != "13" ||
                !validWebSocketOrigin(origin, request.headers["host"])) {
                sendHttpResponse(client, 400, "application/json", "{\"error\":\"A same-origin WebSocket connection is required\"}");
            } else {
                std::string error;
                std::lock_guard<std::mutex> lock(reminderEventMutex);
                const std::string state = "{\"type\":\"snapshot\"," + buildReminderStateJson(reminderStore).substr(1);
                if (reminderEvents.addClient(static_cast<std::uintptr_t>(client), request.headers["sec-websocket-key"], state, error)) {
                    // The event stream now owns the socket; the HTTP loop remains available.
                    continue;
                }
                sendHttpResponse(client, 400, "application/json", "{\"error\":\"" + jsonEscape(error) + "\"}");
            }
        } else if (request.method == "GET" && request.path == "/api/reminders") {
            sendHttpResponse(client, 200, "application/json", buildReminderStateJson(reminderStore));
        } else if (request.method == "POST" && request.path == "/api/reminders/delete") {
            std::string idText, error;
            bool success = false;
            if (!extractJsonStringField(request.body, "id", idText) || idText.empty() ||
                idText.find_first_not_of("0123456789") != std::string::npos) {
                error = "A positive reminder ID is required";
            } else {
                try { success = reminderStore.deletePending(std::stoll(idText), error); }
                catch (const std::exception&) { error = "Invalid reminder ID"; }
            }
            if (success) publishReminders();
            sendHttpResponse(client, success ? 200 : 400, "application/json", buildReminderStateJson(reminderStore, error));
        } else if (request.method == "POST" && request.path == "/api/reminders") {
            std::string text, runAt, result, error;
            bool success = false;
            if (!extractJsonStringField(request.body, "text", text) ||
                !extractJsonStringField(request.body, "run_at", runAt)) {
                error = "Reminder text and run_at are required";
            } else if (!reminderStore.isReady()) {
                error = reminderStore.initializationError();
            } else {
                const std::string arguments = "{\"text\":\"" + jsonEscape(text) + "\",\"run_at\":\"" + jsonEscape(runAt) + "\"}";
                success = mcpClient.callTool("create_reminder", arguments, result, error);
            }
            if (success) publishReminders();
            std::string state = buildReminderStateJson(reminderStore, error);
            state.pop_back();
            sendHttpResponse(client, success ? 200 : 400, "application/json",
                state + ",\"mcp\":" + buildMcpStateJson(mcpClient) + "}");
        } else if (request.method == "POST" && request.path == "/api/mcp/connect") {
            std::string error;
            const bool success = mcpClient.connect(error);
            sendHttpResponse(client, success ? 200 : 502, "application/json",
                             buildMcpStateJson(mcpClient));
        } else if (request.method == "POST" && request.path == "/api/mcp/disconnect") {
            std::string error;
            mcpClient.disconnect(error);
            sendHttpResponse(client, 200, "application/json", buildMcpStateJson(mcpClient));
        } else if (request.method == "GET" && request.path == "/api/mcp/tools") {
            bool success = true;
            std::string error;
            if (mcpClient.status() == "Connected") success = mcpClient.refreshTools(error);
            sendHttpResponse(client, success ? 200 : 502, "application/json",
                             buildMcpStateJson(mcpClient));
        } else if (request.method == "POST" && request.path == "/api/mcp/call") {
            std::string name, arguments, error, result;
            if (!extractJsonStringField(request.body, "name", name) || name.empty() ||
                !extractJsonStringField(request.body, "arguments", arguments) || arguments.empty()) {
                sendHttpResponse(client, 400, "application/json",
                    buildMcpStateJson(mcpClient,
                        "Tool name and JSON arguments are required"));
            } else {
                const bool success = mcpClient.callTool(name, arguments, result, error);
                if (success && name == "create_reminder") publishReminders();
                sendHttpResponse(client, 200, "application/json", buildMcpStateJson(mcpClient));
            }
        } else if (request.method == "POST" && request.path == "/api/chat") {
            std::string prompt, chatId;
            bool chatIdPresent = false;
            const bool promptValid = extractJsonStringField(request.body, "message", prompt);
            const bool chatIdValid = extractJsonStringField(request.body, "chat_id", chatId,
                                                           &chatIdPresent);
            std::string answer, error;
            if (!promptValid || prompt.empty()) {
                sendHttpResponse(client, 400, "application/json",
                                 "{\"error\":\"Message is required\"," +
                                     buildAgentStateFields(agent) + "}");
            } else if (chatIdPresent && (!chatIdValid || chatId.empty())) {
                sendHttpResponse(client, 400, "application/json",
                                 "{\"error\":\"A valid chat_id is required\"," +
                                     buildAgentStateFields(agent) + "}");
            } else if (!(chatIdPresent ? agent.handleChatMessage(chatId, prompt, answer, error)
                                     : agent.handleChatMessage(agent.activeChatId(), prompt, answer, error))) {
                sendHttpResponse(client, agent.inputRejected() ? 400 : 500, "application/json",
                                 "{\"error\":\"" + jsonEscape(error) + "\"," +
                                     buildAgentStateFields(agent) + "}");
            }
            else {
                const std::string response =
                    "{\"model\":\"" + jsonEscape(agent.modelName()) +
                    "\",\"answer\":\"" + jsonEscape(answer) +
                    "\"," + buildAgentStateFields(agent) + "}";
                sendHttpResponse(client, 200, "application/json", response);
            }
        } else if (request.method == "POST" &&
                   (request.path == "/api/chats" || request.path == "/api/chats/select" ||
                    request.path == "/api/chats/delete" ||
                    request.path == "/api/personalization" || request.path == "/api/task/plan" ||
                    request.path == "/api/invariants" || request.path == "/api/invariants/update" ||
                    request.path == "/api/invariants/delete" ||
                    request.path == "/api/task/transition" ||
                    request.path == "/api/task/revise" || request.path == "/api/task/approve" ||
                    request.path == "/api/task/validation" || request.path == "/api/task/validate" ||
                    request.path == "/api/task/execution" ||
                    request.path == "/api/task/pause" || request.path == "/api/task/resume")) {
            std::string error;
            bool success = false;
            if (request.path == "/api/chats") {
                std::string name;
                if (!extractJsonStringField(request.body, "name", name) || name.empty()) {
                    error = "Chat name is required";
                } else success = agent.createChat(name, error);
            } else if (request.path == "/api/chats/select") {
                std::string chatId;
                if (!extractJsonStringField(request.body, "chat_id", chatId) || chatId.empty()) {
                    error = "chat_id is required";
                } else success = agent.selectChat(chatId, error);
            } else if (request.path == "/api/chats/delete") {
                std::string chatId;
                if (!extractJsonStringField(request.body, "chat_id", chatId) || chatId.empty()) {
                    error = "chat_id is required";
                } else success = agent.deleteChat(chatId, error);
            } else if (request.path == "/api/personalization") {
                std::string text;
                if (!extractJsonStringField(request.body, "text", text)) {
                    error = "Personalization text field is required";
                } else success = agent.setPersonalization(text, error);
            } else if (request.path == "/api/invariants" ||
                       request.path == "/api/invariants/update" ||
                       request.path == "/api/invariants/delete") {
                std::string projectId, key;
                if (!extractJsonStringField(request.body, "project_id", projectId) || projectId.empty() ||
                    !extractJsonStringField(request.body, "key", key) || key.empty()) {
                    error = "project_id and key are required";
                } else if (request.path == "/api/invariants/delete") {
                    success = agent.deleteInvariant(projectId, key, error);
                } else {
                    ProjectInvariant invariant;
                    invariant.key = key;
                    if (!extractJsonStringField(request.body, "value", invariant.value) ||
                        !extractJsonStringField(request.body, "description", invariant.description)) {
                        error = "value and description are required";
                    } else if (request.path == "/api/invariants") {
                        success = agent.createInvariant(projectId, invariant, error);
                    } else {
                        std::string currentKey;
                        if (!extractJsonStringField(request.body, "current_key", currentKey) || currentKey.empty()) {
                            error = "current_key is required";
                        } else success = agent.updateInvariant(projectId, currentKey, invariant, error);
                    }
                }
            } else {
                std::string chatId;
                if (!extractJsonStringField(request.body, "chat_id", chatId) || chatId.empty()) {
                    error = "chat_id is required";
                } else if (!agent.selectChat(chatId, error)) {
                    success = false;
                } else if (request.path == "/api/task/transition") {
                    std::string action;
                    if (!extractJsonStringField(request.body, "action", action) || action.empty()) {
                        error = "action is required";
                    } else {
                        success = agent.performTaskAction(action, error);
                    }
                } else if (request.path == "/api/task/plan") {
                    std::string taskRequest, plan;
                    if (!extractJsonStringField(request.body, "task_request", taskRequest) ||
                        taskRequest.empty()) error = "task_request is required";
                    else success = agent.handleChatMessage(chatId, taskRequest, plan, error);
                } else if (request.path == "/api/task/revise") {
                    std::string feedback, plan;
                    if (!extractJsonStringField(request.body, "feedback", feedback)) {
                        error = "feedback field is required";
                    } else {
                        if (feedback.empty()) {
                            feedback = "Переделай план: сделай его яснее, полнее и добавь проверяемые шаги.";
                        }
                        success = agent.handleChatMessage(chatId, feedback, plan, error);
                    }
                } else if (request.path == "/api/task/approve") {
                    success = agent.approveTaskPlan(error);
                } else if (request.path == "/api/task/validation") {
                    success = agent.moveTaskToValidation(error);
                } else if (request.path == "/api/task/validate") {
                    bool passed = false;
                    success = agent.validateTask(passed, error);
                } else if (request.path == "/api/task/execution") {
                    success = agent.returnTaskToExecution(error);
                } else if (request.path == "/api/task/pause") {
                    success = agent.pauseTask(error);
                } else if (request.path == "/api/task/resume") {
                    success = agent.resumeTask(error);
                }
            }
            if (success) {
                sendHttpResponse(client, 200, "application/json", "{" +
                                     buildAgentStateFields(agent) + "}");
            } else {
                sendHttpResponse(client, 400, "application/json", "{\"error\":\"" +
                                     jsonEscape(error) + "\"," +
                                     buildAgentStateFields(agent) + "}");
            }
        } else if (request.method == "GET" && request.path == "/api/state") {
            sendHttpResponse(client, 200, "application/json",
                             "{" + buildAgentStateFields(agent) + "}");
        } else {
            std::string fileName, contentType;
            if (request.method == "GET" && request.path == "/") { fileName = "index.html"; contentType = "text/html"; }
            else if (request.method == "GET" && request.path == "/styles.css") { fileName = "styles.css"; contentType = "text/css"; }
            else if (request.method == "GET" && request.path == "/app.js") { fileName = "app.js"; contentType = "application/javascript"; }
            else if (request.method == "GET" && request.path == "/reminders.js") { fileName = "reminders.js"; contentType = "application/javascript"; }
            const std::string content = fileName.empty() ? "" : readFile(fileName);
            sendHttpResponse(client, content.empty() ? 404 : 200, content.empty() ? "text/plain" : contentType, content.empty() ? "Not found" : content);
        }
        net::closeSocket(client);
    }
    reminderScheduler.stop();
    reminderEvents.stop();
    net::closeSocket(server);
    return exitCode;
}
