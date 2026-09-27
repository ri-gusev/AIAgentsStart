"""Day 19 data access, pure processing and Moscow presentation tests."""
from contextlib import closing
from datetime import datetime, timedelta, timezone
from pathlib import Path
import sqlite3
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from mcp_server.reminder_store import ReminderStore
from mcp_server.reminder_composition import UpcomingReminders, ReminderSummary, summarize, build_view


class CompositionTests(unittest.TestCase):
    def test_upcoming_reads_existing_database(self):
        with tempfile.TemporaryDirectory() as folder:
            store = ReminderStore(Path(folder) / "reminders.db")
            with closing(store._connect()) as db, db:
                for text, timestamp, status in [
                    ("later", 1000 + 86400, "pending"), ("first", 1001, "pending"),
                    ("past", 999, "pending"), ("now", 1000, "pending"),
                    ("out", 1000 + 30 * 86400 + 1, "pending"),
                    ("done", 1002, "triggered"), ("boundary", 1000 + 30 * 86400, "pending")
                ]:
                    db.execute("INSERT INTO reminders(text,run_at,status,created_at,triggered_at) VALUES(?,?,?,?,?)",
                               (text, timestamp, status, 900, 1002 if status == "triggered" else None))
            with patch("mcp_server.reminder_store.time.time", return_value=1000):
                result = store.upcoming()
            self.assertEqual(result["range_days"], 30)
            self.assertEqual([r["text"] for r in result["reminders"]], ["first", "later", "boundary"])
            self.assertTrue(all(r["status"] == "pending" and r["run_at"].endswith("Z") for r in result["reminders"]))
            for days in (0, -1, True, "30"):
                with self.assertRaises(ValueError): store.upcoming(days)

    def test_processing_and_view_do_not_access_sqlite(self):
        now = datetime.now(timezone.utc)
        def item(i, hours, status="pending"):
            return {"id": i, "text": f"reminder {i}", "run_at": (now + timedelta(hours=hours)).isoformat(), "status": status}
        source = UpcomingReminders(range_days=30, reminders=[item(2, 3), item(1, 1), item(3, 25), item(4, -1), item(5, 2, "triggered")])
        with patch.object(sqlite3, "connect", side_effect=AssertionError("Pure tools accessed SQLite")):
            summary = summarize(source)
            view = build_view(ReminderSummary.model_validate(summary))
        self.assertEqual(summary["count"], 2)
        self.assertEqual([i["id"] for i in summary["items"]], [1, 2])
        self.assertEqual(view["count"], 2)
        self.assertEqual([i["title"] for i in view["items"]], ["reminder 1", "reminder 2"])
        self.assertNotIn("<", view["message"])

    def test_moscow_date_rollover_and_empty_result(self):
        source = {"period_hours": 24, "count": 1, "items": [
            {"id": 1, "text": "night", "run_at": "2026-09-27T22:30:00Z"}]}
        view = build_view(ReminderSummary.model_validate(source))
        self.assertEqual(view["items"][0], {"id": 1, "title": "night", "date": "28.09.2026", "time": "01:30"})
        empty = summarize(UpcomingReminders(range_days=30, reminders=[]))
        self.assertEqual(empty, {"period_hours": 24, "count": 0, "items": []})
        self.assertEqual(build_view(ReminderSummary.model_validate(empty))["count"], 0)
        source["count"] = 2
        with self.assertRaises(ValueError): build_view(ReminderSummary.model_validate(source))

    def test_bad_time_and_period(self):
        source = UpcomingReminders(range_days=30, reminders=[
            {"id": 1, "text": "bad", "run_at": "not-a-date", "status": "pending"}])
        with self.assertRaises(ValueError): summarize(source)
        for hours in (0, -1, True):
            with self.assertRaises(ValueError): summarize(source, hours)


if __name__ == "__main__": unittest.main()
