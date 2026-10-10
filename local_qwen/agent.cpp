#include "agent.h"
#include <curl/curl.h>
#include "../json_value.h"
#include <iostream>
#include <string>
#ifdef _WIN32
#include <windows.h>
#endif

static size_t receive(char* data, size_t size, size_t count, void* output) {
    const size_t bytes = size * count;
    static_cast<std::string*>(output)->append(data, bytes);
    return bytes;
}

int local_qwen::Agent::run() {
#ifdef _WIN32
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
#endif
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) return 1;
    CURL* curl = curl_easy_init();
    if (!curl) { curl_global_cleanup(); return 1; }
    curl_slist* headers = curl_slist_append(nullptr, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, "http://127.0.0.1:11434/api/chat");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
    curl_easy_setopt(curl, CURLOPT_NOPROXY, "127.0.0.1");

    history_.clear();
    std::cout << "Local Qwen (qwen3:8b). Type /exit to quit.\n";
    std::string input;
    int exitCode = 0;
    while (std::cout << "You> " << std::flush, std::getline(std::cin, input)) {
        if (input == "/exit") break;
        if (input.empty()) continue;
        const std::string user = "{\"role\":\"user\",\"content\":\"" + app_json::jsonEscape(input) + "\"}";
        const std::string messages = history_.empty() ? user : history_ + ',' + user;
        const std::string body = "{\"model\":\"qwen3:8b\",\"stream\":false,\"think\":false,\"messages\":[" + messages + "]}";
        std::string response;
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        const CURLcode result = curl_easy_perform(curl);
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        if (result != CURLE_OK) {
            std::cerr << "HTTP connection failed: " << curl_easy_strerror(result)
                      << ". Check that Ollama is running on port 11434.\n";
            exitCode = 1;
            continue;
        }
        if (status < 200 || status >= 300) {
            std::cerr << "Ollama HTTP " << status << ": " << response << '\n';
            exitCode = 1;
            continue;
        }
        app_json::JsonValue root;
        std::string error;
        if (!app_json::JsonParser(response).parse(root, error)) {
            std::cerr << "Invalid JSON from Ollama.\n";
            exitCode = 1;
            continue;
        }
        const auto* message = root.member("message");
        const auto* content = message ? message->member("content") : nullptr;
        if (!content || content->type != app_json::JsonValue::Type::String) {
            std::cerr << "Ollama response has no message.content.\n";
            exitCode = 1;
            continue;
        }
        history_ = messages + ",{\"role\":\"assistant\",\"content\":\"" + app_json::jsonEscape(content->text) + "\"}";
        std::cout << "Qwen> " << content->text << "\n\n";
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    curl_global_cleanup();
    return exitCode;
}
