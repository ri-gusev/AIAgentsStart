#include "api_client.h"
#include "json_value.h"

#include <curl/curl.h>

#include <cctype>
#include <limits>
#include <algorithm>
#include <regex>

namespace {
constexpr long kRequestTimeoutSeconds = 90L;
constexpr long kMaxCompletionTokens = 1600L;

size_t writeResponse(void* data, size_t size, size_t count, void* userData) {
    static_cast<std::string*>(userData)->append(static_cast<char*>(data), size * count);
    return size * count;
}

std::string jsonEscape(const std::string& text) {
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
        default: result += c < 0x20 ? "?" : std::string(1, static_cast<char>(c));
        }
    }
    return result;
}

bool readHexCodeUnit(const std::string& json, size_t& position, unsigned& codeUnit) {
    if (json.size() - position < 4) return false;
    codeUnit = 0;
    for (size_t digitIndex = 0; digitIndex < 4; ++digitIndex) {
        const unsigned char c = static_cast<unsigned char>(json[position++]);
        unsigned digit = 0;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else return false;
        codeUnit = codeUnit * 16 + digit;
    }
    return true;
}

void appendUtf8(std::string& result, unsigned codePoint) {
    if (codePoint <= 0x7F) {
        result += static_cast<char>(codePoint);
    } else if (codePoint <= 0x7FF) {
        result += static_cast<char>(0xC0 | (codePoint >> 6));
        result += static_cast<char>(0x80 | (codePoint & 0x3F));
    } else if (codePoint <= 0xFFFF) {
        result += static_cast<char>(0xE0 | (codePoint >> 12));
        result += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
        result += static_cast<char>(0x80 | (codePoint & 0x3F));
    } else {
        result += static_cast<char>(0xF0 | (codePoint >> 18));
        result += static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F));
        result += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
        result += static_cast<char>(0x80 | (codePoint & 0x3F));
    }
}

std::string extractJsonStringField(const std::string& json, const std::string& field,
                                   size_t from = 0) {
    const auto key = json.find("\"" + field + "\"", from);
    if (key == std::string::npos) return {};
    auto start = json.find(':', key + field.size() + 2);
    if (start == std::string::npos) return {};
    do { ++start; } while (start < json.size() && std::isspace(static_cast<unsigned char>(json[start])));
    if (start >= json.size() || json[start++] != '"') return {};
    std::string result;
    while (start < json.size()) {
        const unsigned char c = static_cast<unsigned char>(json[start++]);
        if (c == '"') return result;
        if (c < 0x20) return {};
        if (c != '\\') {
            result += static_cast<char>(c);
            continue;
        }
        if (start >= json.size()) return {};
        switch (json[start++]) {
        case '"': result += '"'; break;
        case '\\': result += '\\'; break;
        case '/': result += '/'; break;
        case 'n': result += '\n'; break;
        case 'r': result += '\r'; break;
        case 't': result += '\t'; break;
        case 'b': result += '\b'; break;
        case 'f': result += '\f'; break;
        case 'u': {
            unsigned codePoint = 0;
            if (!readHexCodeUnit(json, start, codePoint)) return {};
            if (codePoint >= 0xD800 && codePoint <= 0xDBFF) {
                if (json.size() - start < 6 || json[start] != '\\' ||
                    json[start + 1] != 'u') return {};
                start += 2;
                unsigned lowSurrogate = 0;
                if (!readHexCodeUnit(json, start, lowSurrogate) ||
                    lowSurrogate < 0xDC00 || lowSurrogate > 0xDFFF) return {};
                codePoint = 0x10000 + ((codePoint - 0xD800) << 10) +
                            (lowSurrogate - 0xDC00);
            } else if (codePoint >= 0xDC00 && codePoint <= 0xDFFF) {
                return {};
            }
            appendUtf8(result, codePoint);
            break;
        }
        default: return {};
        }
    }
    return {};
}

std::string extractContent(const std::string& json) {
    const auto choices = json.find("\"choices\"");
    if (choices == std::string::npos) return {};
    const auto message = json.find("\"message\"", choices);
    if (message == std::string::npos) return {};
    return extractJsonStringField(json, "content", message);
}

