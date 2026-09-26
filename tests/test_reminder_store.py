from datetime import datetime, timedelta, timezone
from contextlib import closing
from pathlib import Path
import sqlite3
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "mcp_server"))
from reminder_store import ReminderStore


class ReminderStoreTest(unittest.TestCase):
    def test_unqualified_time_means_moscow_and_explicit_offsets_are_preserved(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "reminders.db"
            store = ReminderStore(path)
            moscow = timezone(timedelta(hours=3))
            future = (datetime.now(moscow) + timedelta(days=1)).replace(microsecond=0)
            values = [future.strftime("%Y-%m-%d %H:%M:%S"),
                      future.isoformat(), future.astimezone(timezone.utc).isoformat()]
            class ExplicitTimezoneDateTime(datetime):
                def astimezone(self, tz=None):
                    if self.tzinfo is None:
                        raise AssertionError("Naive clock must not use the server OS timezone")
                    return super().astimezone(tz)
            with patch("reminder_store.datetime", ExplicitTimezoneDateTime):
                results = [store.create("Moscow clock", value) for value in values]
            expected = future.astimezone(timezone.utc).isoformat().replace("+00:00", "Z")
            self.assertTrue(all(result["run_at"] == expected for result in results))
            with closing(sqlite3.connect(path)) as db:
                timestamps = db.execute("SELECT run_at FROM reminders").fetchall()
                self.assertEqual(timestamps, [(int(future.timestamp()),)] * 3)

    def test_registration_and_restart(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "reminders.db"
            store = ReminderStore(path)
            future = datetime.now(timezone(timedelta(hours=3))) + timedelta(minutes=2)
            reminder = store.create("  Пойти на тренировку  ", future.isoformat())
            self.assertEqual(reminder["status"], "pending")
            self.assertEqual(reminder["text"], "Пойти на тренировку")
            self.assertTrue(reminder["run_at"].endswith("Z"))
            ReminderStore(path)
            with closing(sqlite3.connect(path)) as db:
                row = db.execute("SELECT id,text,run_at,status,created_at,triggered_at FROM reminders").fetchone()
                self.assertEqual(row[0], reminder["id"])
                self.assertEqual(row[2], int(future.timestamp()))
                self.assertEqual(row[3], "pending")
                self.assertIsNone(row[5])
                self.assertEqual(db.execute("SELECT count(*) FROM reminder_notifications").fetchone()[0], 0)

    def test_invalid_inputs_do_not_create_records(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "reminders.db"
            store = ReminderStore(path)
            valid = (datetime.now(timezone.utc) + timedelta(minutes=2)).isoformat()
            for text, at in [("", valid), (" " * 3, valid), ("x" * 2001, valid),
                             ("test", "nonsense"), ("test", "2026-02-30 18:00:00"),
                             ("test", "2026-09-26"), ("test", "2000-01-01T00:00:00Z")]:
                with self.subTest(text=text[:10], at=at), self.assertRaises(ValueError):
                    store.create(text, at)
            with closing(sqlite3.connect(path)) as db:
                self.assertEqual(db.execute("SELECT count(*) FROM reminders").fetchone()[0], 0)


if __name__ == "__main__":
    unittest.main()
