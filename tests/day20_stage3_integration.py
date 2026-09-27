"""Isolated real SDK -> Manager -> hourly watcher -> existing WebSocket test.

No OpenAI or public Codeforces requests. The API client is replaced only in this
test subprocess. Production uses the official API. Ports 8080/18023 must be free.
"""
import json
from contextlib import closing
import os
from pathlib import Path
import shutil
import signal
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time
import mcp
from day18_integration import ROOT, EventConnection, wait_backend, stop


def main():
    for port in (8080, 18023):
        with socket.socket() as check:
            if check.connect_ex(('127.0.0.1', port)) == 0:
                raise RuntimeError(f'Port {port} occupied; existing processes untouched')
    with tempfile.TemporaryDirectory(prefix='day20-stage3-', dir=ROOT / 'build') as directory:
        folder = Path(directory)
        shutil.copy2(ROOT / 'agent_config.example.json', folder / 'agent_config.local.json')
        (folder / 'mcp_server').mkdir()
        shutil.copy2(ROOT / 'mcp_server/reminders_schema.sql', folder / 'mcp_server/reminders_schema.sql')
        env = os.environ.copy()
        env.update(OPENAI_API_KEY='offline-test-only', CODEFORCES_DB=str(folder / 'cf.db'),
                   REMINDERS_DB=str(folder / 'reminders.db'),
                   MCP_SERVER_URL='http://127.0.0.1:1/mcp',
                   CODEFORCES_MCP_SERVER_URL='http://127.0.0.1:18023/mcp')
        if os.name == 'nt': env['PATH'] = 'C:/msys64/ucrt64/bin;' + env.get('PATH', '')
        code = (
            'import site,sys,time;site.addsitedir(' + repr(str(Path(mcp.__file__).parent.parent)) + ');'
            'sys.path.insert(0,' + repr(str(ROOT)) + ');'
            'from pathlib import Path;import codeforces_mcp_server.server as server\n'
            "contest={'id':9002,'name':'Stage3 fixture <not live>','type':'CF','phase':'BEFORE',"
            "'start_time_seconds':2000000000,'duration_seconds':7200}\n"
            'class Client:\n'
            ' def upcoming_contests(self):\n'
            "  if not Path('baseline').exists(): Path('baseline').touch();return []\n"
            "  Path('attempt').touch()\n"
            "  deadline=time.monotonic()+15\n"
            "  while not Path('ready').exists() and time.monotonic()<deadline: time.sleep(.02)\n"
            "  with Path('polls').open('a') as output: output.write('poll\\n')\n"
            '  return [contest]\n'
            'server.codeforces_client=Client()\n'
            'server.contest_store.sync(server.codeforces_client)\n'
            "server.mcp.settings.port=18023;server.mcp.run(transport='streamable-http')\n"
        )
        sdk = backend = events = None
        log = tempfile.TemporaryFile()
        try:
            sdk = subprocess.Popen([sys.executable, '-s', '-c', code], cwd=folder, env=env,
                                   stdout=subprocess.DEVNULL, stderr=log)
            for _ in range(100):
                if sdk.poll() is not None:
                    log.seek(0); raise RuntimeError(log.read().decode(errors='replace'))
                with socket.socket() as check:
                    if check.connect_ex(('127.0.0.1', 18023)) == 0: break
                time.sleep(.05)
            else: raise RuntimeError('SDK did not start')
            executable = ROOT / ('build/day20-tests/openai_cli.exe' if os.name == 'nt' else 'build/openai_cli')
            backend = subprocess.Popen([str(executable)], cwd=folder, env=env,
                creationflags=subprocess.CREATE_NEW_PROCESS_GROUP if os.name == 'nt' else 0,
                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            wait_backend(backend)
            events = EventConnection()
            assert events.receive()['type'] == 'snapshot'
            (folder / 'ready').touch()
            event = events.receive()
            assert event['type'] == 'codeforces_contests', event
            assert event['payload']['new_count'] == 1, event
            assert event['payload']['contests'][0]['id'] == 9002, event
            time.sleep(.4)
            assert (folder / 'polls').read_text().splitlines() == ['poll']
            with closing(sqlite3.connect(folder / 'cf.db')) as db:
                assert db.execute('SELECT COUNT(*) FROM cf_contest_snapshot').fetchone()[0] == 1
                assert len(json.loads(db.execute('SELECT new_contests_json FROM cf_sync_state WHERE id=1').fetchone()[0])) == 1
            backend.send_signal(signal.CTRL_BREAK_EVENT if os.name == 'nt' else signal.SIGTERM)
            backend.wait(timeout=10)
            assert backend.returncode == 0, backend.returncode
            print('PASS: real SDK tools/call -> Codeforces state -> watcher -> existing WebSocket; no duplicate poll; graceful shutdown')
        finally:
            if events: events.close()
            stop(backend); stop(sdk); log.close()


if __name__ == '__main__': main()