size_t findTopLevelFieldValue(const std::string& json, const std::string& field) {
    int objectDepth = 0;
    for (size_t position = 0; position < json.size(); ++position) {
        if (json[position] == '{') {
            ++objectDepth;
            continue;
        }
        if (json[position] == '}') {
            --objectDepth;
            continue;
        }
        if (json[position] != '"') continue;

        const size_t stringStart = position + 1;
        bool escaped = false;
        bool containsEscape = false;
        size_t stringEnd = stringStart;
        for (; stringEnd < json.size(); ++stringEnd) {
            const char c = json[stringEnd];
            if (escaped) {
                escaped = false;
                continue;
            }
            if (c == '\\') {
                escaped = true;
                containsEscape = true;
                continue;
            }
            if (c == '"') break;
        }
        if (stringEnd >= json.size()) return std::string::npos;

        if (objectDepth == 1 && !containsEscape &&
            stringEnd - stringStart == field.size() &&
            json.compare(stringStart, field.size(), field) == 0) {
            size_t colon = stringEnd + 1;
            while (colon < json.size() &&
                   std::isspace(static_cast<unsigned char>(json[colon]))) ++colon;
            if (colon < json.size() && json[colon] == ':') return colon + 1;
        }
        position = stringEnd;
    }
    return std::string::npos;
}

size_t findJsonObjectEnd(const std::string& json, size_t objectStart) {
    int depth = 0;
    bool inString = false;
    bool escaped = false;
    for (size_t position = objectStart; position < json.size(); ++position) {
        const char c = json[position];
        if (inString) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') inString = false;
            continue;
        }
        if (c == '"') inString = true;
        else if (c == '{') ++depth;
        else if (c == '}' && --depth == 0) return position;
    }
    return std::string::npos;
}

bool extractJsonUnsignedField(const std::string& json, const std::string& field,
                              size_t from, size_t to, std::uint64_t& value) {
    const auto key = json.find("\"" + field + "\"", from);
    if (key == std::string::npos || key >= to) return false;
    const auto colon = json.find(':', key + field.size() + 2);
    if (colon == std::string::npos || colon >= to) return false;
    size_t position = json.find_first_not_of(" \t\r\n", colon + 1);
    if (position == std::string::npos || position >= to ||
        !std::isdigit(static_cast<unsigned char>(json[position]))) {
        return false;
    }

    std::uint64_t parsed = 0;
    while (position < to && std::isdigit(static_cast<unsigned char>(json[position]))) {
        const unsigned digit = static_cast<unsigned>(json[position] - '0');
        if (parsed > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) return false;
        parsed = parsed * 10 + digit;
        ++position;
    }
    value = parsed;
    return true;
}

void extractTokenUsage(const std::string& json, ApiTokenUsage& usage) {
    usage = {};
    const auto usageValue = findTopLevelFieldValue(json, "usage");
    if (usageValue == std::string::npos) return;
    const auto usageStart = json.find_first_not_of(" \t\r\n", usageValue);
    if (usageStart == std::string::npos || json[usageStart] != '{') return;
    const auto usageEnd = findJsonObjectEnd(json, usageStart);
    if (usageEnd == std::string::npos) return;
    extractJsonUnsignedField(json, "prompt_tokens", usageStart, usageEnd, usage.inputTokens);
    extractJsonUnsignedField(json, "cached_tokens", usageStart, usageEnd,
                             usage.cachedInputTokens);
    extractJsonUnsignedField(json, "completion_tokens", usageStart, usageEnd, usage.outputTokens);
    extractJsonUnsignedField(json, "total_tokens", usageStart, usageEnd, usage.totalTokens);
}

std::string extractFinishReason(const std::string& json) {
    const auto position = json.rfind("\"finish_reason\"");
    return position == std::string::npos
               ? std::string{}
               : extractJsonStringField(json, "finish_reason", position);
}

std::string explainMissingContent(const std::string& json) {
    const auto choices = json.find("\"choices\"");
    const std::string refusal = extractJsonStringField(json, "refusal", choices);
    if (!refusal.empty()) return "The model declined to provide an answer";

    const std::string finishReason = extractFinishReason(json);
    std::string error = "OpenAI returned HTTP 200 but choices[0].message.content was empty or null";
    if (finishReason == "stop" || finishReason == "length" ||
        finishReason == "content_filter" || finishReason == "tool_calls" ||
        finishReason == "function_call") {
        error += " (finish_reason=" + finishReason + ")";
    }
    if (finishReason == "length") error += ". The completion token limit was reached.";
    return error;
}

