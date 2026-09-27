#include "mcp_manager.h"
#include "json_value.h"
#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <sstream>
#include <utility>

namespace {
std::string aliasFor(const std::string& server, const std::string& name) {
    const std::string alias = server + "__" + name;
    if (alias.size() <= 64 && alias.find_first_not_of(
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") == std::string::npos) return alias;
    // Stable safe aliases for future tools with names outside OpenAI's limits.
    std::uint64_t hash = 14695981039346656037ULL;
    for (unsigned char c : name) { hash ^= c; hash *= 1099511628211ULL; }
    std::ostringstream encoded;
    encoded << server << "__tool_" << std::hex << hash;
    return encoded.str();
}
}

McpManager::McpManager(McpClient& reminder, std::string codeforcesUrl) : reminder_(reminder) {
    if (codeforcesUrl.empty()) {
        const char* configured = std::getenv("CODEFORCES_MCP_SERVER_URL");
        codeforcesUrl = configured && *configured ? configured : "http://127.0.0.1:8001/mcp";
    }
    codeforces_ = std::make_unique<McpClient>(std::move(codeforcesUrl), false);
}

bool McpManager::connectAll(std::string& error) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    error.clear();
    bool success = true;
    for (const auto& server : std::vector<std::pair<std::string, McpClient*>>{
            {"reminder", &reminder_}, {"codeforces", codeforces_.get()}}) {
        std::string detail;
        const bool ready = server.second->status() == "Connected" ?
            server.second->refreshTools(detail) : server.second->connect(detail);
        if (ready && server.first == "codeforces" && codeforcesStateJson_.empty()) {
            std::string cached, ignored;
            if (codeforces_->callTool("get_new_contests", "{}", cached, ignored)) codeforcesStateJson_ = cached;
        }
        if (!ready) {
            if (!error.empty()) error += "; ";
            error += server.first + ": " + detail;
            success = false;
        }
    }
    return success;
}

std::vector<RegisteredMcpTool> McpManager::registry() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<RegisteredMcpTool> tools;
    for (const auto& server : std::vector<std::pair<std::string, const McpClient*>>{
            {"reminder", &reminder_}, {"codeforces", codeforces_.get()}}) {
        if (server.second->status() != "Connected") continue;
        for (const auto& tool : server.second->tools()) {
            tools.push_back({aliasFor(server.first, tool.name), server.first, tool.name,
                             tool.description, tool.inputSchemaJson});
        }
    }
    return tools;
}

std::string McpManager::modelToolsJson() const {
    std::string json = "[";
    bool first = true;
    for (const auto& tool : registry()) {
        if (!first) json += ',';
        first = false;
        json += "{\"type\":\"function\",\"function\":{\"name\":\"" + app_json::jsonEscape(tool.alias) +
                "\",\"description\":\"" + app_json::jsonEscape(tool.description) +
                "\",\"strict\":false,\"parameters\":" + (tool.schemaJson.empty() ? "{}" : tool.schemaJson) + "}}";
    }
    return json + ']';
}

bool McpManager::callTool(const std::string& alias, const std::string& argumentsJson,
                          std::string& resultJson, std::string& error) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    resultJson.clear(); error.clear();
    const auto tools = registry();
    const auto found = std::find_if(tools.begin(), tools.end(), [&](const auto& tool) { return tool.alias == alias; });
    RoutedMcpCall record;
    record.alias = alias; record.argumentsJson = argumentsJson;
    if (found == tools.end()) error = "Unknown or unavailable MCP tool alias";
    else {
        record.serverId = found->serverId; record.actualToolName = found->actualToolName;
        McpClient& client = found->serverId == "reminder" ? reminder_ : *codeforces_;
        record.success = client.callTool(found->actualToolName, argumentsJson, resultJson, error);
    }
    record.resultJson = resultJson; record.error = error;
    if (record.success && record.serverId == "codeforces" &&
        (record.actualToolName == "get_new_contests" || record.actualToolName == "sync_contests"))
        codeforcesStateJson_ = resultJson;
    calls_.push_back(record);
    if (calls_.size() > 20) calls_.erase(calls_.begin());
    if (record.success && onSuccess_) onSuccess_(*found);
    return record.success;
}

