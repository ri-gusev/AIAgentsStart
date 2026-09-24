#include "mcp_client.h"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <sstream>
#include <utility>

namespace {
constexpr long kConnectTimeoutSeconds = 2L;
constexpr long kRequestTimeoutSeconds = 8L;

struct JsonValue {
    enum class Type { Null, Boolean, Number, String, Array, Object };
    Type type = Type::Null;
    bool boolean = false;
    std::string text;
    std::vector<JsonValue> array;
    std::map<std::string, JsonValue> object;

    const JsonValue* member(const std::string& key) const {
        const auto found = object.find(key);
        return found == object.end() ? nullptr : &found->second;
    }
};

void appendUtf8(std::string& output, unsigned codePoint) {
    if (codePoint <= 0x7F) output += static_cast<char>(codePoint);
    else if (codePoint <= 0x7FF) {
        output += static_cast<char>(0xC0 | (codePoint >> 6));
        output += static_cast<char>(0x80 | (codePoint & 0x3F));
    } else if (codePoint <= 0xFFFF) {
        output += static_cast<char>(0xE0 | (codePoint >> 12));
        output += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
        output += static_cast<char>(0x80 | (codePoint & 0x3F));
    } else {
        output += static_cast<char>(0xF0 | (codePoint >> 18));
        output += static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F));
        output += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
        output += static_cast<char>(0x80 | (codePoint & 0x3F));
    }
}

class JsonParser {
public:
    explicit JsonParser(const std::string& input) : input_(input) {}

    bool parse(JsonValue& value, std::string& error) {
        skipWhitespace();
        if (!parseValue(value)) {
            error = "Invalid JSON in MCP response at byte " + std::to_string(position_);
            return false;
        }
        skipWhitespace();
        if (position_ != input_.size()) {
            error = "Unexpected data after MCP JSON response";
            return false;
        }
        return true;
    }

private:
    void skipWhitespace() {
        while (position_ < input_.size() &&
               std::isspace(static_cast<unsigned char>(input_[position_]))) ++position_;
    }

    bool parseValue(JsonValue& value) {
        skipWhitespace();
        if (position_ >= input_.size()) return false;
        const char current = input_[position_];
        if (current == '"') {
            value.type = JsonValue::Type::String;
            return parseString(value.text);
        }
        if (current == '{') return parseObject(value);
        if (current == '[') return parseArray(value);
        if (current == 't' && match("true")) {
            value.type = JsonValue::Type::Boolean; value.boolean = true; return true;
        }
        if (current == 'f' && match("false")) {
            value.type = JsonValue::Type::Boolean; value.boolean = false; return true;
        }
        if (current == 'n' && match("null")) {
            value.type = JsonValue::Type::Null; return true;
        }
        return parseNumber(value);
    }

    bool parseObject(JsonValue& value) {
        value = JsonValue{};
        value.type = JsonValue::Type::Object;
        ++position_;
        skipWhitespace();
        if (consume('}')) return true;
        while (position_ < input_.size()) {
            std::string key;
            if (!parseString(key)) return false;
            skipWhitespace();
            if (!consume(':')) return false;
            JsonValue child;
            if (!parseValue(child)) return false;
            value.object[std::move(key)] = std::move(child);
            skipWhitespace();
            if (consume('}')) return true;
            if (!consume(',')) return false;
            skipWhitespace();
        }
        return false;
    }

    bool parseArray(JsonValue& value) {
        value = JsonValue{};
        value.type = JsonValue::Type::Array;
        ++position_;
        skipWhitespace();
        if (consume(']')) return true;
        while (position_ < input_.size()) {
            JsonValue child;
            if (!parseValue(child)) return false;
            value.array.push_back(std::move(child));
            skipWhitespace();
            if (consume(']')) return true;
            if (!consume(',')) return false;
            skipWhitespace();
        }
        return false;
    }

