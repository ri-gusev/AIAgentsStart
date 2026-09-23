"""Local Day 16 MCP server. Run it separately from the C++ backend."""

from mcp.server.fastmcp import FastMCP


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


if __name__ == "__main__":
    mcp.run(transport="streamable-http")
