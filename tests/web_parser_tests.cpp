// Exercise private HTTP parsing helpers without exposing test-only production APIs.
// No server is started and no LLM requests are made.
#include "../web_server.cpp"

#include <stdexcept>

namespace {
void require(bool condition, const char* description) {
    if (!condition) throw std::runtime_error(description);
}
}

int main() {
    try {
        require(extractJsonStringField(R"({"message":"Hello"})", "message") == "Hello",
                "plain message");
        require(extractJsonStringField(R"({"message":"\u041f\u0440\u0438\u0432\u0435\u0442"})", "message") ==
                    u8"Привет", "Unicode Cyrillic");
        require(extractJsonStringField(R"({"message":"\ud83d\ude80"})", "message") ==
                    u8"🚀", "surrogate pair");
        require(extractJsonStringField(R"({"message":"\"\\\/\b\f\n\r\t"})", "message") ==
                    std::string("\"\\/\b\f\n\r\t"), "standard escapes");
        const char* invalid[] = {
            R"({"message":"\uD800"})", R"({"message":"\uDC00"})",
            R"({"message":"\uD800\u0041"})", R"({"message":"\u00xx"})",
            R"({"message":"\q"})", R"({"message":null})", R"({"message":123})",
            "{\"message\":\"unterminated", "{\"message\":\"raw\nline\"}"
        };
        for (const char* json : invalid) {
            require(extractJsonStringField(json, "message").empty(), "reject malformed string");
        }
        std::size_t length = 0;
        require(parseContentLength(" 42 \r", length) && length == 42, "content length");
        require(parseContentLength("0", length) && length == 0, "zero content length");
        const char* invalidLengths[] = {
            "", "-1", "42junk", "1.5", "1048577", "99999999999999999999999999999"
        };
        for (const char* value : invalidLengths) {
            require(!parseContentLength(value, length), "reject invalid/oversized length");
        }
        std::cout << "HTTP parser tests passed (no API requests)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "HTTP parser test failed: " << error.what() << '\n';
        return 1;
    }
}
