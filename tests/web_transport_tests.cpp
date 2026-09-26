// Real local TCP transport, existing HTTP helpers and dependency-free handshake.
// No Agent API requests, production databases or model calls.
#include "../web_server.cpp"
#include "../websocket_handshake.h"
#include <stdexcept>

namespace {
void requireTransport(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
struct SocketGuard {
    net::Socket value = net::invalidSocket;
    ~SocketGuard() { net::closeSocket(value); }
    SocketGuard() = default;
    SocketGuard(const SocketGuard&) = delete;
    SocketGuard& operator=(const SocketGuard&) = delete;
};
}

int main() {
    try {
        net::SocketRuntime runtime;
        requireTransport(runtime.ready(), "socket runtime");
        requireTransport(websocket::sha1Base64("") == "2jmj7l5rSw0yVb/vlWAYkK/YBwk=", "SHA-1 empty vector");
        requireTransport(websocket::sha1Base64("abc") == "qZk+NkcGgWq6PiVxeFDCbJzQ2J0=", "SHA-1 abc vector");
        std::string accept;
        requireTransport(websocket::acceptKey("dGhlIHNhbXBsZSBub25jZQ==", accept) &&
                         accept == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=", "RFC 6455 handshake vector");
        for (const std::string key : {"", "bad", "AAAAAAAAAAAAAAAAAAAAAB==", "!!!!!!!!!!!!!!!!!!!!!!=="})
            requireTransport(!websocket::acceptKey(key, accept), "reject invalid WebSocket nonce");
        requireTransport(validWebSocketOrigin("", ""), "non-browser WebSocket client");
        requireTransport(validWebSocketOrigin("http://127.0.0.1:8080", "127.0.0.1:8080"), "local same-origin WebSocket");
        requireTransport(!validWebSocketOrigin("http://evil.example:8080", "127.0.0.1:8080"), "reject cross-origin WebSocket");
#ifdef _WIN32
        requireTransport(!validWebSocketOrigin("http://203.0.113.7:8080", "203.0.113.7:8080"), "Windows remains local-only");
#else
        requireTransport(validWebSocketOrigin("http://203.0.113.7:8080", "203.0.113.7:8080"), "Linux external IP origin");
        requireTransport(validWebSocketOrigin("http://agent.example:8080", "AGENT.EXAMPLE:8080"), "Linux external DNS origin");
        requireTransport(!validWebSocketOrigin("http://agent.example:8080", ""), "reject missing browser Host");
        requireTransport(!validWebSocketOrigin("http://agent.example:8080", "agent.example:8080/path"), "reject invalid Host authority");
#endif
        SocketGuard listener, client, server;
        listener.value = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        requireTransport(listener.value != net::invalidSocket, "create TCP listener");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        requireTransport(bind(listener.value, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
                         listen(listener.value, 1) == 0, "listen on ephemeral test port");
        net::SocketLength length = sizeof(address);
        requireTransport(getsockname(listener.value, reinterpret_cast<sockaddr*>(&address), &length) == 0, "get test port");
        client.value = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        requireTransport(client.value != net::invalidSocket &&
                         connect(client.value, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "connect TCP client");
        requireTransport(net::waitReadable(listener.value, 1000) == 1, "portable select detects accept");
        server.value = ::accept(listener.value, nullptr, nullptr);
        requireTransport(server.value != net::invalidSocket, "accept TCP client");
        requireTransport(net::setSocketTimeout(server.value, SO_RCVTIMEO, 1000) &&
                         net::setSocketTimeout(client.value, SO_RCVTIMEO, 1000), "portable receive timeouts");
        const std::string body = u8"{\"text\":\"Проверка МСК\"}";
        requireTransport(sendAll(client.value, "POST /api/reminders HTTP/1.1\r\nHost: 127.0.0.1:8080\r\nContent-Length: " +
                         std::to_string(body.size()) + "\r\n\r\n" + body), "send request with UTF-8 body");
        HttpRequest request;
        requireTransport(readHttpRequest(server.value, request) && request.method == "POST" &&
                         request.path == "/api/reminders" && request.body == body &&
                         request.headers["host"] == "127.0.0.1:8080", "shared HTTP parser reads real TCP request");
        sendHttpResponse(server.value, 200, "application/json", "{\"ok\":true}");
        std::string response;
        char buffer[4096];
        while (response.find("{\"ok\":true}") == std::string::npos) {
            const auto count = net::receive(client.value, buffer, sizeof(buffer));
            requireTransport(count > 0, "receive HTTP response");
            response.append(buffer, static_cast<std::size_t>(count));
        }
        requireTransport(response.find("HTTP/1.1 200 OK") == 0, "shared HTTP response writer");
        requireTransport(net::setNonblocking(server.value), "portable nonblocking mode");
        requireTransport(net::receive(server.value, buffer, sizeof(buffer)) < 0 && net::temporarySocketError(), "nonblocking would-block handling");
#ifndef _WIN32
        int pair[2];
        requireTransport(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0, "create SIGPIPE test sockets");
        SocketGuard sender;
        sender.value = pair[0];
        net::closeSocket(pair[1]);
        requireTransport(net::sendBytes(sender.value, "x", 1) < 0, "closed peer returns error without SIGPIPE termination");
#endif
        std::cout << "TCP HTTP transport, origin validation, socket wrappers and WebSocket handshake passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Web transport test failed: " << error.what() << '\n';
        return 1;
    }
}
