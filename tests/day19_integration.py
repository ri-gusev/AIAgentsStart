"""Real HTTP -> C++ pipeline -> three SDK tools/call; isolated SQLite, no LLM.

Run with the MCP Python environment. Requires free ports 8080 and 18019.
"""
import argparse
from datetime import datetime, timedelta, timezone
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import time

import mcp
from day18_integration import ROOT, request, wait_backend, stop

STEPS = ["get_upcoming_reminders", "summarize_reminders", "build_reminder_view"]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", type=Path)
    args = parser.parse_args()
    for port in (8080, 18019):
        with socket.socket() as check:
            if check.connect_ex(("127.0.0.1", port)) == 0:
                raise RuntimeError(f"Port {port} is occupied; existing processes left untouched")
    with tempfile.TemporaryDirectory(prefix="day19-live-", dir=ROOT / "build") as directory:
        folder = Path(directory)
        shutil.copy2(ROOT / "agent_config.example.json", folder / "agent_config.local.json")
        (folder / "mcp_server").mkdir()
        shutil.copy2(ROOT / "mcp_server/reminders_schema.sql", folder / "mcp_server/reminders_schema.sql")
        env = os.environ.copy()
        env.update(OPENAI_API_KEY="day19-local-test-placeholder", REMINDERS_DB=str(folder / "reminders.db"),
                   MCP_SERVER_URL="http://127.0.0.1:18019/mcp")
        if os.name == "nt": env["PATH"] = "C:/msys64/ucrt64/bin;" + env.get("PATH", "")
        executable = args.executable.resolve() if args.executable else ROOT / (
            "build/mingw-debug/openai_cli.exe" if os.name == "nt" else "build/openai_cli")
        backend = subprocess.Popen([str(executable)], cwd=folder, env=env,
                                   stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        server = None
        try:
            wait_backend(backend)
            status, result = request("/api/reminders/overview")
            assert status == 502 and result["failed_step"] == STEPS[0] and not result["success"], result
            for failure in (None, *STEPS):
                code = (
                    "import site,sys;site.addsitedir(" + repr(str(Path(mcp.__file__).parent.parent)) + ");"
                    "sys.path.insert(0," + repr(str(ROOT)) + ");"
                    "import mcp_server.server as server;from mcp.server.fastmcp.exceptions import ToolError;"
                    "server.mcp.settings.port=18019\n"
                    "def fail(*args,**kwargs): raise ValueError('Injected pipeline failure')\n"
                )
                target = {STEPS[0]: "server.reminder_store.upcoming", STEPS[1]: "server.summarize", STEPS[2]: "server.build_view"}
                if failure: code += target[failure] + "=fail\n"
                code += "server.mcp.run(transport='streamable-http')\n"
                server = subprocess.Popen([sys.executable, "-s", "-c", code], cwd=folder, env=env,
                                          stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
                for _ in range(50):
                    status, state = request("/api/mcp/connect", {})
                    if status == 200: break
                    if server.poll() is not None:
                        raise RuntimeError(server.stderr.read().decode(errors="replace"))
                    time.sleep(0.1)
                assert status == 200, state
                assert {t["name"] for t in state["tools"]} == {"create_reminder", *STEPS}
                schemas = {t["name"]: t["inputSchema"] for t in state["tools"]}
                for tool, parameter, default in ((STEPS[0], "days", 30), (STEPS[1], "hours", 24)):
                    prop = schemas[tool]["properties"][parameter]
                    assert prop["type"] == "integer" and prop["minimum"] == 1 and prop["default"] == default, prop
                assert "reminders" in schemas[STEPS[1]]["required"]
                assert "summary" in schemas[STEPS[2]]["required"]
                if failure is None:
                    status, empty = request("/api/reminders/overview")
                    assert status == 200 and empty["result"]["count"] == 0, empty
                    due = datetime.now(timezone.utc).replace(microsecond=0) + timedelta(hours=1)
                    text = 'Тренировка "утром"'
                    status, created = request("/api/reminders", {"text": text, "run_at": due.isoformat()})
                    assert status == 200 and created["mcp"]["calls"][-1]["success"], created
                    reminder_id = created["reminders"][0]["id"]
                    for hours in (30, 31 * 24):
                        status, _ = request("/api/reminders", {"text": "outside", "run_at": (due + timedelta(hours=hours)).isoformat()})
                        assert status == 200
                status, result = request("/api/reminders/overview")
                _, state = request("/api/mcp/tools")
                if failure is None:
                    assert status == 200 and result["success"] and result["steps"] == STEPS, result
                    view = result["result"]
                    local = due.astimezone(timezone(timedelta(hours=3)))
                    assert view["count"] == 1 and view["items"] == [{
                        "id": reminder_id, "title": text, "date": local.strftime("%d.%m.%Y"), "time": local.strftime("%H:%M")}], view
                    calls = state["calls"][-3:]
                    assert [call["name"] for call in calls] == STEPS and all(call["success"] for call in calls)
                    # Actual MCP arguments contain the complete preceding structured result.
                    assert json.loads(calls[1]["arguments"])["reminders"] == calls[0]["result"]
                    assert json.loads(calls[2]["arguments"])["summary"] == calls[1]["result"]
                    assert json.loads(calls[0]["arguments"]) == {"days": 30}
                    assert len(calls[0]["result"]["reminders"]) == 2
                    assert json.loads(calls[1]["arguments"])["hours"] == 24
                else:
                    assert status == 502 and not result["success"] and result["failed_step"] == failure, result
                    assert "Injected pipeline failure" in result["error"], result
                    expected = STEPS[:STEPS.index(failure) + 1]
                    assert [c["name"] for c in state["calls"]] == expected, state["calls"]
                    assert not state["calls"][-1]["success"]
                request("/api/mcp/disconnect", {})
                stop(server)
                server = None
            print("Day 19: SDK discovery, real three-call HTTP pipeline, data forwarding, Moscow view, empty result, create_reminder and all failure steps passed")
        finally:
            stop(server)
            stop(backend)


if __name__ == "__main__": main()
