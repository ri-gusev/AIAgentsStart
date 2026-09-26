#pragma once

#include <cstdint>
#include <memory>
#include <string>

// Owns upgraded sockets. Nonblocking I/O runs separately from chat/MCP requests.
class ReminderEvents {
public:
    ReminderEvents();
    ~ReminderEvents();
    bool addClient(std::uintptr_t socket, const std::string& websocketKey,
                   const std::string& initialState, std::string& error);
    void broadcast(const std::string& eventJson);
    void stop();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