std::string buildChatCompletionBody(const std::string& model, const std::string& messagesJson,
                                    bool jsonResponse, const std::string& toolsJson,
                                    bool includeParallelOption = true) {
    std::string requestMessages = messagesJson;
    if (jsonResponse && !requestMessages.empty() && requestMessages.front() == '[') {
        const std::string instruction = "{\"role\":\"system\",\"content\":\"Return one valid JSON object only.\"}";
        requestMessages.insert(1, instruction + (requestMessages.size() > 2 ? "," : ""));
    }
    std::string body = "{\"model\":\"" + jsonEscape(model) + "\",\"messages\":" + requestMessages +
                       ",\"max_completion_tokens\":" + std::to_string(kMaxCompletionTokens);
    if (jsonResponse) body += ",\"response_format\":{\"type\":\"json_object\"}";
    if (!toolsJson.empty() && toolsJson != "[]") {
        body += ",\"tools\":" + toolsJson + ",\"tool_choice\":\"auto\"";
        // Luna's default reasoning is incompatible with function tools on
        // Chat Completions. Keep the existing endpoint/model; apply to every
        // tool-loop request, including the request following a tool result.
        if (model == "gpt-5.6-luna" || model.rfind("gpt-5.6-luna-", 0) == 0)
            body += ",\"reasoning_effort\":\"none\"";
        if (includeParallelOption) body += ",\"parallel_tool_calls\":false";
    }
    return body + '}';
}

struct ProviderError {
    std::string type, code, param, message;
};
ProviderError parseProviderError(const std::string& response) {
    ProviderError fields;
    app_json::JsonValue root; std::string ignored;
    if (!app_json::JsonParser(response).parse(root, ignored)) return fields;
    const auto* error = root.member("error");
    if (!error || error->type != app_json::JsonValue::Type::Object) return fields;
    auto read = [&](const char* key) {
        const auto* field = error->member(key);
        return field && field->type == app_json::JsonValue::Type::String ? field->text : std::string{};
    };
    fields.type = read("type"); fields.code = read("code");
    fields.param = read("param"); fields.message = read("message");
    return fields;
}
bool rejectsParallelOption(long status, const std::string& response) {
    const auto error = parseProviderError(response);
    return status == 400 && error.param == "parallel_tool_calls" &&
        (error.code == "unsupported_parameter" || error.message.rfind("Unsupported parameter", 0) == 0);
}
void replaceSensitiveValue(std::string& text, const std::string& value) {
    if (value.empty()) return;
    std::size_t at = 0;
    while ((at = text.find(value, at)) != std::string::npos) {
        text.replace(at, value.size(), "[redacted]"); at += 10;
    }
}

