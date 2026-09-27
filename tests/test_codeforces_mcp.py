"""Offline Stage 1 tests: mocked HTTP, isolated SQLite and real MCP SDK."""

from contextlib import closing
import io
import json
import os
from pathlib import Path
import sqlite3
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch
from urllib.error import HTTPError, URLError

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from codeforces_mcp_server.codeforces_client import (
    CONTEST_LIST_URL, CodeforcesClient, CodeforcesAPIError, normalize_upcoming)
from codeforces_mcp_server.contest_store import ContestStore, codeforces_database_path


def contest(contest_id, **changes):
    value = {"id": contest_id, "name": f"Round {contest_id}", "type": "CF", "phase": "BEFORE",
             "startTimeSeconds": 1800000000 + contest_id, "durationSeconds": 7200}
    value.update(changes)
    return value


def fake_http(payload):
    return Mock(side_effect=lambda *args, **kwargs: io.BytesIO(json.dumps(payload).encode()))


class StoreTests(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.addCleanup(self.folder.cleanup)
        self.path = Path(self.folder.name) / "cf.db"
        self.now = 1000
        self.store = ContestStore(self.path, clock=lambda: self.now)
        self.client = Mock(spec=CodeforcesClient)

    def sync(self, *ids):
        self.client.upcoming_contests.side_effect = None
        self.client.upcoming_contests.return_value = normalize_upcoming([contest(i) for i in ids])
        self.now += 1
        return self.store.sync(self.client)

    def snapshot(self):
        with closing(sqlite3.connect(self.path)) as db:
            return list(db.execute("SELECT * FROM cf_contest_snapshot ORDER BY contest_id"))

    def state(self):
        with closing(sqlite3.connect(self.path)) as db:
            return db.execute("SELECT * FROM cf_sync_state WHERE id=1").fetchone()

    def test_initial_baseline(self):
        result = self.sync(1, 2, 3)
        self.assertTrue(result["success"] and result["initialized"])
        self.assertEqual(result["new_count"], 0)
        self.assertEqual(result["new_contests"], [])
        self.assertEqual(len(self.snapshot()), 3)

    def test_subsequent_sync(self):
        self.sync(1, 2, 3)
        result = self.sync(1, 2, 3, 4, 5)
        self.assertEqual([c["id"] for c in result["new_contests"]], [4, 5])
        self.assertEqual(result["new_count"], 2)
        delta = self.store.get_new_contests()
        self.assertEqual(delta["contests"], result["new_contests"])
        self.assertEqual(delta["last_sync_at"], result["checked_at"])

    def test_disappeared_contest(self):
        self.sync(1, 2, 3)
        self.assertEqual(self.sync(2, 3)["new_count"], 0)
        self.assertEqual([c[0] for c in self.snapshot()], [2, 3])

    def test_api_error_preserves_snapshot_and_delta(self):
        self.sync(1, 2, 3)
        self.sync(1, 2, 3, 4)
        snapshot, delta, success_at = self.snapshot(), self.store.get_new_contests(), self.state()[2]
        self.now += 10
        self.client.upcoming_contests.side_effect = CodeforcesAPIError("Codeforces API FAILED: maintenance")
        with self.assertRaises(CodeforcesAPIError): self.store.sync(self.client)
        self.assertEqual(self.snapshot(), snapshot)
        self.assertEqual(self.store.get_new_contests(), delta)
        state = self.state()
        self.assertEqual(state[2], success_at)
        self.assertEqual(state[3], self.now)
        self.assertIn("maintenance", state[5])
        self.sync(1, 2, 3, 4)
        self.assertIsNone(self.state()[5])

    def test_repeated_identical_sync(self):
        self.sync(1, 2, 3)
        self.sync(1, 2, 3, 4)
        self.assertEqual(self.sync(1, 2, 3, 4)["new_count"], 0)
        self.assertEqual(self.store.get_new_contests()["contests"], [])

    def test_new_id_below_old_max(self):
        self.sync(100, 200)
        self.assertEqual([c["id"] for c in self.sync(5, 100, 200)["new_contests"]], [5])

    def test_empty_baseline_is_initialized(self):
        self.sync()
        self.assertEqual(self.sync(1)["new_count"], 1)

    def test_empty_snapshot_after_removal_is_not_new_baseline(self):
        self.sync(1)
        self.sync()
        self.assertEqual(self.sync(2)["new_count"], 1)

    def test_failed_first_attempt_does_not_initialize(self):
        self.client.upcoming_contests.side_effect = CodeforcesAPIError("offline")
        with self.assertRaises(CodeforcesAPIError): self.store.sync(self.client)
        self.assertEqual(self.state()[1], 0)
        self.assertIsNone(self.store.get_new_contests()["last_sync_at"])
        self.assertEqual(self.sync(1, 2)["new_count"], 0)

    def test_reader_does_not_poll(self):
        self.sync(1)
        self.client.upcoming_contests.reset_mock()
        self.store.get_new_contests()
        self.store.get_new_contests()
        self.client.upcoming_contests.assert_not_called()

    def test_persisted_baseline_after_restart(self):
        self.sync(1, 2)
        self.store = ContestStore(self.path, clock=lambda: self.now)
        self.assertEqual([c["id"] for c in self.sync(1, 2, 3)["new_contests"]], [3])

    def test_bounded_state(self):
        for i in range(1, 101): self.sync(i)
        with closing(sqlite3.connect(self.path)) as db:
            self.assertEqual(db.execute("SELECT count(*) FROM cf_sync_state").fetchone()[0], 1)
            self.assertEqual(db.execute("SELECT count(*) FROM cf_contest_snapshot").fetchone()[0], 1)
            tables = {row[0] for row in db.execute("SELECT name FROM sqlite_master WHERE type='table'")}
            self.assertEqual(tables, {"cf_sync_state", "cf_contest_snapshot"})
        self.assertEqual([c["id"] for c in self.store.get_new_contests()["contests"]], [100])

    def test_transaction_rollback(self):
        self.sync(1, 2)
        snapshot, state = self.snapshot(), self.state()
        with closing(sqlite3.connect(self.path)) as db, db:
            db.execute("CREATE TRIGGER reject_three BEFORE INSERT ON cf_contest_snapshot "
                       "WHEN NEW.contest_id=3 BEGIN SELECT RAISE(ABORT, 'test rollback'); END")
        with self.assertRaises(sqlite3.IntegrityError): self.sync(2, 3)
        self.assertEqual(self.snapshot(), snapshot)
        self.assertEqual(self.state(), state)

    def test_database_env_override(self):
        with patch.dict(os.environ, {"CODEFORCES_DB": str(self.path)}):
            self.assertEqual(codeforces_database_path(), self.path.resolve())


class ClientTests(unittest.TestCase):
    def test_official_url_and_filter_optional_start(self):
        first = contest(2)
        del first["startTimeSeconds"]
        opener = fake_http({"status": "OK", "result": [first, contest(3, phase="FINISHED"), contest(1)]})
        result = CodeforcesClient(opener).upcoming_contests()
        args, kwargs = opener.call_args
        self.assertEqual(args[0].full_url, CONTEST_LIST_URL)
        self.assertEqual(kwargs["timeout"], 15)
        self.assertEqual([c["id"] for c in result], [1, 2])
        self.assertIsNone(result[1]["start_time_seconds"])
        self.assertEqual(set(result[0]), {"id", "name", "type", "phase", "start_time_seconds", "duration_seconds"})

    def test_failed_status(self):
        with self.assertRaisesRegex(CodeforcesAPIError, "FAILED: maintenance"):
            CodeforcesClient(fake_http({"status": "FAILED", "comment": "maintenance"})).upcoming_contests()

    def test_network_and_http_error(self):
        for error in (URLError("offline"), TimeoutError(), HTTPError(CONTEST_LIST_URL, 503, "Unavailable", {}, None)):
            with self.subTest(error=error), self.assertRaises(CodeforcesAPIError):
                CodeforcesClient(Mock(side_effect=error)).upcoming_contests()

    def test_invalid_json_and_response(self):
        with self.assertRaises(CodeforcesAPIError):
            CodeforcesClient(lambda *a, **k: io.BytesIO(b"not JSON")).upcoming_contests()
        for payload in ([], {"status": "UNKNOWN"}, {"status": "OK", "result": None}, {"status": "OK", "result": [{}]}):
            with self.subTest(payload=payload), self.assertRaises(CodeforcesAPIError):
                CodeforcesClient(fake_http(payload)).upcoming_contests()

    def test_invalid_contest_fields(self):
        for change in ({"id": True}, {"id": -1}, {"durationSeconds": "7200"},
                       {"startTimeSeconds": "later"}, {"name": None}, {"type": ""}):
            with self.subTest(change=change), self.assertRaises(CodeforcesAPIError):
                normalize_upcoming([contest(1, **change)])
        with self.assertRaises(CodeforcesAPIError): normalize_upcoming([contest(1), contest(1)])


class SDKTests(unittest.IsolatedAsyncioTestCase):
    async def test_tools_registered_and_structured_calls(self):
        # Import the real server with an isolated DB; do not touch default state.
        with tempfile.TemporaryDirectory() as folder, patch.dict(os.environ, {"CODEFORCES_DB": str(Path(folder) / "sdk.db")}):
            import importlib
            server = importlib.import_module("codeforces_mcp_server.server")
            old_store, old_client = server.contest_store, server.codeforces_client
            server.contest_store = ContestStore(Path(folder) / "sdk.db")
            server.codeforces_client = CodeforcesClient(fake_http({"status": "OK", "result": [contest(1)]}))
            try:
                tools = await server.mcp.list_tools()
                self.assertEqual({tool.name for tool in tools}, {"sync_contests", "get_new_contests"})
                self.assertTrue(all(tool.inputSchema["type"] == "object" for tool in tools))
                self.assertIn("hourly", next(t for t in tools if t.name == "get_new_contests").description)
                baseline = await server.mcp.call_tool("sync_contests", {})
                self.assertEqual(baseline[1]["new_count"], 0)
                delta = await server.mcp.call_tool("get_new_contests", {})
                self.assertEqual(delta[1]["contests"], [])
                server.codeforces_client = CodeforcesClient(fake_http({"status": "FAILED", "comment": "test failure"}))
                with self.assertRaisesRegex(Exception, "test failure"):
                    await server.mcp.call_tool("sync_contests", {})
            finally:
                server.contest_store, server.codeforces_client = old_store, old_client


if __name__ == "__main__": unittest.main()
