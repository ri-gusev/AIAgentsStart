"""Start the actual SDK server on 8001 with an isolated database.

Default: MCP initialization, tools/list and get_new_contests, no external HTTP.
--live-sync: additionally call sync_contests against the real Codeforces API.
"""

import argparse
import asyncio
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time

import mcp
from mcp import ClientSession
from mcp.client.streamable_http import streamable_http_client

ROOT = Path(__file__).resolve().parent.parent


async def verify(live_sync):
    async with streamable_http_client("http://127.0.0.1:8001/mcp") as (read, write, _):
        async with ClientSession(read, write) as session:
            initialized = await session.initialize()
            assert initialized.serverInfo.name == "Codeforces MCP Server"
            tools = await session.list_tools()
            assert {tool.name for tool in tools.tools} == {"sync_contests", "get_new_contests"}
            result = await session.call_tool("get_new_contests", {})
            assert not result.isError and result.structuredContent == {
                "last_sync_at": None, "new_count": 0, "contests": []}
            print("PASS: actual SDK server on 127.0.0.1:8001/mcp; initialize, tools/list, structured get_new_contests")
            if live_sync:
                result = await session.call_tool("sync_contests", {})
                if result.isError:
                    details = "; ".join(item.text for item in result.content if hasattr(item, "text"))
                    print("LIVE API UNAVAILABLE: " + details)
                    return False
                assert result.structuredContent["success"] and result.structuredContent["initialized"]
                assert result.structuredContent["new_count"] == 0
                delta = await session.call_tool("get_new_contests", {})
                assert not delta.isError and delta.structuredContent["new_count"] == 0
                print("PASS: real Codeforces API sync initialized the baseline without new-contest notifications")
    return True


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--live-sync", action="store_true")
    args = parser.parse_args()
    with socket.socket() as check:
        if check.connect_ex(("127.0.0.1", 8001)) == 0:
            raise RuntimeError("Port 8001 is occupied; existing server left untouched")
    with tempfile.TemporaryDirectory(prefix="codeforces-mcp-") as folder:
        env = os.environ.copy()
        env["CODEFORCES_DB"] = str(Path(folder) / "state.db")
        code = "import site,runpy;site.addsitedir(" + repr(str(Path(mcp.__file__).parent.parent)) + ");runpy.run_module('codeforces_mcp_server.server',run_name='__main__')"
        process = subprocess.Popen([sys.executable, "-s", "-c", code], cwd=ROOT, env=env,
                                   stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        try:
            for _ in range(100):
                if process.poll() is not None:
                    raise RuntimeError(process.stderr.read().decode(errors="replace"))
                with socket.socket() as check:
                    if check.connect_ex(("127.0.0.1", 8001)) == 0: break
                time.sleep(0.1)
            else:
                raise RuntimeError("Test MCP server did not start")
            return 0 if asyncio.run(verify(args.live_sync)) else 2
        finally:
            if process.poll() is None:
                process.terminate()
                try: process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
            process.stderr.close()


if __name__ == "__main__": sys.exit(main())
