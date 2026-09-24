"""Local MCP server. Run it separately from the C++ backend."""

import json
from typing import Annotated
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen

from pydantic import Field
from mcp.server.fastmcp import FastMCP
from mcp.server.fastmcp.exceptions import ToolError


mcp = FastMCP(
    "Local Tools Server",
    host="127.0.0.1",
    port=8000,
    streamable_http_path="/mcp",
    json_response=True,
)


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


if __name__ == "__main__":
    mcp.run(transport="streamable-http")
