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

std::string extractJsonStringField(const std::string& json, const std::string& field) {
    const auto key = json.find("\"" + field + "\"");
    if (key == std::string::npos) return {};
    auto start = json.find(':', key + field.size() + 2);
    if (start == std::string::npos) return {};
    do { ++start; } while (start < json.size() && std::isspace(static_cast<unsigned char>(json[start])));
    if (start >= json.size() || json[start++] != '"') return {};
    std::string result;
    bool escaped = false;
    for (; start < json.size(); ++start) {
        const char c = json[start];
        if (escaped) { result += c == 'n' ? '\n' : c == 'r' ? '\r' : c == 't' ? '\t' : c; escaped = false; }
        else if (c == '\\') escaped = true;
        else if (c == '"') return result;
        else result += c;
    }
    return {};
}

bool extractJsonUnsignedField(const std::string& json, const std::string& field,
                              std::size_t& value) {
    const auto key = json.find("\"" + field + "\"");
    if (key == std::string::npos) return false;
    const auto colon = json.find(':', key + field.size() + 2);
    if (colon == std::string::npos) return false;
    const auto start = json.find_first_not_of(" \t\r\n", colon + 1);
    if (start == std::string::npos ||
        !std::isdigit(static_cast<unsigned char>(json[start]))) return false;
    try {
        value = std::stoull(json.substr(start));
        return true;
    } catch (...) {
        return false;
    }
}

std::string buildAgentStateFields(const Agent& agent) {
    const Agent::TokenStatistics& usage = agent.tokenStatistics();
    const Agent::CostStatistics cost = agent.costStatistics();
    const auto usd = [](double value) {
        std::ostringstream stream;
        stream << std::fixed << std::setprecision(8) << value;
        return stream.str();
    };
    std::string branchesJson = "[";
    bool first = true;
    for (const auto& branch : agent.branches()) {
        if (!first) branchesJson += ',';
        first = false;
        branchesJson += "{\"id\":" + std::to_string(branch.id) +
                        ",\"label\":\"" + jsonEscape(branch.label) +
                        "\",\"active\":" + (branch.active ? "true" : "false") + "}";
    }
    branchesJson += ']';

    std::string conversationJson = "[";
    first = true;
    for (const auto& message : agent.visibleConversation()) {
        if (!first) conversationJson += ',';
        first = false;
        conversationJson += "{\"role\":\"" + jsonEscape(message.role) +
                            "\",\"content\":\"" + jsonEscape(message.content) + "\"}";
    }
    conversationJson += ']';

    return
        "\"strategy\":" + std::to_string(agent.strategy()) +
        ",\"memory\":{\"raw_messages\":" +
        std::to_string(agent.rawHistoryMessageCount()) +
        ",\"long_term_facts\":" + std::to_string(agent.longTermFactCount()) +
        ",\"active_branch_id\":" + std::to_string(agent.activeBranchId()) +
        ",\"branches\":" + branchesJson + "}" +
        ",\"conversation\":" + conversationJson +
        ",\"usage\":{\"input_tokens\":" + std::to_string(usage.inputTokens) +
        ",\"cached_input_tokens\":" + std::to_string(usage.cachedInputTokens) +
        ",\"output_tokens\":" + std::to_string(usage.outputTokens) +
        ",\"total_tokens\":" + std::to_string(usage.totalTokens) +
        ",\"cost_usd\":{\"input\":" + usd(cost.inputUsd) +
        ",\"output\":" + usd(cost.outputUsd) +
        ",\"total\":" + usd(cost.totalUsd) + "}}";
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
    std::string resetError;
    if (!agent.resetAllMemory(resetError)) {
        std::cerr << "Could not reset agent memory: " << resetError << '\n';
        closesocket(server);
        WSACleanup();
        return 1;
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
            const std::string prompt = extractJsonStringField(request.body, "message");
            std::string answer, error;
            if (prompt.empty()) sendHttpResponse(client, 400, "application/json", "{\"error\":\"Message is required\"}");
            else if (!agent.respond(prompt, answer, error)) {
                sendHttpResponse(client, 500, "application/json",
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
        } else if (request.method == "GET" && request.path == "/api/state") {
            sendHttpResponse(client, 200, "application/json",
                             "{" + buildAgentStateFields(agent) + "}");
        } else if (request.method == "POST" && request.path == "/api/strategy") {
            std::size_t strategy = 0;
            std::string error;
            if (!extractJsonUnsignedField(request.body, "strategy", strategy) ||
                !agent.setStrategy(static_cast<int>(strategy), error)) {
                sendHttpResponse(client, 400, "application/json",
                                 "{\"error\":\"" + jsonEscape(
                                     error.empty() ? "strategy must be 1, 2, or 3" : error) +
                                     "\"}");
            } else {
                sendHttpResponse(client, 200, "application/json",
                                 "{" + buildAgentStateFields(agent) + "}");
            }
        } else if (request.method == "POST" && request.path == "/api/branch") {
            std::size_t branchId = 0;
            std::string error;
            if (!extractJsonUnsignedField(request.body, "branch_id", branchId) ||
                !agent.selectBranch(branchId, error)) {
                sendHttpResponse(client, 400, "application/json",
                                 "{\"error\":\"" + jsonEscape(
                                     error.empty() ? "branch_id is required" : error) + "\"}");
            } else {
                sendHttpResponse(client, 200, "application/json",
                                 "{" + buildAgentStateFields(agent) + "}");
            }
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