    bool parseString(std::string& output) {
        output.clear();
        if (!consume('"')) return false;
        while (position_ < input_.size()) {
            const unsigned char current = static_cast<unsigned char>(input_[position_++]);
            if (current == '"') return true;
            if (current < 0x20) return false;
            if (current != '\\') {
                output += static_cast<char>(current);
                continue;
            }
            if (position_ >= input_.size()) return false;
            const char escaped = input_[position_++];
            switch (escaped) {
            case '"': output += '"'; break;
            case '\\': output += '\\'; break;
            case '/': output += '/'; break;
            case 'b': output += '\b'; break;
            case 'f': output += '\f'; break;
            case 'n': output += '\n'; break;
            case 'r': output += '\r'; break;
            case 't': output += '\t'; break;
            case 'u': {
                unsigned codePoint = 0;
                if (!readHex4(codePoint)) return false;
                if (codePoint >= 0xD800 && codePoint <= 0xDBFF) {
                    if (position_ + 2 > input_.size() || input_[position_] != '\\' ||
                        input_[position_ + 1] != 'u') return false;
                    position_ += 2;
                    unsigned low = 0;
                    if (!readHex4(low) || low < 0xDC00 || low > 0xDFFF) return false;
                    codePoint = 0x10000 + ((codePoint - 0xD800) << 10) + low - 0xDC00;
                } else if (codePoint >= 0xDC00 && codePoint <= 0xDFFF) return false;
                appendUtf8(output, codePoint);
                break;
            }
            default: return false;
            }
        }
        return false;
    }

    bool readHex4(unsigned& value) {
        if (input_.size() - position_ < 4) return false;
        value = 0;
        for (int index = 0; index < 4; ++index) {
            const unsigned char current = static_cast<unsigned char>(input_[position_++]);
            unsigned digit = 0;
            if (current >= '0' && current <= '9') digit = current - '0';
            else if (current >= 'a' && current <= 'f') digit = current - 'a' + 10;
            else if (current >= 'A' && current <= 'F') digit = current - 'A' + 10;
            else return false;
            value = value * 16 + digit;
        }
        return true;
    }

    bool parseNumber(JsonValue& value) {
        const size_t start = position_;
        if (position_ < input_.size() && input_[position_] == '-') ++position_;
        if (position_ >= input_.size()) return false;
        if (input_[position_] == '0') ++position_;
        else {
            if (!std::isdigit(static_cast<unsigned char>(input_[position_]))) return false;
            while (position_ < input_.size() &&
                   std::isdigit(static_cast<unsigned char>(input_[position_]))) ++position_;
        }
        if (position_ < input_.size() && input_[position_] == '.') {
            ++position_;
            if (position_ >= input_.size() ||
                !std::isdigit(static_cast<unsigned char>(input_[position_]))) return false;
            while (position_ < input_.size() &&
                   std::isdigit(static_cast<unsigned char>(input_[position_]))) ++position_;
        }
        if (position_ < input_.size() && (input_[position_] == 'e' || input_[position_] == 'E')) {
            ++position_;
            if (position_ < input_.size() && (input_[position_] == '+' || input_[position_] == '-')) ++position_;
            if (position_ >= input_.size() ||
                !std::isdigit(static_cast<unsigned char>(input_[position_]))) return false;
            while (position_ < input_.size() &&
                   std::isdigit(static_cast<unsigned char>(input_[position_]))) ++position_;
        }
        value.type = JsonValue::Type::Number;
        value.text = input_.substr(start, position_ - start);
        return position_ > start;
    }

    bool consume(char expected) {
        if (position_ >= input_.size() || input_[position_] != expected) return false;
        ++position_;
        return true;
    }

    bool match(const char* text) {
        const std::string value(text);
        if (input_.compare(position_, value.size(), value) != 0) return false;
        position_ += value.size();
        return true;
    }

    const std::string& input_;
    size_t position_ = 0;
};

std::string jsonEscape(const std::string& value) {
    std::string result;
    for (unsigned char current : value) {
        switch (current) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\b': result += "\\b"; break;
        case '\f': result += "\\f"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default: result += current < 0x20 ? "?" : std::string(1, static_cast<char>(current));
        }
    }
    return result;
}

