#include "reminder_events.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>

namespace {
std::string frame(unsigned char opcode, const std::string& payload) {
    std::string output(1, static_cast<char>(0x80 | opcode));
    if (payload.size() < 126) output += static_cast<char>(payload.size());
    else if (payload.size() <= 65535) {
        output += static_cast<char>(126);
        output += static_cast<char>((payload.size() >> 8) & 255);
        output += static_cast<char>(payload.size() & 255);
    } else {
        output += static_cast<char>(127);
        for (int shift = 56; shift >= 0; shift -= 8)
            output += static_cast<char>((static_cast<std::uint64_t>(payload.size()) >> shift) & 255);
    }
    return output + payload;
}

bool handshakeAccept(const std::string& key, std::string& accept) {
    unsigned char nonce[32]{};
    DWORD nonceSize = sizeof(nonce);
    if (!CryptStringToBinaryA(key.c_str(), static_cast<DWORD>(key.size()), CRYPT_STRING_BASE64,
                             nonce, &nonceSize, nullptr, nullptr) || nonceSize != 16) return false;
    const std::string data = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    unsigned char digest[20]{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA1_ALGORITHM, nullptr, 0) != 0) return false;
    bool success = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) == 0;
    if (success) success = BCryptHashData(hash,
        reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())), static_cast<ULONG>(data.size()), 0) == 0 &&
        BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0;
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!success) return false;
    DWORD length = 0;
    if (!CryptBinaryToStringA(digest, sizeof(digest), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &length)) return false;
    std::vector<char> encoded(length);
    if (!CryptBinaryToStringA(digest, sizeof(digest), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, encoded.data(), &length)) return false;
    accept = encoded.data();
    return true;
}
}

struct ReminderEvents::Impl {
    struct Client {
        SOCKET socket;
        std::string incoming;
        std::string outgoing;
        bool closing = false;
    };
    std::mutex mutex;
    std::condition_variable wake;
    bool stopped = false;
    std::vector<Client> clients;
    std::thread worker;

    Impl() : worker([this] { run(); }) {}
    ~Impl() { stop(); }
    void stop() {
        { std::lock_guard<std::mutex> lock(mutex); stopped = true; }
        wake.notify_all();
        if (worker.joinable()) worker.join();
    }
    bool readFrames(Client& client) {
        char buffer[4096];
        const int count = recv(client.socket, buffer, sizeof(buffer), 0);
        if (count == 0) return false;
        if (count == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) return false;
        if (count > 0) client.incoming.append(buffer, static_cast<std::size_t>(count));
        if (client.incoming.size() > 8192) return false;
        while (client.incoming.size() >= 2) {
            const auto byte = [&client](std::size_t index) { return static_cast<unsigned char>(client.incoming[index]); };
            const unsigned char opcode = byte(0) & 15;
            if ((byte(0) & 0x70) || !(byte(0) & 0x80) || !(byte(1) & 0x80)) return false;
            std::uint64_t length = byte(1) & 127;
            std::size_t header = 2;
            if (length == 126) {
                if (client.incoming.size() < 4) break;
                length = (static_cast<unsigned>(byte(2)) << 8) | byte(3); header = 4;
            } else if (length == 127) {
                if (client.incoming.size() < 10) break;
                length = 0;
                for (int index = 2; index < 10; ++index) length = (length << 8) | byte(index);
                header = 10;
            }
            if (length > 4096 || (opcode >= 8 && length > 125)) return false;
            if (client.incoming.size() < header + 4 + length) break;
            std::string payload = client.incoming.substr(header + 4, static_cast<std::size_t>(length));
            for (std::size_t index = 0; index < payload.size(); ++index)
                payload[index] ^= client.incoming[header + index % 4];
            client.incoming.erase(0, header + 4 + static_cast<std::size_t>(length));
            if (opcode == 8) { client.outgoing += frame(8, payload); client.closing = true; break; }
            if (opcode == 9) client.outgoing += frame(10, payload);
            else if (opcode != 10 && opcode != 1) return false;
        }
        return true;
    }
    void run() {
        auto nextPing = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        std::unique_lock<std::mutex> lock(mutex);
        while (!stopped) {
            const bool ping = std::chrono::steady_clock::now() >= nextPing;
            if (ping) nextPing = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            for (auto client = clients.begin(); client != clients.end();) {
                bool alive = readFrames(*client);
                if (ping && !client->closing) client->outgoing += frame(9, "");
                if (client->outgoing.size() > 1024 * 1024) alive = false;
                if (alive && !client->outgoing.empty()) {
                    const int sent = send(client->socket, client->outgoing.data(), static_cast<int>(client->outgoing.size()), 0);
                    if (sent > 0) client->outgoing.erase(0, static_cast<std::size_t>(sent));
                    else if (sent == 0 || WSAGetLastError() != WSAEWOULDBLOCK) alive = false;
                }
                if (client->closing && client->outgoing.empty()) alive = false;
                if (!alive) { closesocket(client->socket); client = clients.erase(client); }
                else ++client;
            }
            wake.wait_for(lock, std::chrono::milliseconds(100));
        }
        for (const auto& client : clients) closesocket(client.socket);
        clients.clear();
    }
};

ReminderEvents::ReminderEvents() : impl_(std::make_unique<Impl>()) {}
ReminderEvents::~ReminderEvents() = default;
void ReminderEvents::stop() { impl_->stop(); }
bool ReminderEvents::addClient(std::uintptr_t rawSocket, const std::string& websocketKey,
                               const std::string& initialState, std::string& error) {
    std::string accept;
    if (!handshakeAccept(websocketKey, accept)) { error = "Invalid WebSocket key"; return false; }
    const SOCKET socket = static_cast<SOCKET>(rawSocket);
    const std::string response = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + accept + "\r\n\r\n";
    const DWORD timeout = 1000;
    setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->stopped || impl_->clients.size() >= 32) { error = "Too many reminder connections"; return false; }
    std::size_t sent = 0;
    while (sent < response.size()) {
        const int count = send(socket, response.data() + sent, static_cast<int>(response.size() - sent), 0);
        if (count <= 0) { error = "WebSocket handshake failed"; return false; }
        sent += static_cast<std::size_t>(count);
    }
    u_long nonblocking = 1;
    if (ioctlsocket(socket, FIONBIO, &nonblocking) != 0) { error = "Could not initialize event connection"; return false; }
    impl_->clients.push_back({socket, {}, frame(1, initialState), false});
    impl_->wake.notify_all();
    return true;
}
void ReminderEvents::broadcast(const std::string& eventJson) {
    const std::string message = frame(1, eventJson);
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (auto& client : impl_->clients) if (!client.closing) client.outgoing += message;
    impl_->wake.notify_all();
}
#else
struct ReminderEvents::Impl {};
ReminderEvents::ReminderEvents() : impl_(std::make_unique<Impl>()) {}
ReminderEvents::~ReminderEvents() = default;
void ReminderEvents::stop() {}
bool ReminderEvents::addClient(std::uintptr_t, const std::string&, const std::string&, std::string& error) {
    error = "Web server requires Windows"; return false;
}
void ReminderEvents::broadcast(const std::string&) {}
#endif
