"""Real two-server MCP transport + existing Agent with scripted model replies.

No OpenAI/Codeforces requests, no production database changes or UI server needed.
The Codeforces delta is an explicitly seeded test fixture, not live discoveries.
"""

import argparse
from contextlib import closing
import json
import os
from pathlib import Path
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time

import mcp

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
from codeforces_mcp_server.contest_store import ContestStore


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", type=Path)
    args = parser.parse_args()
    ports = (18020, 18021)
    for port in ports:
        with socket.socket() as check:
            if check.connect_ex(("127.0.0.1", port)) == 0:
                raise RuntimeError(f"Port {port} is occupied; user processes left untouched")
    with tempfile.TemporaryDirectory(prefix="day20-stage2-") as directory:
        folder = Path(directory)
        env = os.environ.copy()
        start = int(time.time()) + 25 * 3600
        env.update(REMINDERS_DB=str(folder / "reminders.db"), CODEFORCES_DB=str(folder / "cf.db"),
                   MCP_SERVER_URL=f"http://127.0.0.1:{ports[0]}/mcp",
                   CODEFORCES_MCP_SERVER_URL=f"http://127.0.0.1:{ports[1]}/mcp",
                   DAY20_CONTEST_START=str(start))
        if os.name == "nt": env["PATH"] = "C:/msys64/ucrt64/bin;" + env.get("PATH", "")
        ContestStore(folder / "cf.db")
        fixture = {"id": 9001, "name": "Day20 fixture contest (not live discovery)", "type": "CF",
                   "phase": "BEFORE", "start_time_seconds": start, "duration_seconds": 7200}
        with closing(sqlite3.connect(folder / "cf.db")) as db, db:
            db.execute("INSERT INTO cf_contest_snapshot VALUES(?,?,?,?,?,?)",
                       (9001, fixture["name"], "CF", "BEFORE", start, 7200))
            db.execute("UPDATE cf_sync_state SET initialized=1,last_success_at=?,last_attempt_at=?,new_contests_json=? WHERE id=1",
                       (int(time.time()), int(time.time()), json.dumps([fixture])))
        processes, logs = [], []
        try:
            for server_id, module, port in (
                ("reminder", "mcp_server.server", ports[0]),
                ("codeforces", "codeforces_mcp_server.server", ports[1]),
            ):
                code = (
                    "import site,sys,importlib;site.addsitedir(" + repr(str(Path(mcp.__file__).parent.parent)) + ");"
                    "sys.path.insert(0," + repr(str(ROOT)) + ");server=importlib.import_module(" + repr(module) + ");"
                    "server.mcp.settings.port=" + str(port) + "\n"
                    "def shared_name() -> dict[str,str]: return {'server':" + repr(server_id) + "}\n"
                    "server.mcp.tool()(shared_name)\nserver.mcp.run(transport='streamable-http')\n"
                )
                log = tempfile.TemporaryFile()
                logs.append(log)
                process = subprocess.Popen([sys.executable, "-s", "-c", code], cwd=folder, env=env,
                                           stdout=subprocess.DEVNULL, stderr=log)
                processes.append(process)
                for _ in range(100):
                    if process.poll() is not None:
                        log.seek(0)
                        raise RuntimeError(log.read().decode(errors="replace"))
                    with socket.socket() as check:
                        if check.connect_ex(("127.0.0.1", port)) == 0: break
                    time.sleep(0.1)
                else: raise RuntimeError("SDK fixture server did not start")
            executable = args.executable.resolve() if args.executable else ROOT / (
                "build/day20-tests/agent_tests.exe" if os.name == "nt" else "build/agent_tests")
            result = subprocess.run([str(executable), "--mcp-integration"], cwd=folder, env=env,
                                    capture_output=True, timeout=90)
            print(result.stdout.decode("utf-8", errors="replace"), end="")
            if result.returncode:
                raise RuntimeError(result.stderr.decode("utf-8", errors="replace"))
            with closing(sqlite3.connect(folder / "reminders.db")) as db:
                reminders = db.execute("SELECT text,run_at,status FROM reminders ORDER BY id").fetchall()
            assert len(reminders) == 2 and all(row == ("Day20 contest reminder", start - 86400, "pending") for row in reminders), reminders
            print("PASS: both Agent writes reached the Reminder database at contest start minus 24 hours; Codeforces state remained separate")
        finally:
            for process in processes:
                if process.poll() is None:
                    process.terminate()
                    try: process.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        process.kill(); process.wait(timeout=5)
            for log in logs: log.close()


if __name__ == "__main__": main()
