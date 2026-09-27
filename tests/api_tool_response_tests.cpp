#include "../api_client.cpp"
#include <iostream>
#include <stdexcept>

void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
int main() {
    try {
        ApiChatResponse result;
        std::string error;
        app_json::JsonValue request;
        const std::string body = buildChatCompletionBody("existing-model", "[]", false,
            R"([{"type":"function","function":{"name":"codeforces__get_new_contests","description":"read","parameters":{"type":"object","properties":{}}}}])");
        check(app_json::JsonParser(body).parse(request,error), "Invalid tool request JSON");
        check(request.member("model")->text=="existing-model", "Model unexpectedly changed");
        check(request.member("tool_choice")->text=="auto" && !request.member("parallel_tool_calls")->boolean, "Wrong tool choice options");
        check(request.member("tools")->array.size()==1 && !request.member("response_format"), "Tool request fields incorrect");
        const std::string fixtureTools = R"([{"type":"function","function":{"name":"codeforces__get_new_contests","parameters":{"type":"object","properties":{}}}}])";
        for (const std::string& messages : {std::string(R"([{"role":"user","content":"Hello"}])"),
                std::string(R"([{"role":"assistant","content":null,"tool_calls":[{"id":"x","type":"function","function":{"name":"codeforces__get_new_contests","arguments":"{}"}}]},{"role":"tool","tool_call_id":"x","content":"{}"}])")}) {
            check(app_json::JsonParser(buildChatCompletionBody("gpt-5.6-luna", messages, false, fixtureTools)).parse(request,error), "Luna request malformed");
            check(request.member("model")->text=="gpt-5.6-luna" && request.member("reasoning_effort") &&
                request.member("reasoning_effort")->text=="none" && request.member("tools"), "Luna tool reasoning compatibility missing");
        }
        check(app_json::JsonParser(buildChatCompletionBody("gpt-5.6-luna", "[]", true, "[]")).parse(request,error) &&
            !request.member("reasoning_effort") && request.member("response_format"), "Task/review reasoning unexpectedly changed");
        check(app_json::JsonParser(buildChatCompletionBody("existing-model", "[]", false, fixtureTools)).parse(request,error) &&
            !request.member("reasoning_effort"), "Another model's reasoning unexpectedly changed");
        check(app_json::JsonParser(buildChatCompletionBody("existing-model", "[]", false,
            R"([{"type":"function","function":{"name":"tool","parameters":{"type":"object","properties":{}}}}])", false)).parse(request,error), "Fallback JSON invalid");
        check(request.member("tools") && request.member("tool_choice") && !request.member("parallel_tool_calls"), "Fallback disabled tools");
        const std::string unsupported = R"({"error":{"type":"invalid_request_error","code":"unsupported_parameter","param":"parallel_tool_calls","message":"Unsupported parameter: private prompt sk-test-secret"}})";
        check(rejectsParallelOption(400,unsupported) && !rejectsParallelOption(500,unsupported), "Wrong compatibility retry conditions");
        const std::string schemaError = R"({"error":{"type":"invalid_request_error","code":"invalid_function_parameters","param":"tools[2].function.parameters","message":"Invalid schema for function 'private-secret': submitted credentials sk-test-secret"}})";
        const std::string diagnostic = safeProviderError(400,schemaError);
        check(diagnostic.find("param=tools")!=std::string::npos && diagnostic.find("Invalid function tool schema")!=std::string::npos, "Missing safe schema diagnosis");
        check(diagnostic.find("private-secret")==std::string::npos && diagnostic.find("sk-test-secret")==std::string::npos, "Provider content leaked");
        check(!rejectsParallelOption(400,schemaError) && !rejectsParallelOption(400,"bad JSON"), "Unrelated 400 retried");
        check(safeProviderError(401,R"({"error":{"type":"sk-private","code":"private","param":"sk-secret","message":"credentials"}})") == "OpenAI API returned HTTP 401; message=credentials", "Untrusted metadata leaked");
        const std::string genericError = safeProviderError(400,
            R"({"error":{"type":"invalid_request_error","param":null,"code":null,"message":"Missing required parameter: 'messages'."}})");
        check(genericError.find("message=Missing required parameter: 'messages'.")!=std::string::npos, "Null metadata hid provider message");
        const std::string constraint = sanitizeProviderMessage(
            "Invalid schema for function 'private-tool': 'additionalProperties' is required to be supplied and to be false.", "", "[]");
        check(constraint.find("private-tool")==std::string::npos && constraint.find("'additionalProperties' is required")!=std::string::npos, "Schema reason over-redacted");
        const std::string secrets = sanitizeProviderMessage(
            "API key=opaque-secret Bearer opaque-bearer sk-proj-private-token; URL https://example.test/?key=private and private@example.test; received 'private prompt'.",
            "opaque-secret", "[]");
        for (const char* value : {"opaque-secret", "opaque-bearer", "sk-proj-private-token", "example.test", "private prompt"})
            check(secrets.find(value)==std::string::npos, "Sensitive error content leaked");
        check(sanitizeProviderMessage("Invalid request: private user sentence", "",
            R"([{"role":"user","content":"private user sentence"}])").find("private user sentence")==std::string::npos, "Unquoted input echoed");
        check(sanitizeProviderMessage("Invalid request. content: unquoted private text", "", "[]").find("unquoted private text")==std::string::npos, "Labeled input leaked");
        check(sanitizeProviderMessage("Invalid value 'unterminated private value", "", "[]").find("private value")==std::string::npos, "Unterminated quote leaked");
        const std::string longMessage = sanitizeProviderMessage("Error: " + std::string(700, 'x') + " sk-secret", "", "[]");
        check(longMessage.size()<=515 && longMessage.find("sk-secret")==std::string::npos, "Diagnostic length or redaction failed");
        check(safeProviderError(400,"not JSON") == "OpenAI API returned HTTP 400", "Malformed provider response leaked");
        const std::string legacy = buildChatCompletionBody("existing-model","[]",true,"");
        check(app_json::JsonParser(legacy).parse(request,error) && request.member("response_format") && !request.member("tools"), "Legacy JSON requests changed");
        const std::string call = R"({"choices":[{"finish_reason":"tool_calls","message":{"role":"assistant","content":null,"tool_calls":[{"id":"call_1","type":"function","function":{"name":"codeforces__get_new_contests","arguments":"{}"}}],"reasoning_content":"private"}}]})";
        check(ApiClient::parseChatCompletionResponse(call,result,error), "Valid tool call rejected");
        check(result.content.empty() && result.toolCalls.size()==1, "Tool-only message not parsed");
        check(result.toolCalls[0].name=="codeforces__get_new_contests" && result.toolCalls[0].argumentsJson=="{}", "Incorrect function fields");
        check(result.assistantMessageJson.find("private")==std::string::npos, "Hidden reasoning leaked into conversation");
        app_json::JsonValue duplicate;
        check(app_json::JsonParser(call).parse(duplicate,error), "Fixture parsing failed");
        auto& array = duplicate.object["choices"].array[0].object["message"].object["tool_calls"].array;
        array.push_back(array.front());
        check(!ApiClient::parseChatCompletionResponse(app_json::serializeJson(duplicate),result,error), "Duplicate call ID accepted");
        check(ApiClient::parseChatCompletionResponse(R"({"choices":[{"message":{"role":"assistant","content":"Привет"}}]})",result,error), "Plain answer rejected");
        check(result.content=="Привет" && result.toolCalls.empty(), "Plain answer incorrectly parsed");
        for (const std::string& malformed : {
            std::string("not JSON"), std::string(R"({"choices":[]})"),
            std::string(R"({"choices":[{"message":{"role":"assistant","content":null}}]})"),
            std::string(R"({"choices":[{"message":{"role":"assistant","content":null,"tool_calls":{}}}]})"),
            std::string(R"({"choices":[{"message":{"role":"assistant","content":null,"tool_calls":[{"id":"x","type":"function","function":{"name":"tool","arguments":{}}}]}}]})")}) {
            check(!ApiClient::parseChatCompletionResponse(malformed,result,error) && !error.empty(), "Malformed reply accepted");
        }
        std::cout << "PASS: OpenAI text/tool responses, null content, invalid replies and hidden-reasoning exclusion\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
