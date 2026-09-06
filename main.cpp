#include <curl/curl.h>

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#endif

static size_t writeResponse(void* data, size_t size, size_t count, void* userData) {
    static_cast<std::string*>(userData)->append(static_cast<char*>(data), size * count);
    return size * count;
}

static std::string jsonEscape(const std::string& text) {
    std::string result;
    for (unsigned char c : text) {
        switch (c) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\b': result += "\\b"; break;
        case '\f': result += "\\f"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (c < 0x20) result += "?";
            else result += static_cast<char>(c);
        }
    }
    return result;
}

#ifdef _WIN32
static std::string argvToUtf8(const char* text) {
    if (!text || !*text) return {};

    const int wideSize = MultiByteToWideChar(CP_ACP, 0, text, -1, nullptr, 0);
    if (wideSize <= 1) return {};

    std::wstring wide(static_cast<size_t>(wideSize), L'\0');
    MultiByteToWideChar(CP_ACP, 0, text, -1, wide.data(), wideSize);

    const int utf8Size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (utf8Size <= 1) return {};

    std::string result(static_cast<size_t>(utf8Size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, result.data(), utf8Size, nullptr, nullptr);
    result.pop_back();
    return result;
}
#endif

static std::string extractContent(const std::string& json) {
    const auto message = json.find("\"message\"");
    if (message == std::string::npos) return {};

    const auto content = json.find("\"content\"", message);
    if (content == std::string::npos) return {};

    size_t start = content + 9;
    while (start < json.size() && std::isspace(static_cast<unsigned char>(json[start]))) ++start;
    if (start >= json.size() || json[start++] != ':') return {};
    while (start < json.size() && std::isspace(static_cast<unsigned char>(json[start]))) ++start;
    if (start >= json.size() || json[start++] != '"') return {};

    std::string result;
    for (size_t i = start; i < json.size(); ++i) {
        if (json[i] == '"' && json[i - 1] != '\\') break;
        if (json[i] == '\\' && i + 1 < json.size()) {
            const char escaped = json[++i];
            switch (escaped) {
            case 'n': result += '\n'; break;
            case 'r': result += '\r'; break;
            case 't': result += '\t'; break;
            case 'b': result += '\b'; break;
            case 'f': result += '\f'; break;
            default: result += escaped;
            }
        } else {
            result += json[i];
        }
    }
    return result;
}

static bool askOpenAI(const char* apiKey, const std::string& prompt, const std::string& model,
                      std::string& answer, std::string& error) {
    std::string response;
    const std::string body =
        "{\"model\":\"" + jsonEscape(model) +
        "\",\"messages\":[{\"role\":\"user\",\"content\":\"" + jsonEscape(prompt) +
        "\"}],\"max_completion_tokens\":200}";

    CURL* curl = curl_easy_init();
    if (!curl) {
        error = "Could not initialize libcurl";
        return false;
    }

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, (std::string("Authorization: Bearer ") + apiKey).c_str());

    curl_easy_setopt(curl, CURLOPT_URL, "https://api.openai.com/v1/chat/completions");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeResponse);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    CURLcode result = CURLE_OK;
    for (int attempt = 0; attempt < 2; ++attempt) {
        response.clear();
        result = curl_easy_perform(curl);
        if (result != CURLE_OPERATION_TIMEDOUT || attempt == 1) break;
        curl_easy_setopt(curl, CURLOPT_FRESH_CONNECT, 1L);
    }
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (result != CURLE_OK) {
        error = curl_easy_strerror(result);
        return false;
    }
    if (status < 200 || status >= 300) {
        error = "OpenAI API returned HTTP " + std::to_string(status) + ": " + response;
        return false;
    }

    answer = extractContent(response);
    if (answer.empty()) {
        error = "Could not read message.content from the OpenAI response";
        return false;
    }
    return true;
}

static std::string extractJsonStringField(const std::string& json, const std::string& field) {
    const auto key = json.find("\"" + field + "\"");
    if (key == std::string::npos) return {};

    auto start = json.find(':', key + field.size() + 2);
    if (start == std::string::npos) return {};
    do { ++start; } while (start < json.size() &&
                           std::isspace(static_cast<unsigned char>(json[start])));
    if (start >= json.size() || json[start++] != '"') return {};

    std::string result;
    bool escaped = false;
    for (; start < json.size(); ++start) {
        const char c = json[start];
        if (escaped) {
            switch (c) {
            case 'n': result += '\n'; break;
            case 'r': result += '\r'; break;
            case 't': result += '\t'; break;
            default: result += c;
            }
            escaped = false;
        } else if (c == '\\') {
            escaped = true;
        } else if (c == '"') {
            return result;
        } else {
            result += c;
        }
    }
    return {};
}

#ifdef _WIN32
struct HttpRequest {
    std::string method;
    std::string path;
    std::string body;
};

static bool sendAll(SOCKET socket, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        const int count = send(socket, data.data() + sent,
                               static_cast<int>(data.size() - sent), 0);
        if (count == SOCKET_ERROR || count == 0) return false;
        sent += static_cast<size_t>(count);
    }
    return true;
}

