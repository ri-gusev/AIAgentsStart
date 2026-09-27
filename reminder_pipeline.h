#pragma once

#include <functional>
#include <string>
#include <vector>

class McpClient;

struct ReminderPipelineResult {
    bool success = false;
    std::vector<std::string> steps;
    std::string resultJson;
    std::string failedStep;
    std::string error;
};

class ReminderPipeline {
public:
    using ToolCaller = std::function<bool(const std::string&, const std::string&,
                                         std::string&, std::string&)>;
    explicit ReminderPipeline(McpClient& client);
    // Injectable transport for tests; production uses the existing McpClient.
    explicit ReminderPipeline(ToolCaller caller);
    ReminderPipelineResult run(int days = 30, int hours = 24) const;

private:
    ToolCaller callTool_;
};