std::string sanitizeProviderMessage(std::string message, const std::string& apiKey,
                                    const std::string& messagesJson) {
    replaceSensitiveValue(message, apiKey);
    // Remove exact conversation content if an error echoes it without quotes.
    app_json::JsonValue messages; std::string ignored;
    if (app_json::JsonParser(messagesJson).parse(messages, ignored) &&
        messages.type == app_json::JsonValue::Type::Array) {
        for (const auto& item : messages.array) {
            const auto* content = item.member("content");
            if (content && content->type == app_json::JsonValue::Type::String)
                replaceSensitiveValue(message, content->text);
        }
    }
    // Bound provider-controlled input and diagnostic output. Redaction precedes
    // output truncation so a truncated credential is not accidentally revealed.
    if (message.size() > 16384) message.resize(16384);
    message = std::regex_replace(message, std::regex(
        R"((authorization\s*[:=]\s*(bearer\s+)?|bearer\s+|(?:api[_ -]?key|password|secret|access[_ -]?token)\s*[:=]\s*)[^\s,;]+)",
        std::regex::icase), "[redacted]");
    message = std::regex_replace(message, std::regex(
        R"((sk-|sess-)[A-Za-z0-9_-]+|eyJ[A-Za-z0-9_-]+\.[A-Za-z0-9_-]+\.[A-Za-z0-9_-]+)"), "[redacted]");
    message = std::regex_replace(message, std::regex(
        R"(https?://[^\s]+|[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,})", std::regex::icase), "[redacted]");
    message = std::regex_replace(message, std::regex(
        R"((?:prompt|submitted text|request body|input|content|arguments)\s*[:=].*)", std::regex::icase), "[redacted]");
    const std::vector<std::string> safeNames = {
        "tools", "messages", "model", "tool_choice", "parallel_tool_calls", "response_format",
        "max_completion_tokens", "max_tokens", "reasoning_effort", "temperature", "top_p",
        "required", "additionalProperties", "properties", "items", "type", "object", "array",
        "string", "integer", "boolean", "number", "null", "function", "parameters", "strict",
        "$ref", "$defs", "system", "developer", "user", "assistant", "tool", "role", "content",
        "json", "json_object", "json_schema", "auto", "none", "true", "false"
    };
    std::string cleaned;
    for (std::size_t index = 0; index < message.size();) {
        const char quote = message[index];
        // An apostrophe in an English word is not a quoted request value.
        const bool apostrophe = quote == '\'' && index > 0 && index + 1 < message.size() &&
            std::isalpha(static_cast<unsigned char>(message[index-1])) &&
            std::isalpha(static_cast<unsigned char>(message[index+1]));
        if ((quote == '\'' || quote == '"' || quote == '`') && !apostrophe) {
            std::size_t end = index + 1;
            while (end < message.size() && message[end] != quote) {
                if (message[end] == '\\' && end + 1 < message.size()) ++end;
                ++end;
            }
            const std::string value = message.substr(index + 1, end - index - 1);
            if (end < message.size() && std::find(safeNames.begin(), safeNames.end(), value) != safeNames.end())
                cleaned += quote + value + quote;
            else cleaned += "[redacted]";
            index = end < message.size() ? end + 1 : end;
        } else {
            const auto byte = static_cast<unsigned char>(message[index++]);
            cleaned += byte < 32 || byte == 127 ? ' ' : static_cast<char>(byte);
        }
    }
    if (cleaned.size() > 512) {
        std::size_t end = 512;
        while (end > 0 && (static_cast<unsigned char>(cleaned[end]) & 0xC0) == 0x80) --end;
        cleaned.resize(end); cleaned += "...";
    }
    return cleaned;
}

std::string safeProviderError(long status, const std::string& response,
                              const std::string& apiKey = {}, const std::string& messagesJson = {}) {
    // Metadata uses a whitelist; the message is redacted separately because it
    // can echo prompts, arguments, tool names or credentials.
    const auto fields = parseProviderError(response);
    std::string result = "OpenAI API returned HTTP " + std::to_string(status);
    for (const char* type : {"invalid_request_error", "authentication_error", "permission_error", "rate_limit_error", "server_error"})
        if (fields.type == type) result += "; type=" + fields.type;
    for (const char* code : {"unsupported_parameter", "unsupported_value", "invalid_function_parameters", "invalid_json_schema",
                             "model_not_found", "context_length_exceeded", "invalid_api_key", "insufficient_quota"})
        if (fields.code == code) result += "; code=" + fields.code;
    for (const char* param : {"tools", "messages", "model", "tool_choice", "parallel_tool_calls",
                              "response_format", "max_completion_tokens", "max_tokens"}) {
        const std::string name = param;
        if (fields.param == name || fields.param.rfind(name + "[", 0) == 0 || fields.param.rfind(name + ".", 0) == 0)
            result += "; param=" + name;
    }
    if (fields.message.rfind("Invalid schema for function", 0) == 0)
        result += "; detail=Invalid function tool schema";
    else if (fields.message.rfind("Unsupported parameter", 0) == 0)
        result += "; detail=Unsupported request parameter";
    else if (fields.message.rfind("Unsupported value", 0) == 0)
        result += "; detail=Unsupported request value";
    else if (fields.message.rfind("Invalid JSON", 0) == 0)
        result += "; detail=Invalid request JSON";
    const std::string message = sanitizeProviderMessage(fields.message, apiKey, messagesJson);
    if (!message.empty()) result += "; message=" + message;
    return result;
}
}

ApiClient::ApiClient() : initialized_(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK) {}
ApiClient::~ApiClient() { if (initialized_) curl_global_cleanup(); }
bool ApiClient::isReady() const { return initialized_; }