static bool readHttpRequest(SOCKET client, HttpRequest& request) {
    std::string raw;
    char buffer[4096];
    size_t headerEnd = std::string::npos;

    while ((headerEnd = raw.find("\r\n\r\n")) == std::string::npos) {
        const int count = recv(client, buffer, sizeof(buffer), 0);
        if (count <= 0) return false;
        raw.append(buffer, static_cast<size_t>(count));
        if (raw.size() > 1024 * 1024) return false;
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

static std::string readFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

static void sendHttpResponse(SOCKET client, int status, const std::string& contentType,
                             const std::string& body) {
    const std::string statusText = status == 200 ? "OK" : status == 400 ? "Bad Request" :
                                   status == 404 ? "Not Found" : "Internal Server Error";
    const std::string response =
        "HTTP/1.1 " + std::to_string(status) + " " + statusText + "\r\n"
        "Content-Type: " + contentType + "; charset=utf-8\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n\r\n" + body;
    sendAll(client, response);
}

static int runWebServer(const char* apiKey) {
    WSADATA wsaData{};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "Could not initialize WinSock\n";
        return 1;
    }

    SOCKET server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server == INVALID_SOCKET) {
        WSACleanup();
        return 1;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(8080);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR ||
        listen(server, SOMAXCONN) == SOCKET_ERROR) {
        std::cerr << "Could not start server on http://127.0.0.1:8080\n";
        closesocket(server);
        WSACleanup();
        return 1;
    }

    std::cout << "Open http://127.0.0.1:8080 in your browser\n";
    std::cout << "Press Ctrl+C to stop the server\n";

    while (true) {
        SOCKET client = accept(server, nullptr, nullptr);
        if (client == INVALID_SOCKET) continue;

        const DWORD timeoutMs = 10000;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));

        HttpRequest request;
        if (!readHttpRequest(client, request)) {
            sendHttpResponse(client, 400, "application/json", "{\"error\":\"Invalid request\"}");
            closesocket(client);
            continue;
        }

        if (request.method == "POST" && request.path == "/api/chat") {
            const std::string prompt = extractJsonStringField(request.body, "message");
            if (prompt.empty()) {
                sendHttpResponse(client, 400, "application/json",
                                 "{\"error\":\"Message is required\"}");
            } else {
                std::string result = "{\"answers\":[";
                const std::string models[] = {"gpt-5.6-luna", "gpt-5.6-terra", "gpt-5.6-sol"};
                for (int i = 0; i < 3; ++i) {
                    const std::string& model = models[i];
                    std::string answer;
                    std::string error;
                    const bool success = askOpenAI(apiKey, prompt, model, answer, error);
                    if (i > 0) result += ',';
                    result += "{\"model\":\"" + jsonEscape(model) +
                              "\",\"answer\":\"" + jsonEscape(success ? answer : "") +
                              "\",\"error\":\"" + jsonEscape(success ? "" : error) + "\"}";
                }
                result += "]}";
                sendHttpResponse(client, 200, "application/json", result);
            }
        } else {
            std::string fileName;
            std::string contentType;
            if (request.method == "GET" && request.path == "/") {
                fileName = "index.html";
                contentType = "text/html";
            } else if (request.method == "GET" && request.path == "/styles.css") {
                fileName = "styles.css";
                contentType = "text/css";
            } else if (request.method == "GET" && request.path == "/app.js") {
                fileName = "app.js";
                contentType = "application/javascript";
            }

            const std::string content = fileName.empty() ? "" : readFile(fileName);
            if (content.empty()) sendHttpResponse(client, 404, "text/plain", "Not found");
            else sendHttpResponse(client, 200, contentType, content);
        }
        closesocket(client);
    }
}
#endif

int main(int argc, char* argv[]) {
#ifdef _WIN32
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
#endif

    const char* apiKey = std::getenv("OPENAI_API_KEY");
    if (!apiKey || !*apiKey) {
        std::cerr << "OPENAI_API_KEY is not set\n";
        return 1;
    }

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        std::cerr << "Could not initialize libcurl\n";
        return 1;
    }

    if (argc > 1 && std::string(argv[1]) == "--web") {
#ifdef _WIN32
        const int result = runWebServer(apiKey);
        curl_global_cleanup();
        return result;
#else
        std::cerr << "Web mode is currently available on Windows only\n";
        curl_global_cleanup();
        return 1;
#endif
    }

    std::string prompt = "Say hello in one short sentence.";
#ifdef _WIN32
    if (argc > 1) prompt = argvToUtf8(argv[1]);
    for (int i = 2; i < argc; ++i) prompt += " " + argvToUtf8(argv[i]);
#else
    if (argc > 1) prompt = argv[1];
    for (int i = 2; i < argc; ++i) prompt += " " + std::string(argv[i]);
#endif

    std::string answer;
    std::string error;
    const bool success = askOpenAI(apiKey, prompt, "gpt-5.6-luna", answer, error);
    curl_global_cleanup();
    if (!success) {
        std::cerr << error << '\n';
        return 1;
    }
    std::cout << answer << '\n';
}
