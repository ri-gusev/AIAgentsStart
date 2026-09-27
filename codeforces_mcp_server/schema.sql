CREATE TABLE IF NOT EXISTS cf_contest_snapshot (
    contest_id INTEGER PRIMARY KEY,
    name TEXT NOT NULL,
    type TEXT NOT NULL,
    phase TEXT NOT NULL CHECK (phase = 'BEFORE'),
    start_time_seconds INTEGER,
    duration_seconds INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS cf_sync_state (
    id INTEGER PRIMARY KEY CHECK (id = 1),
    initialized INTEGER NOT NULL DEFAULT 0 CHECK (initialized IN (0, 1)),
    last_success_at INTEGER,
    last_attempt_at INTEGER,
    new_contests_json TEXT NOT NULL DEFAULT '[]',
    last_error TEXT
);

INSERT OR IGNORE INTO cf_sync_state(id) VALUES (1);