std::string serializeJson(const JsonValue& value) {
    switch (value.type) {
    case JsonValue::Type::Null: return "null";
    case JsonValue::Type::Boolean: return value.boolean ? "true" : "false";
    case JsonValue::Type::Number: return value.text;
    case JsonValue::Type::String: return "\"" + jsonEscape(value.text) + "\"";
    case JsonValue::Type::Array: {
        std::string result = "[";
        bool first = true;
        for (const auto& child : value.array) {
            if (!first) result += ',';
            first = false;
            result += serializeJson(child);
        }
        return result + ']';
    }
    case JsonValue::Type::Object: {
        std::string result = "{";
        bool first = true;
        for (const auto& entry : value.object) {
            if (!first) result += ',';
            first = false;
            result += "\"" + jsonEscape(entry.first) + "\":" + serializeJson(entry.second);
        }
        return result + '}';
    }
    }
    return "null";
}

std::string trim(const std::string& value) {
    size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) ++begin;
    size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;
    return value.substr(begin, end - begin);
}

std::string jsonFromMcpBody(const std::string& body) {
    const std::string direct = trim(body);
    if (!direct.empty() && direct.front() == '{') return direct;
    std::istringstream stream(body);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("data:", 0) == 0) {
            const std::string data = trim(line.substr(5));
            if (!data.empty() && data.front() == '{') return data;
        }
    }
    return {};
}

bool parseMcpResponse(const std::string& body, JsonValue& root, std::string& error) {
    const std::string json = jsonFromMcpBody(body);
    if (json.empty()) {
        error = "MCP server returned an empty or unsupported response";
        return false;
    }
    if (!JsonParser(json).parse(root, error) || root.type != JsonValue::Type::Object) {
        if (error.empty()) error = "MCP response must be a JSON object";
        return false;
    }
    if (const JsonValue* rpcError = root.member("error")) {
        std::string message = "MCP request failed";
        if (rpcError->type == JsonValue::Type::Object) {
            const JsonValue* detail = rpcError->member("message");
            if (detail && detail->type == JsonValue::Type::String && !detail->text.empty()) {
                message += ": " + detail->text;
            }
        }
        error = message;
        return false;
    }
    return true;
}

size_t writeBody(void* data, size_t size, size_t count, void* userData) {
    static_cast<std::string*>(userData)->append(static_cast<char*>(data), size * count);
    return size * count;
}

size_t readHeaders(char* data, size_t size, size_t count, void* userData) {
    const size_t bytes = size * count;
    std::string line(data, bytes);
    const auto separator = line.find(':');
    if (separator != std::string::npos) {
        std::string name = line.substr(0, separator);
        for (char& current : name) {
            current = static_cast<char>(std::tolower(static_cast<unsigned char>(current)));
        }
        if (name == "mcp-session-id") {
            *static_cast<std::string*>(userData) = trim(line.substr(separator + 1));
        }
    }
    return bytes;
}
}

struct McpClient::HttpResponse {
    long statusCode = 0;
    std::string body;
    std::string sessionId;
};

McpClient::McpClient(std::string serverUrl) : serverUrl_(std::move(serverUrl)) {
    if (const char* configuredUrl = std::getenv("MCP_SERVER_URL")) {
        if (*configuredUrl) serverUrl_ = configuredUrl;
    }
}

McpClient::~McpClient() {
    std::string ignored;
    disconnect(ignored);
}

bool McpClient::postJson(const std::string& payload, bool includeSession,
                         HttpResponse& response, std::string& error) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        error = "Could not initialize the MCP HTTP client";
        return false;
    }

    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: application/json, text/event-stream");
    const std::string versionHeader = "MCP-Protocol-Version: " + protocolVersion_;
    headers = curl_slist_append(headers, versionHeader.c_str());
    std::string sessionHeader;
    if (includeSession && !sessionId_.empty()) {
        sessionHeader = "Mcp-Session-Id: " + sessionId_;
        headers = curl_slist_append(headers, sessionHeader.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, serverUrl_.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(payload.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, readHeaders);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response.sessionId);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, kConnectTimeoutSeconds);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, kRequestTimeoutSeconds);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_VERBOSE, 0L);

    const CURLcode result = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.statusCode);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (result != CURLE_OK) {
        error = "Cannot reach MCP server at " + serverUrl_ + ": " + curl_easy_strerror(result);
        return false;
    }
    if (response.statusCode < 200 || response.statusCode >= 300) {
        error = "MCP server returned HTTP " + std::to_string(response.statusCode);
        return false;
    }
    return true;
}