McpClient& McpManager::codeforcesClient() { return *codeforces_; }
std::vector<RoutedMcpCall> McpManager::calls() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_); return calls_;
}
void McpManager::setSuccessfulCallHandler(std::function<void(const RegisteredMcpTool&)> handler) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    onSuccess_ = std::move(handler);
}

bool McpManager::syncCodeforces(std::string& resultJson, std::string& error) {
    // Background polling touches only this server; serialize with Agent calls.
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    resultJson.clear(); error.clear();
    if (codeforces_->status() != "Connected" && !codeforces_->connect(error)) return false;
    RoutedMcpCall record;
    record.alias = "codeforces__sync_contests";
    record.serverId = "codeforces";
    record.actualToolName = "sync_contests";
    record.argumentsJson = "{}";
    record.success = codeforces_->callTool(record.actualToolName, record.argumentsJson, resultJson, error);
    record.resultJson = resultJson; record.error = error;
    if (record.success) codeforcesStateJson_ = resultJson;
    calls_.push_back(record);
    if (calls_.size() > 20) calls_.erase(calls_.begin());
    return record.success;
}

bool McpManager::setServerConnection(const std::string& serverId, bool connect, std::string& error) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    McpClient* client = serverId == "reminder" ? &reminder_ : serverId == "codeforces" ? codeforces_.get() : nullptr;
    if (!client) { error = "Unknown MCP server"; return false; }
    const bool success = connect ? client->connect(error) : client->disconnect(error);
    if (success && connect && serverId == "codeforces") {
        std::string cached, ignored;
        if (client->callTool("get_new_contests", "{}", cached, ignored)) codeforcesStateJson_ = cached;
    }
    return success;
}

std::string McpManager::uiStateJson() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::string json = "{\"servers\":[";
    bool first = true;
    for (const auto& server : std::vector<std::pair<std::string, const McpClient*>>{
            {"reminder", &reminder_}, {"codeforces", codeforces_.get()}}) {
        if (!first) json += ',';
        first = false;
        json += "{\"id\":\"" + server.first + "\",\"name\":\"" +
            std::string(server.first == "reminder" ? "Reminder MCP" : "Codeforces MCP") +
            "\",\"status\":\"" + app_json::jsonEscape(server.second->status()) +
            "\",\"url\":\"" + app_json::jsonEscape(server.second->serverUrl()) +
            "\",\"tool_count\":" + std::to_string(server.second->tools().size()) + "}";
    }
    json += "],\"codeforces\":";
    std::string syncAt = "null", newCount = "0";
    app_json::JsonValue state; std::string ignored;
    if (!codeforcesStateJson_.empty() && app_json::JsonParser(codeforcesStateJson_).parse(state, ignored)) {
        const auto* at = state.member("checked_at");
        if (!at) at = state.member("last_sync_at");
        const auto* count = state.member("new_count");
        // Build only presentation metadata, never expose raw tool results.
        if (at) syncAt = app_json::serializeJson(*at);
        if (count) newCount = app_json::serializeJson(*count);
    }
    json += "{\"last_sync_at\":" + syncAt + ",\"new_count\":" + newCount + "},\"calls\":[";
    first = true;
    for (const auto& call : calls_) {
        if (!first) json += ',';
        first = false;
        json += "{\"server_id\":\"" + app_json::jsonEscape(call.serverId) +
            "\",\"tool\":\"" + app_json::jsonEscape(call.actualToolName.empty() ? call.alias : call.actualToolName) +
            "\",\"success\":" + (call.success ? "true" : "false") + "}";
    }
    return json + "]}";
}
