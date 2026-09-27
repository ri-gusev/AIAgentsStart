#include "reminder_pipeline.h"
#include "mcp_client.h"
#include <utility>

ReminderPipeline::ReminderPipeline(McpClient& client)
    : ReminderPipeline([&client](const std::string& name, const std::string& arguments,
                                 std::string& result, std::string& error) {
          return client.callTool(name, arguments, result, error);
      }) {}

ReminderPipeline::ReminderPipeline(ToolCaller caller) : callTool_(std::move(caller)) {}

ReminderPipelineResult ReminderPipeline::run(int days, int hours) const {
    ReminderPipelineResult outcome;
    std::string data;
    auto step = [&](const std::string& name, const std::string& arguments) {
        std::string next;
        if (!callTool_(name, arguments, next, outcome.error)) {
            outcome.failedStep = name;
            if (outcome.error.empty()) outcome.error = "MCP tool call failed";
            return false;
        }
        // McpClient already parses and serializes the result. These tools must
        // return structured objects, not MCP text/content envelopes.
        const auto first = next.find_first_not_of(" \t\r\n");
        if (first == std::string::npos || next[first] != '{') {
            outcome.failedStep = name;
            outcome.error = "Expected a structured JSON object from MCP tool";
            return false;
        }
        outcome.steps.push_back(name);
        data = std::move(next);
        return true;
    };
    if (!step("get_upcoming_reminders", "{\"days\":" + std::to_string(days) + "}")) return outcome;
    if (!step("summarize_reminders", "{\"reminders\":" + data +
              ",\"hours\":" + std::to_string(hours) + "}")) return outcome;
    if (!step("build_reminder_view", "{\"summary\":" + data + "}")) return outcome;
    outcome.success = true;
    outcome.resultJson = std::move(data);
    return outcome;
}
