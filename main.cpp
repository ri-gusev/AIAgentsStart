#include <curl/curl.h>

#include <cstdlib>
#include <cctype>
#include <iostream>
#include <string>

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

int main(int argc, char* argv[]) {
    const char* apiKey = std::getenv("OPENAI_API_KEY");
    if (!apiKey || !*apiKey) {
        std::cerr << "OPENAI_API_KEY is not set\n";
        return 1;
    }

    std::string prompt = argc > 1 ? argv[1] : "Say hello in one short sentence.";
    for (int i = 2; i < argc; ++i) prompt += " " + std::string(argv[i]);

    std::string response;
    const std::string body =
        "{\"model\":\"gpt-5.6-luna\",\"messages\":[{\"role\":\"user\",\"content\":\"" +
        jsonEscape(prompt) + "\"}]}";

    curl_global_init(CURL_GLOBAL_DEFAULT);
    CURL* curl = curl_easy_init();
    if (!curl) return 1;

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, (std::string("Authorization: Bearer ") + apiKey).c_str());

    curl_easy_setopt(curl, CURLOPT_URL, "https://api.openai.com/v1/chat/completions");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeResponse);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    const CURLcode result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    curl_global_cleanup();

    if (result != CURLE_OK) {
        std::cerr << "Request failed: " << curl_easy_strerror(result) << "\n";
        return 1;
    }
    if (status < 200 || status >= 300) {
        std::cerr << "API error (HTTP " << status << "): " << response << "\n";
        return 1;
    }

    const std::string content = extractContent(response);
    if (content.empty()) {
        std::cerr << "Could not read response: " << response << "\n";
        return 1;
    }
    std::cout << content << '\n';
}
