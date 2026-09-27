#pragma once
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

class McpManager;
class CodeforcesContestWatcher {
public:
    using Sync = std::function<bool(std::string&, std::string&)>;
    using Publish = std::function<void(const std::string&)>;
    CodeforcesContestWatcher(McpManager& manager, Publish publish);
    // Injected transport/interval for offline tests; production always uses one hour.
    CodeforcesContestWatcher(Sync sync, Publish publish,
        std::chrono::milliseconds interval = std::chrono::hours(1));
    ~CodeforcesContestWatcher();
    void start();
    void stop();
    std::string lastError() const;
private:
    void run();
    Sync sync_;
    Publish publish_;
    std::chrono::milliseconds interval_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    bool stopped_ = false;
    std::string error_;
    std::thread worker_;
};
