#include "web_server.h"

#include "agent.h"

#include <cctype>
#include <atomic>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
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
#endif

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
        "\",\"plan\":\"" + jsonEscape(agent.taskState().plan) +
        "\",\"validation_report\":\"" + jsonEscape(agent.taskState().validationReport) +
        "\",\"paused\":" + std::string(agent.taskState().paused ? "true" : "false") + "}" +
        ",\"project_summary\":\"" + jsonEscape(agent.projectSummary()) + "\"" +
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

#ifdef _WIN32
struct HttpRequest { std::string method; std::string path; std::string body; };

bool sendAll(SOCKET socket, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        const int count = send(socket, data.data() + sent, static_cast<int>(data.size() - sent), 0);
        if (count == SOCKET_ERROR || count == 0) return false;
        sent += static_cast<size_t>(count);
    }
    return true;
}

bool readHttpRequest(SOCKET client, HttpRequest& request) {
    std::string raw;
    char buffer[4096];
    size_t headerEnd = std::string::npos;
    while ((headerEnd = raw.find("\r\n\r\n")) == std::string::npos) {
        const int count = recv(client, buffer, sizeof(buffer), 0);
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
        if (name == "content-length" &&
            !parseContentLength(line.substr(colon + 1), contentLength)) return false;
    }
    request.body = raw.substr(headerEnd + 4);
    while (request.body.size() < contentLength) {
        const int count = recv(client, buffer, sizeof(buffer), 0);
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

void sendHttpResponse(SOCKET client, int status, const std::string& contentType, const std::string& body) {
    const std::string statusText = status == 200 ? "OK" : status == 400 ? "Bad Request" : status == 404 ? "Not Found" : "Internal Server Error";
    sendAll(client, "HTTP/1.1 " + std::to_string(status) + " " + statusText + "\r\n"
            "Content-Type: " + contentType + "; charset=utf-8\r\n"
            "Content-Length: " + std::to_string(body.size()) + "\r\n"
            "Cache-Control: no-store\r\nConnection: close\r\n\r\n" + body);
}
#endif
}

int runWebServer(Agent& agent) {
#ifndef _WIN32
    std::cerr << "Web mode is currently available on Windows only\n";
    return 1;
#else
    WSADATA wsaData{};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) return 1;
    SOCKET server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server == INVALID_SOCKET) { WSACleanup(); return 1; }
    sockaddr_in address{};
    address.sin_family = AF_INET; address.sin_port = htons(8080); address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR || listen(server, SOMAXCONN) == SOCKET_ERROR) {
        std::cerr << "Could not start server on http://127.0.0.1:8080\n";
        closesocket(server); WSACleanup(); return 1;
    }
    gStopRequested.store(false);
    SetConsoleCtrlHandler(handleConsoleSignal, TRUE);
    std::cout << "Open http://127.0.0.1:8080 in your browser\nPress Ctrl+C to stop the server\n";
    while (!gStopRequested.load()) {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(server, &readSet);
        timeval waitTime{};
        waitTime.tv_usec = 250000;
        const int ready = select(0, &readSet, nullptr, nullptr, &waitTime);
        if (ready == 0) continue;
        if (ready == SOCKET_ERROR) {
            if (gStopRequested.load()) break;
            continue;
        }
        SOCKET client = accept(server, nullptr, nullptr);
        if (client == INVALID_SOCKET) continue;
        const DWORD timeoutMs = 10000;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
        HttpRequest request;
        if (!readHttpRequest(client, request)) {
            sendHttpResponse(client, 400, "application/json", "{\"error\":\"Invalid request\"}");
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
            } else {
                std::string chatId;
                if (!extractJsonStringField(request.body, "chat_id", chatId) || chatId.empty()) {
                    error = "chat_id is required";
                } else if (!agent.selectChat(chatId, error)) {
                    success = false;
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
                    if (success) {
                        bool passed = false;
                        success = agent.validateTask(passed, error);
                    }
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
            const std::string content = fileName.empty() ? "" : readFile(fileName);
            sendHttpResponse(client, content.empty() ? 404 : 200, content.empty() ? "text/plain" : contentType, content.empty() ? "Not found" : content);
        }
        closesocket(client);
    }
    SetConsoleCtrlHandler(handleConsoleSignal, FALSE);
    closesocket(server);
    WSACleanup();
    return 0;
#endif
}