bool McpClient::connect(std::string& error) {
    error.clear();
    std::string ignored;
    disconnect(ignored);
    nextRequestId_ = 1;

    HttpResponse response;
    const std::string request =
        "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(nextRequestId_++) +
        ",\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-06-18\"," 
        "\"capabilities\":{},\"clientInfo\":{\"name\":\"AI Agent Course\",\"version\":\"Day 17\"}}}";
    if (!postJson(request, false, response, error)) {
        setError(error);
        return false;
    }

    JsonValue root;
    if (!parseMcpResponse(response.body, root, error)) {
        setError(error);
        return false;
    }
    const JsonValue* result = root.member("result");
    if (!result || result->type != JsonValue::Type::Object) {
        error = "MCP initialize response has no result object";
        setError(error);
        return false;
    }
    if (const JsonValue* version = result->member("protocolVersion")) {
        if (version->type == JsonValue::Type::String && !version->text.empty()) {
            protocolVersion_ = version->text;
        }
    }
    if (const JsonValue* serverInfo = result->member("serverInfo")) {
        if (serverInfo->type == JsonValue::Type::Object) {
            const JsonValue* name = serverInfo->member("name");
            if (name && name->type == JsonValue::Type::String && !name->text.empty()) {
                serverName_ = name->text;
            }
        }
    }
    sessionId_ = response.sessionId;
    if (!sendInitialized(error)) {
        setError(error);
        return false;
    }
    status_ = "Connected";
    if (!refreshTools(error)) {
        setError(error);
        return false;
    }
    status_ = "Connected";
    lastError_.clear();
    return true;
}

bool McpClient::sendInitialized(std::string& error) {
    HttpResponse response;
    return postJson("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\",\"params\":{}}",
                    true, response, error);
}

bool McpClient::refreshTools(std::string& error) {
    error.clear();
    if (status_ != "Connected" && sessionId_.empty()) {
        error = "Connect to the MCP server before refreshing tools";
        return false;
    }
    HttpResponse response;
    const std::string request =
        "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(nextRequestId_++) +
        ",\"method\":\"tools/list\",\"params\":{}}";
    if (!postJson(request, true, response, error)) {
        setError(error);
        return false;
    }
    JsonValue root;
    if (!parseMcpResponse(response.body, root, error)) {
        setError(error);
        return false;
    }
    const JsonValue* result = root.member("result");
    const JsonValue* tools = result && result->type == JsonValue::Type::Object
        ? result->member("tools") : nullptr;
    if (!tools || tools->type != JsonValue::Type::Array) {
        error = "MCP tools/list response has no tools array";
        setError(error);
        return false;
    }

    std::vector<McpTool> discovered;
    for (const auto& item : tools->array) {
        if (item.type != JsonValue::Type::Object) continue;
        const JsonValue* name = item.member("name");
        const JsonValue* description = item.member("description");
        const JsonValue* schema = item.member("inputSchema");
        if (!name || name->type != JsonValue::Type::String || name->text.empty() || !schema) continue;
        McpTool tool;
        tool.name = name->text;
        if (description && description->type == JsonValue::Type::String) {
            tool.description = description->text;
        }
        tool.inputSchemaJson = serializeJson(*schema);
        discovered.push_back(std::move(tool));
    }
    tools_ = std::move(discovered);
    status_ = "Connected";
    lastError_.clear();
    return true;
}

