"""Local MCP server. Run it separately from the C++ backend."""

from typing import Annotated, Any

from pydantic import Field
from mcp.server.fastmcp import FastMCP
from mcp.server.fastmcp.exceptions import ToolError

try:
    from .reminder_store import ReminderStore
    from .reminder_composition import PositiveInteger, UpcomingReminders, ReminderSummary, summarize, build_view
except ImportError:
    from reminder_store import ReminderStore
    from reminder_composition import PositiveInteger, UpcomingReminders, ReminderSummary, summarize, build_view


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


@mcp.tool()
def get_upcoming_reminders(days: PositiveInteger = 30) -> dict[str, Any]:
    """Read future pending reminders from the existing SQLite database, sorted by run_at, for the next days (default 30)."""
    try:
        return reminder_store.upcoming(days)
    except (ValueError, OverflowError) as error:
        raise ToolError(str(error)) from error
    except Exception as error:
        raise ToolError("Could not read reminders from SQLite") from error


@mcp.tool()
def summarize_reminders(reminders: UpcomingReminders, hours: PositiveInteger = 24) -> dict[str, Any]:
    """Accept the complete get_upcoming_reminders result as reminders; select and count upcoming items within hours (default 24). No database access."""
    try:
        return summarize(reminders, hours)
    except (ValueError, OverflowError) as error:
        raise ToolError(str(error)) from error


@mcp.tool()
def build_reminder_view(summary: ReminderSummary) -> dict[str, Any]:
    """Convert the complete summarize_reminders result into presentation JSON with Moscow date/time. No database access or HTML."""
    try:
        return build_view(summary)
    except (ValueError, OverflowError) as error:
        raise ToolError(str(error)) from error


if __name__ == "__main__":
    mcp.run(transport="streamable-http")
