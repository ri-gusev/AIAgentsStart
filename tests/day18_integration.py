"""Live local MCP + C++ + WebSocket + SQLite test. Uses isolated databases and no LLM calls.

Run with the MCP Python environment; --serve leaves the isolated app running for browser QA.
"""
import argparse
import base64
from datetime import datetime, timedelta, timezone
import hashlib
import json
import os
from pathlib import Path
import shutil
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen

ROOT = Path(__file__).resolve().parent.parent


def request(path, body=None):
    data = json.dumps(body).encode() if body is not None else None
    req = Request("http://127.0.0.1:8080" + path, data=data,
                  headers={"Content-Type": "application/json"})
    try:
        with urlopen(req, timeout=10) as response:
            return response.status, json.load(response)
    except HTTPError as response:
        return response.code, json.load(response)


class EventConnection:
    def __init__(self, host="127.0.0.1:8080"):
        self.socket = socket.create_connection(("127.0.0.1", 8080), timeout=8)
        self.buffer = b""
        key = base64.b64encode(os.urandom(16)).decode()
        self.socket.sendall(("GET /api/reminders/events HTTP/1.1\r\nHost: " + host + "\r\n"
            "Origin: http://" + host + "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
            "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: " + key + "\r\n\r\n").encode())
        while b"\r\n\r\n" not in self.buffer:
            self.buffer += self.socket.recv(4096)
        headers, self.buffer = self.buffer.split(b"\r\n\r\n", 1)
        accept = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest())
        assert b"101 Switching Protocols" in headers and accept in headers, headers

    def read(self, count):
        while len(self.buffer) < count:
            block = self.socket.recv(65536)
            if not block:
                raise RuntimeError("WebSocket closed before notification")
            self.buffer += block
        value, self.buffer = self.buffer[:count], self.buffer[count:]
        return value

    def receive(self):
        while True:
            header = self.read(2)
            length = header[1] & 127
            if length == 126:
                length = int.from_bytes(self.read(2), "big")
            elif length == 127:
                length = int.from_bytes(self.read(8), "big")
            payload = self.read(length)
            if header[0] & 15 == 1:
                return json.loads(payload)
            if header[0] & 15 == 9:
                self.send_control(10, payload)

    def send_control(self, opcode, payload):
        mask = os.urandom(4)
        encoded = bytes(value ^ mask[index % 4] for index, value in enumerate(payload))
        self.socket.sendall(bytes([0x80 | opcode, 0x80 | len(payload)]) + mask + encoded)

    def close(self):
        self.socket.close()


def wait_backend(process):
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError("Test backend exited: " + process.stderr.read().decode(errors="replace"))
        try:
            if request("/api/reminders")[0] == 200:
                return
        except (URLError, OSError):
            time.sleep(0.1)
    raise RuntimeError("Test backend did not start")


