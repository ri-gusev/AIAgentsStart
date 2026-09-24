#pragma once

#include <string>
#include <vector>

struct McpTool {
    std::string name;
    std::string description;
    std::string inputSchemaJson;
};

struct McpToolCallRecord {
    std::string name;
    std::string argumentsJson;
    bool success = false;
    std::string resultJson;
    std::string error;
};

class McpClient {
public:
    explicit McpClient(std::string serverUrl = "http://127.0.0.1:8000/mcp");
    ~McpClient();

    McpClient(const McpClient&) = delete;
    McpClient& operator=(const McpClient&) = delete;

    bool connect(std::string& error);
    bool disconnect(std::string& error);
    bool refreshTools(std::string& error);
    bool callTool(const std::string& name, const std::string& argumentsJson,
                  std::string& resultJson, std::string& error);

    const std::string& status() const;
    const std::string& serverName() const;
    const std::string& serverUrl() const;
    const std::string& lastError() const;
    const std::vector<McpTool>& tools() const;
    const std::vector<McpToolCallRecord>& calls() const;

private:
    struct HttpResponse;

    bool postJson(const std::string& payload, bool includeSession,
                  HttpResponse& response, std::string& error);
    bool sendInitialized(std::string& error);
    void setError(const std::string& error);

    std::string status_ = "Disconnected";
    std::string serverName_ = "Local Tools Server";
    std::string serverUrl_;
    std::string sessionId_;
    std::string protocolVersion_ = "2025-06-18";
    std::string lastError_;
    std::vector<McpTool> tools_;
    std::vector<McpToolCallRecord> calls_;
    unsigned long long nextRequestId_ = 1;
};
