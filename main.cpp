#include <curl/curl.h>

#include <cstdlib>
#include <cctype>
#include <iostream>
#include <string>
#include <vector>

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

static std::string extractJsonObject(const std::string& text) {
    const size_t begin = text.find('{');
    if (begin == std::string::npos) return {};

    int depth = 0;
    bool inString = false;
    bool escaped = false;
    for (size_t i = begin; i < text.size(); ++i) {
        const char c = text[i];
        if (inString) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') inString = false;
            continue;
        }
        if (c == '"') inString = true;
        else if (c == '{') ++depth;
        else if (c == '}' && --depth == 0) return text.substr(begin, i - begin + 1);
    }
    return {};
}

static std::string extractJsonString(const std::string& json, const std::string& field) {
    const auto fieldPos = json.find("\"" + field + "\"");
    if (fieldPos == std::string::npos) return {};
    auto start = json.find(':', fieldPos + field.size() + 2);
    if (start == std::string::npos) return {};
    while (++start < json.size() && std::isspace(static_cast<unsigned char>(json[start]))) {}
    if (start >= json.size() || json[start++] != '"') return {};

    std::string value;
    for (; start < json.size(); ++start) {
        if (json[start] == '"' && json[start - 1] != '\\') break;
        if (json[start] == '\\' && start + 1 < json.size()) {
            const char escaped = json[++start];
            value += escaped == 'n' ? '\n' : escaped;
        } else value += json[start];
    }
    return value;
}

static std::vector<std::string> extractJsonArray(const std::string& json, const std::string& field) {
    std::vector<std::string> values;
    const auto fieldPos = json.find("\"" + field + "\"");
    if (fieldPos == std::string::npos) return values;
    auto start = json.find('[', fieldPos);
    if (start == std::string::npos) return values;
    while (++start < json.size()) {
        while (start < json.size() && (std::isspace(static_cast<unsigned char>(json[start])) || json[start] == ',')) ++start;
        if (start >= json.size() || json[start] == ']') break;
        if (json[start++] != '"') break;
        std::string value;
        for (; start < json.size(); ++start) {
            if (json[start] == '"' && json[start - 1] != '\\') break;
            if (json[start] == '\\' && start + 1 < json.size()) value += json[++start];
            else value += json[start];
        }
        values.push_back(value);
    }
    return values;
}

int main(int argc, char* argv[]) {
    const char* apiKey = std::getenv("OPENAI_API_KEY");
    if (!apiKey || !*apiKey) {
        std::cerr << "OPENAI_API_KEY is not set\n";
        return 1;
    }

    const bool strict = argc > 1 && std::string(argv[1]) == "--strict";
    const bool free = argc <= 1 || std::string(argv[1]) == "--free";
    if (!free && !strict) {
        std::cerr << "Usage: openai_cli [--free|--strict] [prompt]\n";
        return 1;
    }

    const int promptStart = argc > 1 ? 2 : 1;
    std::string prompt = argc > promptStart ? argv[promptStart] :
        (strict ? "Explain an electronic component as JSON." : "Say hello in one short sentence.");
    for (int i = promptStart + 1; i < argc; ++i) prompt += " " + std::string(argv[i]);
    if (strict) {
        prompt += "Return JSON with one string field "
                  "title and an array field points. Use exactly one title and at most 4 points."
                  " Do not include any other fields or explanatory text.";
    }

    std::string response;
    std::string body =
        "{\"model\":\"gpt-5.6-luna\",\"messages\":[{\"role\":\"user\",\"content\":\"" +
        jsonEscape(prompt) + "\"}],\"max_completion_tokens\":200";
    if (strict) body += ",\"response_format\":{\"type\":\"json_object\"}";
    body += "}";

    curl_global_init(CURL_GLOBAL_DEFAULT);
    CURL* curl = curl_easy_init();
    if (!curl) return 1;

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, (std::string("Authorization: Bearer ") + apiKey).c_str());

    curl_easy_setopt(curl, CURLOPT_URL, "https://api.openai.com/v1/chat/completions");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 15L);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_VERBOSE, 0L);
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
    if (strict) {
        const std::string json = extractJsonObject(content);
        if (json.empty()) {
            std::cerr << "Response is not a JSON object\n";
            return 1;
        }
        const std::string title = extractJsonString(json, "title");
        const std::vector<std::string> points = extractJsonArray(json, "points");
        if (title.empty() || points.size() > 4) {
            std::cerr << "Invalid strict response format\n";
            return 1;
        }
        std::cout << title << '\n';
        for (const auto& point : points) std::cout << "- " << point << '\n';
    } else {
        std::cout << content << '\n';
    }
}
