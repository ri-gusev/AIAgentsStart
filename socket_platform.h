#pragma once

#include <algorithm>
#include <climits>
#include <cstddef>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <cerrno>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#endif

namespace net {
#ifdef _WIN32
using Socket = SOCKET;
using SocketLength = int;
inline constexpr Socket invalidSocket = INVALID_SOCKET;
#else
using Socket = int;
using SocketLength = socklen_t;
inline constexpr Socket invalidSocket = -1;
#endif

class SocketRuntime {
public:
    SocketRuntime() {
#ifdef _WIN32
        WSADATA data{};
        ready_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
#endif
    }
    ~SocketRuntime() {
#ifdef _WIN32
        if (ready_) WSACleanup();
#endif
    }
    SocketRuntime(const SocketRuntime&) = delete;
    SocketRuntime& operator=(const SocketRuntime&) = delete;
    bool ready() const { return ready_; }
private:
    bool ready_ = true;
};

inline void closeSocket(Socket socket) {
    if (socket == invalidSocket) return;
#ifdef _WIN32
    closesocket(socket);
#else
    close(socket);
#endif
}

inline int lastSocketError() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}
inline bool interruptedSocketError() {
#ifdef _WIN32
    return lastSocketError() == WSAEINTR;
#else
    return lastSocketError() == EINTR;
#endif
}
inline bool temporarySocketError() {
    const int error = lastSocketError();
#ifdef _WIN32
    return error == WSAEWOULDBLOCK || error == WSAEINTR;
#else
    return error == EAGAIN || error == EWOULDBLOCK || error == EINTR;
#endif
}

inline std::ptrdiff_t receive(Socket socket, char* buffer, std::size_t length) {
    const int size = static_cast<int>((std::min)(length, static_cast<std::size_t>(INT_MAX)));
    return recv(socket, buffer, size, 0);
}
inline std::ptrdiff_t sendBytes(Socket socket, const char* buffer, std::size_t length) {
    const int size = static_cast<int>((std::min)(length, static_cast<std::size_t>(INT_MAX)));
#ifdef _WIN32
    return send(socket, buffer, size, 0);
#else
    // A disconnected client must not terminate the backend with SIGPIPE.
    return send(socket, buffer, size, MSG_NOSIGNAL);
#endif
}

inline bool setSocketTimeout(Socket socket, int option, int milliseconds) {
#ifdef _WIN32
    const DWORD timeout = static_cast<DWORD>(milliseconds);
    return setsockopt(socket, SOL_SOCKET, option, reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == 0;
#else
    timeval timeout{};
    timeout.tv_sec = milliseconds / 1000;
    timeout.tv_usec = (milliseconds % 1000) * 1000;
    return setsockopt(socket, SOL_SOCKET, option, &timeout, sizeof(timeout)) == 0;
#endif
}
inline bool setNonblocking(Socket socket) {
#ifdef _WIN32
    u_long enabled = 1;
    return ioctlsocket(socket, FIONBIO, &enabled) == 0;
#else
    const int flags = fcntl(socket, F_GETFL, 0);
    return flags != -1 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) != -1;
#endif
}
inline int waitReadable(Socket socket, int milliseconds) {
#ifndef _WIN32
    if (socket < 0 || socket >= FD_SETSIZE) { errno = EINVAL; return -1; }
#endif
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(socket, &readSet);
    timeval timeout{};
    timeout.tv_sec = milliseconds / 1000;
    timeout.tv_usec = (milliseconds % 1000) * 1000;
#ifdef _WIN32
    return select(0, &readSet, nullptr, nullptr, &timeout);
#else
    return select(socket + 1, &readSet, nullptr, nullptr, &timeout);
#endif
}
} // namespace net
