"""Local MCP server. Run it separately from the C++ backend."""

from typing import Annotated

from pydantic import Field
from mcp.server.fastmcp import FastMCP
from mcp.server.fastmcp.exceptions import ToolError

try:
    from .reminder_store import ReminderStore
except ImportError:
    from reminder_store import ReminderStore


mcp = FastMCP(
    "Local Tools Server",
    host="127.0.0.1",
    port=8000,
    streamable_http_path="/mcp",
    json_response=True,
)
reminder_store = ReminderStore()


@mcp.tool()
def create_reminder(
    text: Annotated[str, Field(min_length=1, max_length=2000, description="Text of the reminder")],
    run_at: Annotated[str, Field(description="Future date/time; without an offset interpreted as Moscow time (MSK, UTC+03:00)")],
) -> dict[str, int | str]:
    """Schedule a reminder. run_at is an ISO date/time with offset, or Moscow YYYY-MM-DD HH:MM:SS (MSK, UTC+03:00). Returns immediately with pending status."""
    try:
        return reminder_store.create(text, run_at)
    except ValueError as error:
        raise ToolError(str(error)) from error
    except Exception as error:
        raise ToolError("Could not save reminder in SQLite") from error


if __name__ == "__main__":
    mcp.run(transport="streamable-http")
