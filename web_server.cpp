#include "web_server.h"

#include "agent.h"

#include <cctype>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

namespace {
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
        if (count <= 0 || raw.size() > 1024 * 1024) return false;
        raw.append(buffer, static_cast<size_t>(count));
    }
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
        if (name == "content-length") contentLength = std::stoul(line.substr(colon + 1));
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
    std::cout << "Open http://127.0.0.1:8080 in your browser\nPress Ctrl+C to stop the server\n";
    while (true) {
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
            else if (!agent.respond(prompt, answer, error)) sendHttpResponse(client, 500, "application/json", "{\"error\":\"" + jsonEscape(error) + "\"}");
            else sendHttpResponse(client, 200, "application/json", "{\"model\":\"" + jsonEscape(agent.modelName()) + "\",\"answer\":\"" + jsonEscape(answer) + "\"}");
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
#endif
}