def stop(process):
    if process is not None and process.poll() is None:
        process.terminate()
        process.wait(timeout=10)
        if os.name != "nt" and process.returncode != 0:
            raise RuntimeError("Process did not shut down gracefully on SIGTERM")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--serve", action="store_true")
    parser.add_argument("--executable", type=Path, help="Override the backend executable for an alternate CMake build directory")
    args = parser.parse_args()
    # Refuse to interfere with an existing user app or MCP process.
    for port in (8080, 18000):
        with socket.socket() as check:
            if check.connect_ex(("127.0.0.1", port)) == 0:
                raise RuntimeError(f"Port {port} is occupied; existing processes were left untouched")
    folder = Path(tempfile.mkdtemp(prefix="day18-live-", dir=ROOT / "build"))
    for name in ("index.html", "styles.css", "app.js", "reminders.js"):
        shutil.copy2(ROOT / name, folder / name)
    shutil.copy2(ROOT / "agent_config.example.json", folder / "agent_config.local.json")
    (folder / "mcp_server").mkdir()
    shutil.copy2(ROOT / "mcp_server/reminders_schema.sql", folder / "mcp_server/reminders_schema.sql")
    env = os.environ.copy()
    env["OPENAI_API_KEY"] = "day18-local-test-placeholder"
    env["REMINDERS_DB"] = str(folder / "reminders.db")
    env["MCP_SERVER_URL"] = "http://127.0.0.1:18000/mcp"
    if os.name == "nt":
        env["PATH"] = "C:/msys64/ucrt64/bin;" + env.get("PATH", "")
    server_code = "import sys;sys.path.insert(0," + repr(str(ROOT)) + ");from mcp_server.server import mcp;mcp.settings.port=18000;mcp.run(transport='streamable-http')"
    mcp_process = subprocess.Popen([sys.executable, "-c", server_code], cwd=folder, env=env,
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    backend = None
    events = None
    try:
        executable = args.executable.resolve() if args.executable else ROOT / (
            "build/mingw-debug/openai_cli.exe" if os.name == "nt" else "build/openai_cli")
        backend = subprocess.Popen([str(executable)], cwd=folder, env=env,
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        wait_backend(backend)
        for path, content_type in (("/", "text/html"), ("/app.js", "application/javascript"),
                                   ("/reminders.js", "application/javascript"), ("/styles.css", "text/css")):
            with urlopen("http://127.0.0.1:8080" + path, timeout=5) as response:
                assert response.status == 200 and response.headers["Content-Type"].startswith(content_type)
        if sys.platform.startswith("linux"):
            listeners = Path("/proc/net/tcp").read_text().splitlines()[1:]
            assert any(line.split()[1] == "00000000:1F90" and line.split()[3] == "0A" for line in listeners), "Linux must listen on 0.0.0.0:8080"
        connected = False
        for _ in range(30):
            status, state = request("/api/mcp/connect", {})
            if status == 200:
                connected = True; break
            if mcp_process.poll() is not None:
                raise RuntimeError("MCP exited: " + mcp_process.stderr.read().decode(errors="replace"))
            time.sleep(0.2)
        assert connected, state
        assert {tool["name"] for tool in state["tools"]} >= {"add", "echo", "get_todo", "create_reminder"}
        tool = next(tool for tool in state["tools"] if tool["name"] == "create_reminder")
        assert set(tool["inputSchema"]["required"]) == {"text", "run_at"}
        assert all(tool["inputSchema"]["properties"][name]["type"] == "string" for name in ("text", "run_at"))
        print("LIVE READY", folder, flush=True)
        if args.serve:
            while True:
                time.sleep(1)
        events = EventConnection("203.0.113.7:8080" if sys.platform.startswith("linux") else "127.0.0.1:8080")
        assert events.receive()["type"] == "snapshot"
        status, created = request("/api/reminders", {
            "text": "Пойти на тренировку (integration)",
            "run_at": (datetime.now(timezone.utc) + timedelta(seconds=3)).isoformat(),
        })
        assert status == 200 and created["reminders"][0]["status"] == "pending", created
        assert created["mcp"]["calls"][-1]["name"] == "create_reminder" and created["mcp"]["calls"][-1]["success"]
        result = created["mcp"]["calls"][-1]["result"]
        assert result["id"] == created["reminders"][0]["id"] and result["status"] == "pending", result
        assert result["text"] == created["reminders"][0]["text"] and result["run_at"].endswith("Z"), result
        assert request("/api/state")[0] == 200, "WebSocket must not block normal HTTP"
        while True:
            event = events.receive()
            if event["notifications"]:
                break
        assert event["type"] == "triggered" and event["reminders"] == []
        assert len(event["notifications"]) == 1 and event["notifications"][0]["reminder_id"] == result["id"]
        with sqlite3.connect(folder / "reminders.db") as db:
            assert db.execute("SELECT count(*) FROM reminders").fetchone()[0] == 0
            assert db.execute("SELECT count(*) FROM sqlite_master WHERE name='reminder_notifications'").fetchone()[0] == 0
        assert request("/api/reminders")[1]["notifications"] == [], "No persisted completed-notification history"
        invalid, error = request("/api/reminders", {"text": "Invalid", "run_at": "2000-01-01T00:00:00Z"})
        assert invalid == 400 and "future" in error["error"], error
        status, cancelled = request("/api/reminders", {
            "text": "Cancel before due",
            "run_at": (datetime.now(timezone.utc) + timedelta(seconds=3)).isoformat(),
        })
        assert status == 200, cancelled
        cancelled_id = cancelled["reminders"][0]["id"]
        status, removed = request("/api/reminders/delete", {"id": str(cancelled_id)})
        assert status == 200 and removed["reminders"] == [], removed
        assert request("/api/reminders/delete", {"id": str(cancelled_id)})[0] == 400
        for invalid_id in ("0", "x", "1 OR 1=1", "999999999999999999999999999"):
            assert request("/api/reminders/delete", {"id": invalid_id})[0] == 400
        status, future = request("/api/reminders", {
            "text": "Survive backend restart",
            "run_at": (datetime.now(timezone.utc) + timedelta(seconds=5)).isoformat(),
        })
        assert status == 200 and future["reminders"][-1]["status"] == "pending"
        stop(mcp_process)  # Scheduling no longer depends on an active MCP connection.
        events.close(); events = None
        stop(backend)
        backend = subprocess.Popen([str(executable)], cwd=folder, env=env,
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        wait_backend(backend)
        events = EventConnection()
        state = events.receive()
        assert any(reminder["status"] == "pending" for reminder in state["reminders"]), state
        while not state["notifications"]:
            state = events.receive()
        assert state["reminders"] == [] and len(state["notifications"]) == 1
        assert state["notifications"][0]["reminder_id"] == future["reminders"][0]["id"]
        time.sleep(1.1)
        final = request("/api/reminders")[1]
        assert final["reminders"] == [] and final["notifications"] == []
        print("PASS: MCP creation -> pending SQLite -> transient WebSocket event -> row removed; deletion prevents triggering; restart, validation and no completed history", flush=True)
    finally:
        if events is not None:
            events.close()
        stop(backend); stop(mcp_process)


if __name__ == "__main__":
    main()
