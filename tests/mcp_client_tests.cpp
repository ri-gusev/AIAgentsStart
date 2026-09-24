// Exercise MCP JSON/SSE parsing without starting a server or making requests.
#include "../mcp_client.cpp"

#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* description) {
    if (!condition) throw std::runtime_error(description);
}
}

int main() {
    try {
        const std::string response = R"({"jsonrpc":"2.0","id":2,"result":{"tools":[{"name":"add","description":"Adds two numbers.","inputSchema":{"type":"object","properties":{"a":{"type":"number"},"b":{"type":"number"}},"required":["a","b"]}}]}})";
        JsonValue root;
        std::string error;
        require(parseMcpResponse(response, root, error), "parse tools/list response");
        const JsonValue* result = root.member("result");
        const JsonValue* tools = result ? result->member("tools") : nullptr;
        require(tools && tools->type == JsonValue::Type::Array && tools->array.size() == 1,
                "extract tools array");
        require(tools->array[0].member("name")->text == "add", "extract tool name");
        require(serializeJson(*tools->array[0].member("inputSchema")).find("\"required\":[\"a\",\"b\"]") != std::string::npos,
                "preserve input schema");

        JsonValue sseRoot;
        require(parseMcpResponse("event: message\r\ndata: " + response + "\r\n\r\n",
                                 sseRoot, error), "parse SSE-wrapped response");
        JsonValue errorRoot;
        require(!parseMcpResponse(R"({"jsonrpc":"2.0","id":1,"error":{"code":-32601,"message":"No method"}})",
                                  errorRoot, error) && error.find("No method") != std::string::npos,
                "surface MCP error");

        McpClient client("http://127.0.0.1:1/mcp");
        std::string toolResult;
        require(!client.callTool("add", R"({"a":1,"b":2})", toolResult, error),
                "reject tool call while disconnected");
        require(error.find("not connected") != std::string::npos,
                "describe disconnected tool call");
        require(client.calls().size() == 1 && !client.calls().front().success &&
                client.calls().front().argumentsJson == R"({"a":1,"b":2})",
                "record manually requested tool call status and arguments");
        std::cout << "MCP response parser tests passed (no network requests)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "MCP client test failed: " << error.what() << '\n';
        return 1;
    }
}
