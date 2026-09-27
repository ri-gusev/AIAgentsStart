"""Independent Codeforces MCP server. No Agent, LLM or background watcher."""

import sqlite3
from typing import Any

from mcp.server.fastmcp import FastMCP
from mcp.server.fastmcp.exceptions import ToolError

try:
    from .codeforces_client import CodeforcesClient, CodeforcesAPIError
    from .contest_store import ContestStore
except ImportError:
    from codeforces_client import CodeforcesClient, CodeforcesAPIError
    from contest_store import ContestStore

mcp = FastMCP("Codeforces MCP Server", host="127.0.0.1", port=8001,
              streamable_http_path="/mcp", json_response=True)
contest_store = ContestStore()
codeforces_client = CodeforcesClient()


@mcp.tool()
def sync_contests() -> dict[str, Any]:
    """Poll the official public Codeforces contest.list API and replace the upcoming snapshot. First sync creates a baseline with no new contests. Later syncs return IDs absent from the previous snapshot; no publication time is inferred."""
    try:
        return contest_store.sync(codeforces_client)
    except CodeforcesAPIError as error:
        raise ToolError(str(error)) from error
    except sqlite3.Error as error:
        raise ToolError("Could not update Codeforces SQLite state") from error


@mcp.tool()
def get_new_contests() -> dict[str, Any]:
    """Return Codeforces contests first discovered by this system during its latest successful hourly check. Reads the saved delta only, with no Codeforces API request. last_sync_at is our polling time, NOT an official Codeforces publication time. Hourly scheduling is provided by the backend watcher, not this tool."""
    try:
        return contest_store.get_new_contests()
    except (sqlite3.Error, ValueError) as error:
        raise ToolError("Could not read Codeforces SQLite state") from error


if __name__ == "__main__":
    mcp.run(transport="streamable-http")
