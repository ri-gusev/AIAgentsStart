PRAGMA journal_mode = WAL;
PRAGMA foreign_keys = ON;
CREATE TABLE IF NOT EXISTS reminders (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    text TEXT NOT NULL CHECK(length(text) BETWEEN 1 AND 2000),
    run_at INTEGER NOT NULL,
    status TEXT NOT NULL DEFAULT 'pending' CHECK(status IN ('pending', 'triggered')),
    created_at INTEGER NOT NULL,
    triggered_at INTEGER,
    CHECK((status = 'pending' AND triggered_at IS NULL) OR
          (status = 'triggered' AND triggered_at IS NOT NULL))
);
CREATE INDEX IF NOT EXISTS reminders_due ON reminders(status, run_at);
CREATE TABLE IF NOT EXISTS reminder_notifications (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    reminder_id INTEGER NOT NULL UNIQUE REFERENCES reminders(id),
    created_at INTEGER NOT NULL
);
