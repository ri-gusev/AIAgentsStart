#include "reminder_pipeline.h"
#include <iostream>
#include <stdexcept>
#include <vector>

void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

int main() {
    try {
        const std::vector<std::string> names = {
            "get_upcoming_reminders", "summarize_reminders", "build_reminder_view"};
        const std::vector<std::string> results = {
            R"({"range_days":30,"reminders":[{"id":5,"text":"quoted \"text\"","run_at":"2026-09-27T18:00:00Z","status":"pending"}]})",
            R"({"period_hours":24,"count":1,"items":[{"id":5,"text":"quoted \"text\"","run_at":"2026-09-27T18:00:00Z"}]})",
            R"({"title":"Plans","count":1,"items":[{"id":5,"title":"quoted \"text\""}]})"};
        for (int failure = -1; failure < 3; ++failure) {
            std::size_t called = 0;
            ReminderPipeline pipeline(ReminderPipeline::ToolCaller{
                [&](const std::string& name, const std::string& args, std::string& result, std::string& error) {
                    const auto index = called++;
                    check(name == names.at(index), "Incorrect call order");
                    const std::string expected = index == 0 ? "{\"days\":30}" : index == 1 ?
                        "{\"reminders\":" + results[0] + ",\"hours\":24}" :
                        "{\"summary\":" + results[1] + "}";
                    check(args == expected, "Previous result was not forwarded unchanged");
                    if (static_cast<int>(index) == failure) { error = "tool failed"; return false; }
                    result = results[index]; return true;
                }});
            const auto outcome = pipeline.run();
            if (failure < 0) {
                check(outcome.success && outcome.steps == names && outcome.resultJson == results[2], "Success result mismatch");
                check(called == 3, "Expected three MCP calls");
            } else {
                check(!outcome.success && outcome.failedStep == names[failure] && outcome.error == "tool failed", "Failure result mismatch");
                check(called == static_cast<std::size_t>(failure + 1), "Pipeline continued after failure");
                check(outcome.resultJson.empty(), "Failure exposed a partial result");
            }
        }
        ReminderPipeline invalid(ReminderPipeline::ToolCaller{
            [](const std::string&, const std::string&, std::string& result, std::string&) {
                result = "null"; return true;
            }});
        check(invalid.run().failedStep == names[0], "Non-object tool result was accepted");
        std::cout << "Reminder pipeline: success, argument forwarding, all failure steps, invalid result passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
