#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct Reminder {
    std::int64_t id = 0;
    std::string text;
    std::int64_t runAt = 0;
    std::string status;
    std::int64_t createdAt = 0;
    std::int64_t triggeredAt = 0;
};

struct ReminderNotification {
    std::int64_t id = 0;
    std::int64_t reminderId = 0;
    std::string text;
    std::int64_t triggeredAt = 0;
};

class ReminderStore {
public:
    explicit ReminderStore(std::string databasePath = {},
                           const std::string& schemaPath = "mcp_server/reminders_schema.sql");
    bool isReady() const;
    const std::string& initializationError() const;
    bool snapshot(std::vector<Reminder>& reminders,
                  std::vector<ReminderNotification>& notifications, std::string& error) const;
    bool triggerDue(std::int64_t now, std::vector<ReminderNotification>& triggered,
                    std::string& error) const;
private:
    std::string databasePath_;
    std::string initializationError_;
};

std::string reminderUtcTime(std::int64_t timestamp);