bool McpClient::callTool(const std::string& name, const std::string& argumentsJson,
                        std::string& resultJson, std::string& error) {
    resultJson.clear();
    error.clear();
    McpToolCallRecord record;
    record.name = name;
    record.argumentsJson = argumentsJson;
    const auto finish = [this, &record, &resultJson, &error](bool success) {
        record.success = success;
        record.resultJson = resultJson;
        record.error = error;
        calls_.push_back(std::move(record));
        if (calls_.size() > 20) calls_.erase(calls_.begin());
        return success;
    };
    if (status_ != "Connected" || sessionId_.empty()) {
        error = "MCP server is not connected";
        return finish(false);
    }
    const auto tool = std::find_if(tools_.begin(), tools_.end(), [&name](const McpTool& item) {
        return item.name == name;
    });
    if (tool == tools_.end()) {
        error = "Tool is not available from the connected MCP server";
        return finish(false);
    }
    JsonValue arguments;
    if (!JsonParser(argumentsJson).parse(arguments, error) ||
        arguments.type != JsonValue::Type::Object) {
        error = "Tool arguments must be a valid JSON object";
        return finish(false);
    }

    const unsigned long long requestId = nextRequestId_++;
    HttpResponse response;
    const std::string request =
        "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(requestId) +
        ",\"method\":\"tools/call\",\"params\":{\"name\":\"" + jsonEscape(name) +
        "\",\"arguments\":" + serializeJson(arguments) + "}}";
    if (!postJson(request, true, response, error)) {
        setError(error);
        return finish(false);
    }
    JsonValue root;
    if (!parseMcpResponse(response.body, root, error)) {
        setError(error);
        return finish(false);
    }
    const JsonValue* result = root.member("result");
    if (!result || result->type != JsonValue::Type::Object) {
        error = "MCP tools/call response has no result object";
        setError(error);
        return finish(false);
    }
    const JsonValue* isError = result->member("isError");
    if (isError && isError->type == JsonValue::Type::Boolean && isError->boolean) {
        error = "MCP tool returned an error";
        const JsonValue* content = result->member("content");
        if (content && content->type == JsonValue::Type::Array) {
            for (const auto& item : content->array) {
                const JsonValue* text = item.member("text");
                if (text && text->type == JsonValue::Type::String && !text->text.empty()) {
                    error = text->text;
                    break;
                }
            }
        }
        resultJson = "{\"isError\":true,\"error\":\"" + jsonEscape(error) + "\"}";
        return finish(false);
    }
    const JsonValue* structured = result->member("structuredContent");
    resultJson = structured ? serializeJson(*structured) : serializeJson(*result);
    return finish(true);
}

bool McpClient::disconnect(std::string& error) {
    error.clear();
    if (!sessionId_.empty()) {
        CURL* curl = curl_easy_init();
        if (curl) {
            curl_slist* headers = nullptr;
            const std::string sessionHeader = "Mcp-Session-Id: " + sessionId_;
            const std::string versionHeader = "MCP-Protocol-Version: " + protocolVersion_;
            headers = curl_slist_append(headers, sessionHeader.c_str());
            headers = curl_slist_append(headers, versionHeader.c_str());
            curl_easy_setopt(curl, CURLOPT_URL, serverUrl_.c_str());
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, kConnectTimeoutSeconds);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, kRequestTimeoutSeconds);
            curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
            curl_easy_setopt(curl, CURLOPT_VERBOSE, 0L);
            curl_easy_perform(curl);
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
        }
    }
    sessionId_.clear();
    tools_.clear();
    calls_.clear();
    status_ = "Disconnected";
    lastError_.clear();
    return true;
}

void McpClient::setError(const std::string& error) {
    status_ = "Error";
    lastError_ = error;
    tools_.clear();
}

const std::string& McpClient::status() const { return status_; }
const std::string& McpClient::serverName() const { return serverName_; }
const std::string& McpClient::serverUrl() const { return serverUrl_; }
const std::string& McpClient::lastError() const { return lastError_; }
const std::vector<McpTool>& McpClient::tools() const { return tools_; }
const std::vector<McpToolCallRecord>& McpClient::calls() const { return calls_; }
