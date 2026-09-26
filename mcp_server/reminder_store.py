"""Registration of reminders; the C++ backend owns the background scheduler."""

from datetime import datetime, timedelta, timezone
from contextlib import closing
import os
from pathlib import Path
import re
import sqlite3
import time

MOSCOW_TIMEZONE = timezone(timedelta(hours=3), "MSK")

def reminder_database_path() -> Path:
    default = Path(__file__).resolve().parent.parent / "reminders.db"
    return Path(os.environ.get("REMINDERS_DB") or default).resolve()


class ReminderStore:
    def __init__(self, path: Path | str | None = None):
        self.path = Path(path) if path is not None else reminder_database_path()
        schema = Path(__file__).with_name("reminders_schema.sql").read_text(encoding="utf-8")
        with closing(self._connect()) as db, db:
            db.executescript(schema)

    def _connect(self):
        db = sqlite3.connect(self.path, timeout=5)
        db.execute("PRAGMA foreign_keys = ON")
        return db

    def create(self, text: str, run_at: str) -> dict:
        if not isinstance(text, str) or not text.strip() or len(text.strip()) > 2000:
            raise ValueError("Reminder text must contain 1 to 2000 characters")
        if not isinstance(run_at, str) or not re.match(
            r"^\d{4}-\d{2}-\d{2}[ T]\d{2}:\d{2}(?::\d{2}(?:\.\d+)?)?(?:Z|[+-]\d{2}:\d{2})?$", run_at
        ):
            raise ValueError("run_at must be a date and time, e.g. 2026-09-26T18:00:00+03:00")
        try:
            moment = datetime.fromisoformat(run_at.replace("Z", "+00:00"))
            # Unqualified clock times always mean Moscow, not the server OS timezone.
            if moment.tzinfo is None:
                moment = moment.replace(tzinfo=MOSCOW_TIMEZONE)
            timestamp = int(moment.astimezone(timezone.utc).timestamp())
        except (ValueError, OverflowError, OSError) as error:
            raise ValueError("run_at is not a valid date and time") from error
        now = int(time.time())
        if timestamp <= now:
            raise ValueError("run_at must be in the future")
        with closing(self._connect()) as db, db:
            cursor = db.execute(
                "INSERT INTO reminders(text, run_at, status, created_at) VALUES (?, ?, 'pending', ?)",
                (text.strip(), timestamp, now),
            )
            reminder_id = cursor.lastrowid
        return {
            "id": reminder_id, "text": text.strip(),
            "run_at": datetime.fromtimestamp(timestamp, timezone.utc).isoformat().replace("+00:00", "Z"),
            "status": "pending",
        }
