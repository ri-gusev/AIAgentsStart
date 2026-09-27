# Day 20 — Stage 1: Codeforces MCP Server

Independent SDK server at `http://127.0.0.1:8001/mcp`. It does not change the Reminder MCP server, C++ Agent, Day 19 pipeline, Scheduler, WebSocket or UI. No LLM, background polling or multi-server router is added in Stage 1.

## Tools and state

- `sync_contests()` calls the public official `https://codeforces.com/api/contest.list?gym=false`, checks `OK`/`FAILED`, keeps `phase == BEFORE`, and atomically replaces the current snapshot and latest delta.
- `get_new_contests()` reads the latest successful delta from SQLite without making an API request. `last_sync_at` is the application's polling timestamp, not Codeforces publication time.

First successful sync initializes a baseline and returns zero new contests, even when the API already contains many contests. Later syncs compare sets of IDs, never `max(id)`. An initialized empty snapshot remains initialized. Contests that disappear are removed from the snapshot; a contest that later reappears is considered newly observed relative to the preceding snapshot, since no historical ID ledger is stored.

Only two tables: `cf_contest_snapshot` contains the current upcoming contests; `cf_sync_state` contains exactly one row and only the latest successful delta. A failed API request updates `last_attempt_at` and `last_error` but preserves the previous snapshot, delta, initialization flag and successful timestamp. Missing optional `startTimeSeconds` is stored and returned as `null`. SQLite can retain reusable pages from its largest snapshot, but no rows/history are accumulated.

Default database: `codeforces_state.db` in the project root. Override with `CODEFORCES_DB`. No API key is required. Hourly scheduling will be implemented in Stage 3; in Stage 1 sync is explicit only.

## Windows PowerShell (project root)

Use a working MCP environment, or create a separate one:

```powershell
py -3.11 -m venv .venv-codeforces
.\.venv-codeforces\Scripts\python.exe -m pip install -r codeforces_mcp_server\requirements.txt
.\.venv-codeforces\Scripts\python.exe codeforces_mcp_server\server.py
```

Existing dependencies can also be used without changing broken venv launchers:

```powershell
py -3.11 -s -c "import site,runpy;site.addsitedir(r'.venv-mcp-run\Lib\site-packages');runpy.run_module('codeforces_mcp_server.server',run_name='__main__')"
```

Optional database override (set before launching):

```powershell
$env:CODEFORCES_DB = 'D:\AIAgentsCourse\codeforces_state.db'
```

## Ubuntu (project root)

```bash
python3 -m venv .venv-codeforces
.venv-codeforces/bin/python -m pip install -r codeforces_mcp_server/requirements.txt
.venv-codeforces/bin/python codeforces_mcp_server/server.py
```

Optional override: `export CODEFORCES_DB=/path/to/codeforces_state.db`.

## Checks

```powershell
.\.venv-codeforces\Scripts\python.exe tests\test_codeforces_mcp.py
.\.venv-codeforces\Scripts\python.exe tests\codeforces_mcp_smoke.py
# Optional actual external API request:
.\.venv-codeforces\Scripts\python.exe tests\codeforces_mcp_smoke.py --live-sync
```

On Ubuntu replace the interpreter with `.venv-codeforces/bin/python`.

Unit tests use mocked HTTP and temporary databases, never real Codeforces requests. The smoke test starts its own SDK server on 8001, initializes an SDK client, discovers exactly two tools, and calls `get_new_contests`. Port 8001 must be free; existing processes are never stopped. `--live-sync` additionally calls the real API once against a temporary database, verifying a zero-delta baseline. Exit code 2 means the external API could not be reached or returned an error; it is not a successful live API test.

The existing Web UI and C++ client still address only Reminder MCP until Stage 2 is explicitly approved.
