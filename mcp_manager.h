#pragma once
#include "mcp_client.h"
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct RegisteredMcpTool {
    std::string alias, serverId, actualToolName, description, schemaJson;
};

struct RoutedMcpCall {
    std::string alias, serverId, actualToolName, argumentsJson, resultJson, error;
    bool success = false;
};

class McpManager {
public:
    explicit McpManager(McpClient& reminder, std::string codeforcesUrl = {});
    bool connectAll(std::string& error);
    std::vector<RegisteredMcpTool> registry() const;
    std::string modelToolsJson() const;
    bool callTool(const std::string& alias, const std::string& argumentsJson,
                  std::string& resultJson, std::string& error);
    McpClient& codeforcesClient();
    std::vector<RoutedMcpCall> calls() const;
    void setSuccessfulCallHandler(std::function<void(const RegisteredMcpTool&)> handler);
    bool syncCodeforces(std::string& resultJson, std::string& error);

private:
    mutable std::recursive_mutex mutex_;
    McpClient& reminder_;
    std::unique_ptr<McpClient> codeforces_;
    std::vector<RoutedMcpCall> calls_;
    std::function<void(const RegisteredMcpTool&)> onSuccess_;
};
