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
-- Migrate earlier Day 18 history: only scheduled reminders are retained.
BEGIN IMMEDIATE;
DROP TABLE IF EXISTS reminder_notifications;
DELETE FROM reminders WHERE status = 'triggered';
COMMIT;
