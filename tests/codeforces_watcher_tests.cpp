#include "codeforces_watcher.h"
#include "json_value.h"
#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace std::chrono_literals;
int main() {
    try {
        std::atomic<int> calls{0}, events{0};
        CodeforcesContestWatcher watcher([&](std::string& result, std::string& error) {
            const int call = ++calls;
            if (call == 3) { error = "API unavailable"; return false; }
            result = call == 2
                ? R"({"success":true,"new_count":1,"new_contests":[{"id":5,"name":"Round"}],"checked_at":100})"
                : R"({"success":true,"new_count":0,"new_contests":[]})";
            return true;
        }, [&](const std::string& event) {
            app_json::JsonValue parsed; std::string error;
            if (!app_json::JsonParser(event).parse(parsed, error) ||
                !parsed.member("type") || parsed.member("type")->text != "codeforces_contests")
                throw std::runtime_error("Invalid event");
            ++events;
        }, 100ms);
        watcher.start(); watcher.start();
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (calls < 3 && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(5ms);
        std::this_thread::sleep_for(10ms);
        if (calls != 3 || events != 1 || watcher.lastError() != "API unavailable")
            throw std::runtime_error("Baseline/duplicate start/delta/error handling failed");
        const auto beforeStop = std::chrono::steady_clock::now();
        watcher.stop(); watcher.stop();
        if (std::chrono::steady_clock::now() - beforeStop > 100ms) throw std::runtime_error("Slow shutdown");
        const int stoppedCalls = calls;
        std::this_thread::sleep_for(150ms);
        if (calls != stoppedCalls) throw std::runtime_error("Polling after stop");
        std::atomic<int> invalidEvents{0};
        CodeforcesContestWatcher invalid([](std::string& result, std::string&) {
            result = R"({"success":true,"new_count":2,"new_contests":[]})"; return true;
        }, [&](const std::string&) { ++invalidEvents; }, 1h);
        invalid.start();
        for (int i=0; i<100 && invalid.lastError().empty(); ++i) std::this_thread::sleep_for(5ms);
        invalid.stop();
        if (invalidEvents || invalid.lastError().empty()) throw std::runtime_error("Malformed result accepted");
        std::cout << "PASS: baseline, delta event, hourly wait, errors, idempotent start/stop and shutdown\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
