#include "mcp_client.h"
#include "json_value.h"

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

using app_json::JsonValue;
using app_json::JsonParser;
using app_json::jsonEscape;
using app_json::serializeJson;

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

McpClient::McpClient(std::string serverUrl, bool useReminderEnvironment) : serverUrl_(std::move(serverUrl)) {
    if (const char* configuredUrl = useReminderEnvironment ? std::getenv("MCP_SERVER_URL") : nullptr) {
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
