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
