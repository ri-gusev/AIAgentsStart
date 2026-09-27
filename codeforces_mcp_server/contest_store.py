"""Bounded snapshot and latest sync delta; no historical contest IDs."""

from contextlib import closing
import json
import os
from pathlib import Path
import sqlite3
import threading
import time

try:
    from .codeforces_client import CodeforcesAPIError
except ImportError:
    from codeforces_client import CodeforcesAPIError


def codeforces_database_path() -> Path:
    default = Path(__file__).resolve().parent.parent / "codeforces_state.db"
    return Path(os.environ.get("CODEFORCES_DB") or default).resolve()


class ContestStore:
    def __init__(self, path: Path | str | None = None, clock=time.time):
        self.path = Path(path) if path is not None else codeforces_database_path()
        self.clock = clock
        # Serialize API polling + commit within this server, including errors.
        self._sync_lock = threading.Lock()
        schema = Path(__file__).with_name("schema.sql").read_text(encoding="utf-8")
        with closing(self._connect()) as db, db:
            db.executescript(schema)

    def _connect(self):
        return sqlite3.connect(self.path, timeout=5)

    def sync(self, client) -> dict:
        with self._sync_lock:
            checked_at = int(self.clock())
            try:
                upcoming = client.upcoming_contests()
            except CodeforcesAPIError as error:
                # Only attempt/error metadata changes. Snapshot and successful
                # delta remain available even when the public API is down.
                with closing(self._connect()) as db, db:
                    db.execute("UPDATE cf_sync_state SET last_attempt_at=?, last_error=? WHERE id=1",
                               (checked_at, str(error)[:2000]))
                raise
            with closing(self._connect()) as db, db:
                db.execute("BEGIN IMMEDIATE")
                initialized = bool(db.execute("SELECT initialized FROM cf_sync_state WHERE id=1").fetchone()[0])
                previous_ids = {row[0] for row in db.execute("SELECT contest_id FROM cf_contest_snapshot")}
                new_contests = [item for item in upcoming if item["id"] not in previous_ids] if initialized else []
                db.execute("DELETE FROM cf_contest_snapshot")
                db.executemany(
                    "INSERT INTO cf_contest_snapshot(contest_id,name,type,phase,start_time_seconds,duration_seconds) "
                    "VALUES(?,?,?,?,?,?)",
                    [(item["id"], item["name"], item["type"], item["phase"], item["start_time_seconds"],
                      item["duration_seconds"]) for item in upcoming],
                )
                db.execute(
                    "UPDATE cf_sync_state SET initialized=1,last_success_at=?,last_attempt_at=?,"
                    "new_contests_json=?,last_error=NULL WHERE id=1",
                    (checked_at, checked_at, json.dumps(new_contests, ensure_ascii=False)),
                )
            return {"success": True, "initialized": True, "checked_at": checked_at,
                    "new_count": len(new_contests), "new_contests": new_contests}

    def get_new_contests(self) -> dict:
        with closing(self._connect()) as db:
            last_sync_at, delta = db.execute(
                "SELECT last_success_at,new_contests_json FROM cf_sync_state WHERE id=1").fetchone()
        contests = json.loads(delta)
        return {"last_sync_at": last_sync_at, "new_count": len(contests), "contests": contests}