bool ApiClient::parseChatCompletionResponse(const std::string& responseJson,
                                            ApiChatResponse& completion, std::string& error) {
    completion = {}; error.clear();
    app_json::JsonValue root;
    if (!app_json::JsonParser(responseJson).parse(root, error)) {
        error = "Invalid OpenAI response JSON"; return false;
    }
    const auto* choices = root.member("choices");
    const auto* message = choices && choices->type == app_json::JsonValue::Type::Array && !choices->array.empty() ?
        choices->array.front().member("message") : nullptr;
    const auto* role = message ? message->member("role") : nullptr;
    if (!message || message->type != app_json::JsonValue::Type::Object || !role ||
        role->type != app_json::JsonValue::Type::String || role->text != "assistant") {
        error = "OpenAI response has no assistant message"; return false;
    }
    app_json::JsonValue assistant;
    assistant.type = app_json::JsonValue::Type::Object;
    assistant.object["role"] = *role;
    const auto* content = message->member("content");
    if (content && content->type != app_json::JsonValue::Type::Null &&
        content->type != app_json::JsonValue::Type::String) {
        error = "Invalid OpenAI assistant content"; return false;
    }
    assistant.object["content"] = content ? *content : app_json::JsonValue{};
    if (content && content->type == app_json::JsonValue::Type::String) completion.content = content->text;
    const auto* toolCalls = message->member("tool_calls");
    if (toolCalls && toolCalls->type != app_json::JsonValue::Type::Null) {
        if (toolCalls->type != app_json::JsonValue::Type::Array) {
            error = "Invalid OpenAI tool_calls"; return false;
        }
        for (const auto& call : toolCalls->array) {
            const auto* id = call.member("id");
            const auto* type = call.member("type");
            const auto* function = call.member("function");
            const auto* name = function ? function->member("name") : nullptr;
            const auto* arguments = function ? function->member("arguments") : nullptr;
            const auto validString = [](const auto* value) {
                return value && value->type == app_json::JsonValue::Type::String;
            };
            if (!validString(id) || id->text.empty() || !validString(type) || type->text != "function" ||
                !validString(name) || name->text.empty() || !validString(arguments)) {
                error = "Invalid OpenAI function tool call"; return false;
            }
            for (const auto& previous : completion.toolCalls) if (previous.id == id->text) {
                error = "Duplicate OpenAI tool_call ID"; return false;
            }
            completion.toolCalls.push_back({id->text, name->text, arguments->text});
        }
        assistant.object["tool_calls"] = *toolCalls;
    }
    if (completion.content.empty() && completion.toolCalls.empty()) {
        error = "OpenAI response contains neither content nor tool calls"; return false;
    }
    completion.assistantMessageJson = app_json::serializeJson(assistant);
    return true;
}

bool ApiClient::sendChatCompletion(const std::string& apiKey, const std::string& model,
                                   const std::string& messagesJson, std::string& answer,
                                   std::string& error, bool jsonResponse,
                                   ApiTokenUsage* usage,
                                   std::string* finishReason, const std::string& toolsJson,
                                   ApiChatResponse* completion) const {
    answer.clear();
    error.clear();
    if (usage) *usage = {};
    if (finishReason) finishReason->clear();
    if (completion) *completion = {};
    if (!initialized_) { error = "Could not initialize libcurl"; return false; }
    std::string body = buildChatCompletionBody(model, messagesJson, jsonResponse, toolsJson);
    CURL* curl = curl_easy_init();
    if (!curl) { error = "Could not initialize libcurl"; return false; }
    std::string response;
    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, ("Authorization: Bearer " + apiKey).c_str());
    curl_easy_setopt(curl, CURLOPT_URL, "https://api.openai.com/v1/chat/completions");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, kRequestTimeoutSeconds);
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
    // Some models reject this optional switch. Retry only that explicit 400,
    // once, with the same model/tools/messages; never fall back to text-only.
    if (result == CURLE_OK && rejectsParallelOption(status, response)) {
        body = buildChatCompletionBody(model, messagesJson, jsonResponse, toolsJson, false);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
        response.clear();
        result = curl_easy_perform(curl);
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (result != CURLE_OK) { error = curl_easy_strerror(result); return false; }
    if (status < 200 || status >= 300) {
        // Provider error bodies may echo submitted text or credentials. Never
        // forward an unredacted body to the browser or diagnostic logs.
        error = safeProviderError(status, response, apiKey, messagesJson);
        return false;
    }
    if (usage) extractTokenUsage(response, *usage);
    if (finishReason) *finishReason = extractFinishReason(response);
    if (completion) {
        if (!parseChatCompletionResponse(response, *completion, error)) return false;
        answer = completion->content;
        return true;
    }
    answer = extractContent(response);
    if (answer.empty()) { error = explainMissingContent(response); return false; }
    return true;
}
