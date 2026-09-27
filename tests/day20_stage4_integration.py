"""Isolated multi-MCP UI API regression. No external API or LLM requests."""
import argparse
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import mcp
from day18_integration import ROOT, request, wait_backend, stop


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--serve', action='store_true')
    args = parser.parse_args()
    for port in (8080, 18024, 18025):
        with socket.socket() as check:
            if check.connect_ex(('127.0.0.1', port)) == 0:
                raise RuntimeError(f'Port {port} occupied; existing processes untouched')
    with tempfile.TemporaryDirectory(prefix='day20-stage4-', dir=ROOT / 'build') as directory:
        folder = Path(directory)
        for name in ('index.html', 'app.js', 'styles.css', 'reminders.js'):
            shutil.copy2(ROOT / name, folder / name)
        shutil.copy2(ROOT / 'agent_config.example.json', folder / 'agent_config.local.json')
        (folder / 'mcp_server').mkdir()
        shutil.copy2(ROOT / 'mcp_server/reminders_schema.sql', folder / 'mcp_server/reminders_schema.sql')
        env = os.environ.copy()
        env.update(OPENAI_API_KEY='offline-only', REMINDERS_DB=str(folder / 'reminders.db'),
                   CODEFORCES_DB=str(folder / 'cf.db'), MCP_SERVER_URL='http://127.0.0.1:18024/mcp',
                   CODEFORCES_MCP_SERVER_URL='http://127.0.0.1:18025/mcp')
        if os.name == 'nt': env['PATH'] = 'C:/msys64/ucrt64/bin;' + env.get('PATH', '')
        processes, logs = [], []
        backend = None
        try:
            for module, port in (('mcp_server.server', 18024), ('codeforces_mcp_server.server', 18025)):
                code = ('import site,sys,importlib;site.addsitedir(' + repr(str(Path(mcp.__file__).parent.parent)) + ');'
                        'sys.path.insert(0,' + repr(str(ROOT)) + ');server=importlib.import_module(' + repr(module) + ')\n')
                if port == 18025:
                    code += ('class FixtureClient:\n def upcoming_contests(self): return []\n'
                             'server.codeforces_client=FixtureClient()\n')
                code += f"server.mcp.settings.port={port};server.mcp.run(transport='streamable-http')\n"
                log = tempfile.TemporaryFile(); logs.append(log)
                sdk = subprocess.Popen([sys.executable, '-s', '-c', code], cwd=folder, env=env,
                                       stdout=subprocess.DEVNULL, stderr=log)
                processes.append(sdk)
                for _ in range(100):
                    if sdk.poll() is not None:
                        log.seek(0); raise RuntimeError(log.read().decode(errors='replace'))
                    with socket.socket() as check:
                        if check.connect_ex(('127.0.0.1', port)) == 0: break
                    time.sleep(.05)
                else: raise RuntimeError('SDK did not start')
            executable = ROOT / ('build/day20-tests/openai_cli.exe' if os.name == 'nt' else 'build/openai_cli')
            backend = subprocess.Popen([str(executable)], cwd=folder, env=env,
                                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            wait_backend(backend)
            assert request('/api/mcp/connect', {})[0] == 200
            deadline = time.monotonic() + 10
            while True:
                status, state = request('/api/mcp/servers')
                if state['codeforces']['last_sync_at'] is not None: break
                if time.monotonic() > deadline: raise RuntimeError('No watcher sync')
                time.sleep(.05)
            assert status == 200 and {s['id']: s['tool_count'] for s in state['servers']} == {'reminder': 4, 'codeforces': 2}, state
            assert all(s['status'] == 'Connected' for s in state['servers']), state
            assert state['codeforces']['new_count'] == 0
            assert state['calls'][-1] == {'server_id': 'codeforces', 'tool': 'sync_contests', 'success': True}
            assert 'resultJson' not in str(state) and 'argumentsJson' not in str(state)
            for _ in range(3):
                assert request('/api/mcp/servers')[1] == state, 'Status GET changed state or invoked tools'
            assert request('/api/mcp/server/connection', {'server_id': 'invalid', 'action': 'connect'})[0] == 400
            status, disconnected = request('/api/mcp/server/connection', {'server_id': 'codeforces', 'action': 'disconnect'})
            assert status == 200 and disconnected['servers'][1]['status'] == 'Disconnected'
            assert disconnected['servers'][0]['status'] == 'Connected'
            assert request('/api/mcp/server/connection', {'server_id': 'codeforces', 'action': 'connect'})[0] == 200
            assert request('/api/reminders/overview')[1]['result']['count'] == 0
            print('PASS: two server statuses/counts, cached monitor, read-only GET, routing log, isolated connection controls, Day19 preserved', flush=True)
            if args.serve:
                (folder / 'owner.pid').write_text(str(os.getpid()))
                print(f'UI READY http://127.0.0.1:8080 (isolated fixtures: {folder}; do not use chat/OpenAI)', flush=True)
                while not (folder / 'stop').exists(): time.sleep(.2)
        finally:
            stop(backend)
            for process in processes: stop(process)
            for log in logs: log.close()


if __name__ == '__main__': main()
