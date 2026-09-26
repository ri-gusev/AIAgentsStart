"""Local MCP server. Run it separately from the C++ backend."""

import json
from typing import Annotated
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen

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
def add(a: float, b: float) -> float:
    """Adds two numbers."""
    return a + b


@mcp.tool()
def echo(text: str) -> str:
    """Returns provided text."""
    return text


@mcp.tool()
def get_todo(id: Annotated[int, Field(ge=1)]) -> dict[str, int | str | bool]:
    """Get a todo item by ID from the public JSONPlaceholder REST API."""
    if id < 1:
        raise ToolError("Todo ID must be a positive integer")

    request = Request(
        f"https://jsonplaceholder.typicode.com/todos/{id}",
        headers={"Accept": "application/json", "User-Agent": "AI-Agent-Course-MCP/Day17"},
    )
    try:
        with urlopen(request, timeout=8) as response:
            todo = json.loads(response.read().decode("utf-8"))
    except HTTPError as error:
        if error.code == 404:
            raise ToolError(f"Todo with ID {id} was not found") from error
        raise ToolError(f"Todo API returned HTTP {error.code}") from error
    except (URLError, TimeoutError, json.JSONDecodeError, UnicodeDecodeError) as error:
        raise ToolError("Could not retrieve a valid todo from the public API") from error

    if not isinstance(todo, dict) or not isinstance(todo.get("id"), int) or \
            not isinstance(todo.get("title"), str) or not isinstance(todo.get("completed"), bool):
        raise ToolError("Todo API returned an invalid response")

    return {"id": todo["id"], "title": todo["title"], "completed": todo["completed"]}


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
