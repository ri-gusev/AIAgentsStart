# C++ Agent Chat

## Day 20 — Stage 4: multi-server MCP panel

The existing MCP panel now displays **Reminder MCP** and **Codeforces MCP**, each
with connection status, URL and discovered tool count (normally 4 and 2).
Reminder's original Connect/Disconnect/Refresh tools controls, Create Reminder,
Reminder Overview and full reminder/delete list are retained. Codeforces has its
own Connect/Disconnect control. Disconnect closes the current MCP session; the
hourly watcher or an Agent request may reconnect later when it needs tools.

The compact Codeforces monitor shows the last successful sync in Moscow time and
the latest successful delta count. **Обновить статус** and the 15-second visible-tab
refresh read cached backend state via `GET /api/mcp/servers` only; they do not call
the public Codeforces API or execute `sync_contests`. After backend startup the
watcher populates this state; connecting Codeforces also reads its saved delta via
`get_new_contests`, without external REST requests.

Collapsed **Recent calls** shows the manager's bounded call history in actual
execution order, e.g. `codeforces → get_new_contests · success`, then
`reminder → create_reminder · success`. No arguments, results, raw JSON or hidden
model reasoning are rendered. Legacy manual reminder actions and deterministic
Day 19 pipeline remain separate from this Agent/router history.

`POST /api/mcp/server/connection` uses the existing manager with `server_id` and
`action` (`connect`/`disconnect`); no second transport or frontend-to-MCP connection
has been introduced. Existing Reminder API routes remain backward compatible.

Manual check: start both MCP servers and the rebuilt backend; open the UI and
hard-refresh (Ctrl+F5). Connect Reminder if needed. Confirm 4/2 tools, test the
independent Codeforces connection button, expand Recent calls, and run an ordinary
chat request using Codeforces and Reminder (requires a usable saved new-contest
delta and OpenAI key). Verify routing order and preserve reminder creation,
Overview and deletion. A zero new-contest delta is a valid state, not an error.

`python tests/day20_stage4_integration.py` checks the UI backend contract on real
isolated SDK servers with an offline API fixture, cached read-only state, tool
counts, routing log, independent disconnect/reconnect and the existing Overview.
Use the MCP Python environment, free ports 8080/18024/18025 and the Stage 3 test
executable paths. `--serve` keeps these isolated fixtures running for browser QA;
creating a `stop` file in the printed temporary fixture directory ends that mode.

## Day 20 — Stage 3: hourly Codeforces watcher

The existing web backend owns one `CodeforcesContestWatcher`, separate from the
Reminder Scheduler. It starts after the HTTP socket is bound, performs one sync
immediately, then waits one hour **after completion** before the next attempt.
Failures wait the same hour (no rapid retries). Restart starts a fresh immediate
check; this is a single-process watcher, not a distributed polling lock.

Each check uses `McpManager::syncCodeforces()` and the existing MCP transport to
call `sync_contests` on `CODEFORCES_MCP_SERVER_URL` (default localhost:8001/mcp).
Manager access to that client and call history is serialized against Agent tool
calls. The watcher does not access the Reminder client or scheduler. Idempotent
start prevents duplicate threads; stop wakes the hourly wait and joins the worker
before the event broadcaster is destroyed. An in-flight MCP request can take up
to the existing transport timeout to finish.

The Codeforces server still owns baseline/delta/database semantics: clean first
sync yields zero new contests, identical sync yields zero, and API failures preserve
the last successful snapshot and delta. Only a successful nonempty sync emits:

```json
{"type":"codeforces_contests","payload":{"new_count":1,"contests":[...]}}
```

This uses the existing `/api/reminders/events` WebSocket; reminder event types are
unchanged. The UI shows the latest compact contest notice inside MCP, with Moscow
times. Browser notifications are sent only when permission is already granted;
events never trigger a permission prompt. No replay or offline notification queue
is added: open the UI to receive events. Stage 4 server status/monitor UI is not
implemented yet. Manual `sync_contests` Agent calls remain possible separately
from the automatic hourly cadence.

Build/start (both Python MCP servers should already be running):

```powershell
cmake --preset mingw-debug
cmake --build --preset mingw-debug
$env:PATH = "C:\msys64\ucrt64\bin;" + $env:PATH
.\build\mingw-debug\openai_cli.exe
```

Ubuntu:

```bash
cmake -S . -B build
cmake --build build
./build/openai_cli
```

Open http://127.0.0.1:8080, optionally grant notifications through the existing
button, and leave the UI open. A clean Codeforces database initializes silently;
a later hourly sync emits a notice only if an upcoming contest ID has appeared
since the previous snapshot. Do not delete production state just to force alerts.

Offline checks: CTest includes `codeforces_hourly_watcher` (injected short interval
only in tests). Run `python tests/day20_stage3_integration.py` with the MCP Python
environment and free ports 8080/18023; Windows expects the verified
`build/day20-tests/openai_cli.exe`, Linux `build/openai_cli`. It launches isolated
real SDK/backend processes and a test-only API fixture, checks WebSocket delivery,
SQLite delta and graceful shutdown, without OpenAI/public API calls or production
database changes.

## Day 20 — Stage 2: multi-MCP routing and Agent tool loop

`McpManager` wraps the existing `McpClient` transport. It borrows the same Reminder client used by the UI and Day 19 pipeline and owns a separate Codeforces client. Registrations:

- `reminder`: `MCP_SERVER_URL`, default `http://127.0.0.1:8000/mcp` (backward compatible).
- `codeforces`: `CODEFORCES_MCP_SERVER_URL`, default `http://127.0.0.1:8001/mcp`.

Before a tool-aware chat request, each client connects/initializes or refreshes `tools/list`. One unavailable server does not disable tools on the other. The model-visible registry retains alias, server ID, real tool name, description and input schema. Aliases such as `reminder__create_reminder` and `codeforces__get_new_contests` avoid collisions; only the original name is sent to that server's `tools/call`.

The existing OpenAI Chat Completions transport now sends registry schemas with `tool_choice: auto` and `parallel_tool_calls: false`. The existing Agent implements the iterative loop:

```text
User -> OpenAI + schemas -> assistant tool_calls?
  no  -> existing output-policy review -> final answer
  yes -> McpManager -> selected MCP server -> tools/call
      -> role:tool + tool_call_id + result -> next OpenAI request
```

The model chooses the tool, arguments, order and stopping point; there is no fixed Codeforces -> Reminder workflow. Up to 8 tool-call attempts are permitted per chat turn. Tool errors are returned to the model as structured error JSON. Duplicate call IDs and secret-bearing arguments are rejected. Intermediate tool messages remain ephemeral: only the existing user/final-answer history is retained, with existing memory, output review, invariants and personalization. All model requests count toward existing token usage; they do not count as extra accepted user turns.

Requests that the existing heuristic considers project tasks are classified by the model against the available tool catalog before routing: a tool operation is ordinary chat; actual project work still follows the existing lifecycle and explicit buttons. PAUSED/DONE and policy guards remain in force. Planning, summaries and output-policy review cannot call tools. Successful Agent-created reminders publish through the existing update event so the normal reminder list continues updating. Scheduler and event formats are unchanged.

### Launch from the project root

Stop an old backend before rebuilding. In PowerShell:

```powershell
$env:PATH = "C:\msys64\ucrt64\bin;" + $env:PATH
cmake --preset mingw-debug
cmake --build --preset mingw-debug
# OPENAI_API_KEY must already be set in this terminal's environment.
.\build\mingw-debug\openai_cli.exe
```

Start Reminder and Codeforces MCP in separate terminals (with a working MCP environment):

```powershell
.\.venv-mcp\Scripts\python.exe mcp_server\server.py
.\.venv-codeforces\Scripts\python.exe codeforces_mcp_server\server.py
```

Fallback for this Windows checkout's broken venv launchers, using already-installed packages:

```powershell
# Terminal 1
py -3.11 -s -c "import site,runpy;site.addsitedir(r'.venv-mcp-run\Lib\site-packages');runpy.run_module('mcp_server.server',run_name='__main__')"
# Terminal 2
py -3.11 -s -c "import site,runpy;site.addsitedir(r'.venv-mcp-run\Lib\site-packages');runpy.run_module('codeforces_mcp_server.server',run_name='__main__')"
```

Ubuntu uses ordinary CMake and `.venv-mcp/bin/python` / `.venv-codeforces/bin/python` as documented below. No new dependencies or model changes are required.

### Manual chat check

Open `http://127.0.0.1:8080` and use the existing chat. Manager connects both servers automatically; the existing MCP UI still represents Reminder only until Stage 4.

1. Ask `Привет`: no tool call is needed.
2. Ask `Выполни синхронизацию соревнований Codeforces`: the model can choose `codeforces__sync_contests`. First successful sync creates a baseline with no new contests. There is no hourly watcher until Stage 3.
3. When the latest successful delta contains a contest starting more than 24 hours from now, ask:

   `Какие новые соревнования Codeforces появились при последней проверке? Для ближайшего из них создай мне напоминание за сутки до начала.`

   Expected model-selected calls: `codeforces__get_new_contests`, then `reminder__create_reminder`; check the final chat answer and normal reminder list. Empty delta means no reminder should be created. If start minus 24 hours is already in the past, the reminder tool rejects that time; the model must explain this rather than claim success.

### Verification boundaries and reproducible tests

The build and CTest suite include the real OpenAI request/response parser, existing Agent memory/policy regressions, MCP parser, Day 19 pipeline and socket/WebSocket tests. For tests, configure `BUILD_TESTING=ON`.

```powershell
ctest --test-dir build/day20-tests --output-on-failure
py -3.11 -s -c "import site,runpy;site.addsitedir(r'.venv-mcp-run\Lib\site-packages');runpy.run_path('tests/day20_stage2_integration.py',run_name='__main__')"
```

`tests/day20_stage2_integration.py` runs two real SDK servers on free ports 18020/18021 and the existing Agent test executable with scripted OpenAI replies. It seeds an explicitly labeled Codeforces test delta in a temporary database, never calls an external mock API, and makes no paid model requests. It verifies two-server routing, argument/result correlation, contest start minus 24 hours in the Reminder DB, reverse tool order, greeting, error feedback, loop limit, same-name aliases, partial availability, lifecycle classification and credential rejection. Additional same-name tools exist only in the test fixtures, not production servers. Use `--executable` to select the `agent_tests` binary from another build directory.

A scripted model test proves the loop/transport, not the quality of a live model's choices. A live OpenAI chat check requires `OPENAI_API_KEY` in the backend terminal. This Stage 2 implementation does not add the Stage 3 watcher or Stage 4 UI and does not remove the deterministic Day 19 pipeline.

## Day 19 — композиция MCP reminders

Новый `ReminderPipeline` находится в `reminder_pipeline.h/.cpp` рядом с существующим `McpClient`, вне Agent. `GET /api/reminders/overview` выполняет ровно три последовательных `tools/call`:

```text
get_upcoming_reminders({days:30}) -> result1
summarize_reminders({reminders:result1,hours:24}) -> result2
build_reminder_view({summary:result2}) -> result3 -> HTTP JSON
```

Первый tool читает будущие pending из существующей `reminders.db` за 30 дней, сортирует по времени. Второй получает весь result1 через arguments и выбирает ближайшие 24 часа. Третий получает весь result2 через arguments и возвращает presentation model с московскими `date`/`time`, без HTML. Последние два tools не обращаются к SQLite. Summary/view существуют только в памяти запроса, не сохраняются. `create_reminder`, schema, Scheduler, WebSocket, Agent и frontend не изменены.

Успех: HTTP 200, `{success:true,steps:[...],result:{title,count,message,items}}`. Ошибка: HTTP 502, `{success:false,failed_step,error}`; следующие шаги не вызываются. Перед запросом подключите MCP через существующую кнопку Connect или API. Без подключения ошибка указывает на `get_upcoming_reminders`.

После пересборки backend и перезапуска MCP-server проверьте из PowerShell:

```powershell
Invoke-RestMethod -Method Post http://127.0.0.1:8080/api/mcp/connect -ContentType application/json -Body '{}'
Invoke-RestMethod http://127.0.0.1:8080/api/reminders/overview | ConvertTo-Json -Depth 12
```

На Ubuntu:

```bash
curl -X POST -H 'Content-Type: application/json' -d '{}' http://127.0.0.1:8080/api/mcp/connect
curl http://127.0.0.1:8080/api/reminders/overview
```

Тесты (MCP Python environment, свободные порты 8080/18019; тест запускает собственные процессы и использует временную базу):

```powershell
.\.venv-mcp\Scripts\python.exe tests\test_reminder_composition.py
.\.venv-mcp\Scripts\python.exe tests\day19_integration.py
```

На Ubuntu используйте `.venv-mcp/bin/python` вместо Windows-пути. `reminder_mcp_pipeline` входит в CTest при `BUILD_TESTING=ON`. Интеграционный тест проверяет SDK discovery всех четырёх tools, три реальных MCP-вызова из одного HTTP-запроса, передачу JSON arguments, московское время, пустой результат, создание reminder и остановку при ошибке каждого этапа.

Если Windows-venv ссылается на удалённую установку Python, но зависимости есть в `.venv-mcp-run/Lib/site-packages`, запуск без изменения окружения:

```powershell
py -3.11 -s -c "import site,runpy;site.addsitedir(r'.venv-mcp-run\Lib\site-packages');runpy.run_module('mcp_server.server',run_name='__main__')"
```

## Windows и Ubuntu Linux: один web backend

HTTP routing и обработчики Agent/MCP/Reminder общие для обеих платформ. `socket_platform.h` изолирует socket type, закрытие, recv/send, ошибки, timeouts, select и nonblocking mode. Windows использует Winsock и слушает только `127.0.0.1:8080`; Linux использует POSIX sockets и слушает `0.0.0.0:8080`. MCP-server по-прежнему локальный: `http://127.0.0.1:8000/mcp` — browser не обращается к нему напрямую.

WebSocket-события Reminder поддерживаются на обеих платформах. На Linux Origin проверяется относительно HTTP Host, поэтому UI можно открыть по IP/DNS VPS. Для SHA-1/base64 handshake добавлен маленький dependency-free helper (только handshake, не криптография для авторизации); Windows сохраняет существующий системный crypto helper. POSIX send использует MSG_NOSIGNAL, timeout — timeval, nonblocking — fcntl. SIGINT/SIGTERM останавливают Scheduler и WebSocket worker перед завершением. Новых внешних библиотек, Docker/nginx/systemd/HTTPS нет.

### Ubuntu 24.04: сборка и запуск

Из корня клонированного проекта установите **существующие** зависимости сборки (curl/SQLite/threads) и Python для MCP:

```bash
sudo apt update
sudo apt install -y build-essential cmake libcurl4-openssl-dev libsqlite3-dev python3 python3-venv
cp -n agent_config.example.json agent_config.local.json
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
python3 -m venv .venv-mcp
.venv-mcp/bin/python -m pip install -r mcp_server/requirements.txt
```

Если каталог `build` был перенесён с Windows, используйте чистый Linux build directory: старый CMakeCache с Windows paths/generator не совместим. `cp -n` не перезаписывает существующий локальный конфиг.

Терминал 1, из корня проекта:

```bash
.venv-mcp/bin/python mcp_server/server.py
```

Терминал 2, также из корня проекта:

```bash
read -rsp 'OPENAI_API_KEY: ' OPENAI_API_KEY
echo
export OPENAI_API_KEY
export MCP_SERVER_URL='http://127.0.0.1:8000/mcp'
./build/openai_cli
```

Откройте `http://<IP-VPS>:8080`. MCP Connect, создание и удаление reminders работают через прежние backend endpoints. Windows-команды ниже остаются действительными. MCP порт 8000 остаётся loopback-only; открывать его наружу не нужно.

Важно: backend не имеет авторизации и теперь доступен извне на Linux. В рамках этой задачи не добавляются защита доступа, firewall rules или HTTPS; не используйте его как защищённый публичный сервис. Browser Notifications API требует secure context: на обычном `http://<IP-VPS>:8080` системный toast может быть недоступен, но разовые WebSocket-уведомления **внутри UI** работают. Это ограничение браузера, а не Scheduler.

Регрессионный интеграционный тест на Ubuntu (8080/18000 должны быть свободны):

```bash
.venv-mcp/bin/python tests/test_reminder_store.py
.venv-mcp/bin/python tests/day18_integration.py
```

Тест использует временные базы внутри `build`, placeholder API key и не вызывает OpenAI. На Linux дополнительно проверяет bind `0.0.0.0:8080`, внешний Host/Origin для WebSocket и корректное завершение через SIGTERM. Для другой build-папки: `--executable /absolute/path/to/openai_cli`. TCP/HTTP и portable handshake тесты входят в CTest.

## Day 18 — MCP Reminder и фоновый Scheduler

`create_reminder(text, run_at)` — tool создания напоминаний существующего Python MCP-server. Tool валидирует текст и будущую дату, записывает `pending` в отдельный `reminders.db` и сразу возвращает `id`, `text`, `run_at`, `status`. Тестовые `add`, `echo`, `get_todo` удалены; LLM не выбирает инструменты.

```text
Reminder UI -> POST /api/reminders -> existing McpClient.tools/call
  -> Python create_reminder -> reminders.db
Background ReminderScheduler -> reminders.db -> WebSocket -> UI + Browser Notification
```

Scheduler работает в отдельном потоке C++ backend с интервалом 1 секунда. Транзакция атомарно забирает наступившие pending reminders и удаляет их из базы; только получивший запись Scheduler отправляет разовое notification event. Конкурентное удаление/срабатывание не создаёт повторных событий. При старте обрабатываются сохранённые pending, включая просроченные. MCP-server нужен для создания, но не для срабатывания уже сохранённых reminders. Пока backend выключен, проверки не выполняются.

Хранилище отделено от memory/task state/invariants/policies/personalization. В SQLite и списке Reminders остаются только запланированные (`pending`) записи. Кнопка «Удалить» отменяет reminder и удаляет его из базы через `POST /api/reminders/delete`; отменённый reminder больше не срабатывает. Завершённые reminders и история уведомлений не сохраняются. При первом запуске новой версии ранее сохранённые triggered и таблица notification history удаляются без возможности восстановления, pending сохраняются.

По умолчанию оба процесса используют `reminders.db` в корне проекта; запускайте backend из корня. Для другого файла задайте одинаковый абсолютный `$env:REMINDERS_DB` в обоих терминалах. Файлы DB/WAL/SHM исключены из Git.

Форма, кнопки `+1/+2 минуты`, список reminders и время уведомлений используют московское время (МСК, UTC+03:00), независимо от часового пояса компьютера. MCP также принимает `2026-09-26 18:00:00` как московское время; явный offset в ISO 8601 учитывается как переданный момент времени. Хранение — Unix UTC, API — ISO UTC: например, 18:00 МСК соответствует 15:00Z. Сохранённые reminders не пересчитываются.

### Запуск и проверка

Один раз установите Python-зависимости (см. «Запуск MCP-server» ниже). Терминал 1:

```powershell
Set-Location D:\AIAgentsCourse
.\.venv-mcp\Scripts\python.exe mcp_server\server.py
```

Терминал 2 (сборка нужна после изменения C++, не перед каждым запуском):

```powershell
Set-Location D:\AIAgentsCourse
cmake --preset mingw-debug
cmake --build --preset mingw-debug
.\load-env.ps1
.\build\mingw-debug\openai_cli.exe
```

Откройте `http://127.0.0.1:8080`, нажмите MCP `Connect`. В Reminder введите текст, нажмите `+1 минута` или `+2 минуты`, затем `Create Reminder`. Разрешите уведомления в браузере. Reminder появится как pending, при наступлении времени исчезнет из списка и базы; в Notifications на 15 секунд появится разовое сообщение, а браузер покажет системное уведомление при выданном разрешении. Можно продолжать пользоваться чатом. Для теста используйте именно +1/+2 минуты, не пример с завтрашней датой.

Разрешение запрашивается только при явном действии пользователя, один раз (маркер в localStorage). Если оно отклонено, включите его вручную в настройках сайта; UI-уведомления всё равно работают. WebSocket `/api/reminders/events` автоматически переподключается и восстанавливает список pending. Истории и повторной доставки завершённых reminders нет: разовое событие получает только подключённый интерфейс. Browser Notifications требуют открытого интерфейса и разрешения браузера/ОС; Service Worker/Web Push не добавлены.

Проверка сохранения: создайте pending через `+2 min`, остановите только C++ backend через Ctrl+C, снова запустите executable — reminder останется и сработает автоматически. Не удаляйте `reminders.db` между запусками.

Тесты (без OpenAI-запросов и без изменения рабочих баз):

```powershell
cmake --preset mingw-debug -DBUILD_TESTING=ON
cmake --build --preset mingw-debug
ctest --test-dir build/mingw-debug --output-on-failure
.\.venv-mcp\Scripts\python.exe tests\test_reminder_store.py
node tests\reminder_timezone_tests.cjs # Optional JS timezone test; requires Node.js
.\.venv-mcp\Scripts\python.exe tests\day18_integration.py
```

Последний тест сам запускает отдельный MCP-server и backend с изолированными базами в `build`; порты 8080/18000 должны быть свободны. Проверяет реальный `tools/call`, WebSocket, ошибки даты/ID, удаление и отсутствие уведомлений для отменённой записи, сохранение pending при перезапуске, отсутствие истории/дублей и срабатывание даже после остановки MCP-server. Системное уведомление проверяется вручную в браузере.

## Day 17 — MCP: ручной запуск tools

MCP добавлен отдельным слоем и не связан с memory, task state machine, invariants, policies или personalization:

```text
Web UI -> web_server.cpp -> McpClient / libcurl -> Local Python MCP Server
```

Локальный сервер `mcp_server/server.py` использует официальный Python MCP SDK и публикует `create_reminder(text, run_at)` и три tools композиции Day 19 (см. выше). Внешние mock REST API не используются. Сервер работает отдельным процессом на `http://127.0.0.1:8000/mcp` через Streamable HTTP. C++-клиент выполняет `initialize`, отправляет `notifications/initialized`, вызывает `tools/list` и сохраняет `name`, `description`, `inputSchema`.

Web UI обращается только к C++ backend:

- `POST /api/mcp/connect` — MCP handshake и первоначальный `tools/list`;
- `POST /api/mcp/disconnect` — завершение MCP-сессии и очистка локального списка;
- `GET /api/mcp/tools` — обновление списка при активном соединении или чтение текущего disconnected/error state.
- `POST /api/mcp/call` — ручной вызов выбранного инструмента; тело содержит `name` и JSON arguments.

В правой колонке существующего интерфейса блок `MCP` показывает статус, имя и адрес сервера, кнопки подключения и раскрываемый список tools. Раскрытие конкретного tool показывает форму по его `inputSchema`; кнопка `Run tool` отправляет вызов через backend. MCP-server никогда не вызывается напрямую из JavaScript.

## Day 14 — Project Invariants

`InvariantStore` persists `project_invariants` in its own SQLite file (`project_invariants.db` by default), keyed by the existing project/chat ID. Each row has `key`, `value`, and `description`; it is isolated from short-term history, working memory, long-term memory, personalization, and task state.

The agent loads the active project's invariants into the context immediately after base and input policies, before long-term/working memory and dialogue. They constrain only Agent output, never the user's input: the user may write any request. Generated plans, execution results, ordinary answers, and validation reports receive the invariant context; plans, execution results, and ordinary answers additionally pass the output-policy review, which must revise any invariant violation. Invariants are never created or changed by dialogue; only the explicit UI/API CRUD actions can modify them.

Configuration accepts an optional `project_invariants_db` path. The UI panel provides view, add, edit, and delete actions for the selected project through one restriction text field; internal key and description values are generated by the application.

## One-button launch in VS Code

Open this folder in VS Code and press **F5** with the `OpenAI Web Chat (CMake)` configuration selected. The included CMake preset configures the UCRT64 MinGW compiler, curl, and SQLite in `build/mingw-debug`, then builds and starts the local server. `.env` is supplied to the debugger only as environment variables; the C++ code still reads the key exclusively from `OPENAI_API_KEY`.

Локальный веб-чат с Agent на C++17. Браузер отвечает только за интерфейс; контекст, policies, память, OpenAI API и расчёт стоимости обрабатываются в C++.

Можно создавать чаты с собственными названиями и переключаться между ними. У каждого чата свои краткосрочная и рабочая память; долговременная память и персонализация общие. Ветки и checkpoints не используются.

## Возможности

- именованные чаты с независимой историей, summary и данными текущей задачи; ненужный чат можно удалить вместе с его рабочей памятью;
- последние 5 отдельных сообщений активного чата передаются модели дословно;
- более старая переписка сжимается в накопительный summary каждые 5 успешно завершённых запросов пользователя в этом чате;
- рабочая память хранит стек, требования, этапы и ограничения конкретной задачи в SQLite отдельно для каждого чата;
- строгая task state machine ведёт проект по этапам `PLANNING → EXECUTION → VALIDATION → DONE`, поддерживает `PAUSED`, а каждый переход атомарно сохраняет в SQLite вместе с записью журнала;
- Pause сжимает состояние проекта в отдельный persistent summary, Resume продолжает с прежнего этапа;
- долговременные факты, профиль, цели и знания хранятся в существующей SQLite-таблице и доступны всем чатам;
- после каждого принятого сообщения Agent автоматически решает, что оставить только в short-term, что сохранить в рабочую память проекта и что добавить в общую long-term memory;
- общая панель персонализации справа задаёт роль, язык и стиль ответа;
- C++-проверки input policy выполняются до обращения к модели;
- output policy проверяет и при необходимости исправляет ответ отдельным LLM-вызовом;
- отображаются токены и оценка стоимости, включая отдельную статистику summarization;
- сообщения прокручиваются внутри чата, строка ввода остаётся внизу;
- HTTP-сервер слушает `127.0.0.1:8080` на Windows и `0.0.0.0:8080` на Linux.

## Архитектура

```mermaid
flowchart LR
    Browser[HTML / CSS / JavaScript] -->|Local HTTP| Server[web_server.cpp]
    Server --> Agent[Agent]
    Server --> MCP[McpClient / libcurl]
    MCP --> LocalMCP[Local Python MCP Server]
    Agent --> RAM[Per-chat raw messages + summary + transcript in RAM]
    Agent --> DB[(SQLite: working memory + project state/summary + shared long-term facts)]
    Agent --> Client[ApiClient / libcurl]
    Client --> OpenAI[OpenAI API]
```

`main.cpp` создаёт `Agent`, проверяет его готовность и запускает веб-сервер. Он не собирает API-запросы и не управляет моделью, policies или памятью.

| Файл | Назначение |
| --- | --- |
| `main.cpp` | Точка запуска Agent и веб-сервера |
| `agent.h`, `agent.cpp` | Конфигурация, контекст, input/output policies, память и статистика |
| `api_client.h`, `api_client.cpp` | OpenAI HTTP API через libcurl и разбор ответа |
| `mcp_client.h`, `mcp_client.cpp` | MCP handshake, `tools/list` и ручной `tools/call` через libcurl |
| `mcp_server/server.py` | Отдельный локальный MCP-server на официальном Python SDK |
| `mcp_server/reminder_store.py`, `mcp_server/reminders_schema.sql` | Валидация/регистрация reminder и общая SQLite-схема |
| `reminder_store.h`, `reminder_store.cpp` | Изолированное pending-only хранилище, удаление и атомарное срабатывание |
| `reminder_scheduler.h`, `reminder_scheduler.cpp` | Фоновая проверка pending reminders |
| `reminder_events.h`, `reminder_events.cpp` | Независимый WebSocket-поток backend → frontend |
| `reminders.js` | Reminder-форма, realtime UI и Browser Notifications |
| `memory_store.h`, `memory_store.cpp` | SQLite: общие долговременные факты, рабочая память чатов, названия и настройки |
| `web_server.h`, `web_server.cpp` | Локальные HTTP-маршруты и выдача статических файлов |
| `socket_platform.h`, `websocket_handshake.h` | Winsock/POSIX wrappers и portable WebSocket handshake без новых библиотек |
| `index.html`, `styles.css`, `app.js` | Веб-интерфейс |
| `agent_config.local.json` | Локальная конфигурация, исключённая из Git |
| `agent_config.example.json` | Безопасный шаблон конфигурации для клонирования репозитория |

## Как обрабатывается сообщение

1. Браузер отправляет текст и ID выбранного чата на локальный `POST /api/chat`. К OpenAI браузер не обращается.
2. Agent проверяет входной текст в C++: обнаруженные секреты и опасные запросы на выполнение команд отклоняются; подозрительные попытки подменить инструкции помечаются.
3. До вызова модели Agent детерминированно различает простой информационный вопрос и задачу с требуемым результатом. Вопрос получает обычный ответ и не запускает lifecycle. Задача в пустом `planning` создаёт только структурированный план; пока он не утверждён, текстовые сообщения могут лишь уточнить или переработать этот план.
4. Для обычного ответа Agent собирает список сообщений API из базовой инструкции с персонализацией, input policy, общих долговременных facts, рабочей памяти выбранного чата, task state, project summary, conversation summary, последних 5 raw-сообщений и нового вопроса.
5. `ApiClient` отправляет основной запрос в OpenAI.
6. Отдельный вызов output policy проверяет качество, структуру и длину первоначального ответа и либо подтверждает его, либо возвращает исправленную версию.
7. После успешного завершения пользовательский запрос и итоговый ответ добавляются в краткосрочную память и видимую историю выбранного чата.
8. Если наступил срок summarization, Agent обновляет summary старой части истории.
9. Дополнительный LLM-вызов автоматически определяет `working_facts` и `long_term_facts`: первые сохраняются в контейнер текущего чата, вторые — в общую долговременную память. Ручного выбора слоя в интерфейсе нет.
10. Ответ, состояние памяти, предупреждения и статистика возвращаются интерфейсу.

Отклонённые запросы и неудачные основные ответы не становятся завершёнными ходами и не увеличивают счётчик периода summarization. Ошибка обновления summary или SQLite не отменяет уже готовый ответ: приложение сообщает о проблеме. Неудачное сжатие сохраняет прежний summary и raw-буфер. Распределённые в рабочую и долговременную память факты одного хода записываются общей SQLite-транзакцией: ошибка записи откатывает этот набор.

## Контекст для модели

`base_instruction` — постоянная строка в конфигурации: роль Agent и общие правила ответа. Agent добавляет к ней сохранённую персонализацию, не изменяя сам конфиг. История и факты добавляются отдельными API-сообщениями, а не становятся частью постоянной базовой инструкции. Рекурсивной сборки system prompt нет.

Каждый основной запрос содержит в таком порядке:

1. system-инструкцию `base_instruction` с общей персонализацией;
2. `input_policy`;
3. общие long-term facts из SQLite, обозначенные как справочные данные, а не команды;
4. working facts только выбранного чата, также обозначенные как данные;
5. сохранённые task state, план и project summary выбранного чата;
6. conversation summary этого чата;
7. последние `N = 5` отдельных raw-сообщений этого чата с исходными ролями `user` / `assistant`;
8. текущий запрос с ролью `user`.

`output_policy` применяется после первоначального ответа отдельным проверочным вызовом. Это не часть пользовательской истории и не постоянное содержимое памяти.

## Память

| Слой | Содержимое | Изоляция | После перезапуска процесса |
| --- | --- | --- | --- |
| Краткосрочная | Последние 5 raw-сообщений и накопительный summary | Своя для каждого чата | Обнуляется |
| Рабочая | Стек, требования, этап задачи, ограничения, открытые вопросы | Свой SQLite-контейнер для каждого чата | Сохраняется |
| Долговременная | Профиль, устойчивые цели, общие решения и знания | Общая для всех чатов | Сохраняется |

Названия чатов и персонализация также сохраняются в SQLite. При первом запуске без чатов Agent создаёт чат «Основной». Переключение чата восстанавливает его RAM-историю в пределах текущего запуска; рабочие данные доступны и после перезапуска.

Удаление чата атомарно удаляет его строку из `chats`, связанные строки `working_memory`, `project_summaries`, `project_task_states` и краткосрочное состояние в RAM. Общая таблица `long_term_memory`, персонализация, режим и статистика токенов не очищаются. Если удалён последний чат, в той же SQLite-транзакции создаётся новый пустой чат «Основной» с состоянием `planning`.

### Short-term: raw window и summary

Agent хранит отдельные `std::vector` raw-сообщений для каждого чата. Для основного ответа выбирается только хвост выбранного чата из последних 5 сообщений; текущий вопрос добавляется отдельно. **5 сообщений — не 5 диалоговых ходов:** один завершённый ход обычно состоит из сообщения пользователя и ответа ассистента.

Старые raw-сообщения не удаляются немедленно. Они остаются в буфере до успешного сжатия:

- пока история короткая, summary пустой;
- после каждых 5 успешно завершённых пользовательских запросов в конкретном чате отдельный LLM-вызов получает его предыдущий summary и новый старый блок;
- последние 5 raw-сообщений исключаются из сжатия;
- summary выделяет основные темы, решения, ограничения и незавершённые вопросы;
- новый summary заменяет предыдущий только при успешном полном ответе модели (`finish_reason = "stop"`);
- из raw-буфера удаляется только префикс, вошедший в новый summary;
- при ошибке или обрезанном ответе summary и raw-буфер не меняются; попытка повторяется после следующего успешно завершённого запроса.

Например, после первых 5 завершённых запросов обычно имеется 10 raw-сообщений: первые 5 попадут в summary, последние 5 останутся дословно. Следующее сжатие объединит предыдущий summary с очередным старым блоком.

В промежутке между сжатиями старый блок может уже выйти из последних 5 сообщений, но ещё не попасть в summary. Он остаётся в RAM, однако в основной запрос передаётся только актуальный summary и последний raw-хвост. При длительной ошибке сжатия этот буфер может расти.

Summary, raw-буфер и счётчик запросов отдельны для каждого чата и существуют только в RAM. Перезапуск сервера очищает их у всех чатов, но не удаляет чаты и постоянную SQLite-память.

### Working: данные задачи выбранного чата

Рабочая память — постоянный контейнер текущей задачи, а не копия общей истории. Например, «этот проект использует C++17, libcurl и SQLite» относится к текущему чату; сведения другого чата не подгружаются в его контекст.

После каждого принятого сообщения модель выделяет краткие facts о стеке, требованиях, ограничениях, текущем этапе и открытых вопросах. Информация хранится в отдельной таблице `working_memory` с ключом `(chat_id, memory_key)`.

### Task state machine и Pause

Каждый чат-проект имеет отдельную запись в `project_task_states` и `project_summaries`:

```text
PLANNING --APPROVE_PLAN--> EXECUTION --EXECUTION_FINISHED (button)--> VALIDATION
                              ^                                |      |
                              |------ VALIDATION_FAILED (button)|      |
                                                               | VALIDATION_PASSED
                                                               v
                                                              DONE

PLANNING/EXECUTION --PAUSE--> PAUSED --RESUME--> сохранённый этап
DONE --CREATE_TASK--> PLANNING
```

- `PLANNING`: Agent формирует и сохраняет структурированный план, после чего останавливается. Только кнопка `Approve Plan` отправляет action `APPROVE_PLAN`; текст «апрувни план», «продолжай» или «переходи дальше» не меняет состояние.
- `EXECUTION`: Agent выполняет утверждённый план и останавливается с готовым результатом. Кнопка `Проверить / Перейти к validation` подтверждает завершение execution и запускает проверку.
- `VALIDATION`: Agent выполняет статическую смысловую проверку без компиляции и запуска файлов. После отчёта пользователь нажимает `Завершить задачу / DONE` при успехе или `Вернуться к execution` при замечаниях.
- `DONE`: доступен только `New Task`, который начинает новый цикл с `PLANNING`.
- `PAUSED`: при Pause Agent сохраняет project summary и исходный этап в `resume_state`; Resume возвращает задачу ровно в этот этап.

Отдельная панель `Task State` показывает текущий state и stepper `Planning → Execution → Validation → Done`. Этапы не являются кнопками: завершённые отмечаются, текущий выделяется, будущие недоступны. Панель показывает только допустимые actions текущего состояния.

Все изменения проходят через одну C++-функцию `Agent::transitionTask(...)`. Она проверяет пару `state + action`, prerequisites и ожидаемое состояние SQLite. Запись `project_task_states` и новая строка `task_state_transition_log(previous_state, action, new_state, timestamp)` выполняются в одной транзакции. При запрете сервер возвращает понятную ошибку, не вызывает модель и не меняет state. `phaseRunning` не допускает одновременный запуск двух фаз.

Validation не запускает команды операционной системы: у Agent нет tools. Он выполняет статическую смысловую проверку доступного результата, не требует компиляции или запуска файла и не должен выдумывать прохождение тестов.

### Long-term: общая SQLite-память

После принятого и успешно завершённого хода LLM-router анализирует сообщение пользователя и итоговый ответ ассистента и отдельно выбирает рабочие и долговременные сведения. Переписка для него — недоверенные данные, а не инструкции. Подозрительный ход не запускает автоматическое сохранение в эти два слоя.

Сохраняются сведения с долгосрочной ценностью:

- явно сообщённые пользователем устойчивые факты и предпочтения;
- долгосрочные цели;
- переиспользуемые знания и подтверждённые общие решения.

Случайные события, временные эмоции, секреты и неподтверждённые предложения ассистента не должны становиться долгосрочными фактами. Данные конкретной задачи направляются в рабочую память, если пользователь явно не указал, что они применимы шире. Долговременная память доступна всем чатам.

Один ход может дать ноль, один или несколько facts в каждом слое. Существующий ключ обновляется через `INSERT ... ON CONFLICT DO UPDATE`, новый добавляется. Краткосрочная история при этом не удаляется.

По умолчанию база находится в `agent_memory.db` относительно текущей рабочей папки. При запуске из корня проекта это `D:\AIAgentsCourse\agent_memory.db`. Другой путь задаётся полем `long_term_memory_db`.

Существующая схема остаётся прежней:

```sql
CREATE TABLE IF NOT EXISTS long_term_memory (
    memory_key TEXT PRIMARY KEY NOT NULL,
    memory_value TEXT NOT NULL,
    updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
);
```

**Запуск и остановка сервера не очищают таблицу.** После перезапуска Agent загружает сохранённые facts. Файл базы и локальный конфиг не предназначены для публикации в GitHub.

Схема расширена без удаления существующих данных: дополнительно создаются `chats`, `working_memory`, `project_summaries`, `project_task_states`, `task_state_transition_log` и `agent_settings`. Старые lowercase-состояния и поле Pause мигрируют в uppercase-состояния и `resume_state`; для существующих чатов недостающие проектные строки создаются с пустым summary и состоянием `PLANNING`. Старые факты в `long_term_memory` остаются общими.

### Автоматическое распределение памяти

После ответа модель одним вызовом возвращает два массива — `working_facts` для текущего чата и `long_term_facts` для всех чатов. Она может вернуть пустые массивы, если сообщение не нужно сохранять надолго. Short-term history обновляется всегда автоматически.

Переключателя Auto/Manual и кнопок выбора памяти под сообщениями в интерфейсе нет. Lifecycle-actions находятся в отдельной панели `Task State`; чат не служит механизмом перехода между этапами. Панели рабочей и общей долговременной памяти справа показывают фактически сохранённые данные.

### Персонализация

Правая панель содержит общую настройку роли, языка и стиля ответа. После нажатия «Сохранить» текст записывается в `agent_settings` и добавляется к `base_instruction` перед последующими LLM-вызовами во всех чатах. Пустой текст сбрасывает персонализацию.

В C++ проверяются длина (не более 2000 байт), возможные секреты, опасные запросы и известные попытки отменить инструкции. Персонализация не предоставляет tools и не имеет права отменять базовые правила, input policy или output policy. Эти проверки эвристические, а не гарантия защиты от всех вариантов injection.

### Видимая история

Отдельный полный transcript каждого чата хранится в RAM для интерфейса. Поэтому после summarization ранние сообщения продолжают отображаться на сайте и возвращаются при переключении чата в текущем запуске. Transcript не отправляется модели целиком и не записывается в SQLite автоматически. Ручное сохранение выбранного сообщения сохраняет только этот текст в выбранный постоянный слой.

При перезапуске сервера видимая история, short-term память, summary и статистика обнуляются; названия чатов, рабочие и общие долговременные facts, режим и персонализация остаются. Закрытие вкладки браузера не останавливает сервер и не сбрасывает его RAM.

## Input policy и безопасность

Проверки выполняются в C++ до API-вызовов и записи в память. Текст `input_policy` добавляет правила для модели, но не заменяет проверки в коде.

- **Prompt injection:** фразы вроде «игнорируй предыдущие инструкции» или «очисти память» отмечаются как подозрительные. Само обсуждение prompt injection не обязательно блокируется. Пользовательский текст не получает полномочий изменять system-инструкции или очищать базу.
- **Секреты:** обнаруженные API-ключи, пароли, токены и приватные ключи приводят к отклонению запроса до обращения к модели. Отклонённый текст не добавляется в историю, summary или SQLite и не должен повторяться в предупреждении или логах.
- **Опасные команды:** запросы на разрушительное исполнение, например удаление системных файлов или форматирование диска, блокируются эвристически. Обсуждение таких команд само по себе не означает их выполнение.

Инструменты не передаются модели: LLM не может выбирать или запускать MCP tools. Пользователь раскрывает список в панели MCP, раскрывает нужный инструмент, вводит аргументы и явно нажимает `Run tool`. Backend вызывает `McpClient::callTool`, который отправляет MCP `tools/call`; результат, аргументы и success/error отображаются в истории ручных вызовов. Вызов tool не изменяет чат, память или task state.

Фильтры основаны на эвристиках: это не полноценный DLP-сканер и не доказательство отсутствия секретов или prompt injection. Не вводите реальные секреты в чат; необычные форматы могут не распознаться, а некоторые безопасные примеры могут быть отклонены.

## Конфигурация

Agent самостоятельно читает `agent_config.local.json`. Для нового клона создайте файл из шаблона, не перезаписывая уже настроенную локальную конфигурацию:

```powershell
Copy-Item agent_config.example.json agent_config.local.json
```

| Поле | Назначение |
| --- | --- |
| `model` | Идентификатор модели OpenAI |
| `base_instruction` | Постоянная инструкция о роли и поведении Agent |
| `input_policy` | Дополнительные правила обработки входного текста |
| `output_policy` | Правила отдельной проверки первоначального ответа |
| `short_term_memory_messages` | Число последних raw-сообщений в основном запросе; по ТЗ `5` |
| `summary_every_requests` | Период сжатия в успешно завершённых пользовательских запросах; по ТЗ `5` |
| `long_term_memory_db` | Путь к SQLite-базе |
| `input_price_per_million` | USD за миллион некэшированных входных токенов |
| `cached_input_price_per_million` | USD за миллион кэшированных входных токенов |
| `output_price_per_million` | USD за миллион выходных токенов |

API-ключ не должен находиться в JSON. Agent читает его только из `OPENAI_API_KEY`. Если ключ хранится в локальном `.env`, загрузите переменную существующим скриптом:

```powershell
.\load-env.ps1
```

Не добавляйте `.env`, `agent_config.local.json` или базу с личными фактами в Git. Указание `base_instruction` не позволяет обойти C++-проверки и не предоставляет модели tools.

## Токены и стоимость

Статистика общая для всех чатов и накапливается для всех полученных API usage текущего запуска: основной ответ, output policy, автоматическое распределение памяти и summarization. Создание, переключение и удаление чатов и сохранение персонализации не вызывают OpenAI и не увеличивают счётчики токенов.

Обычный успешный запрос требует 3 LLM-вызова: основной ответ, output policy и автоматический memory router. Когда обновляется summary, добавляется ещё один вызов. Подозрительный ввод пропускает автоматический router, а ошибки и повторы могут менять фактическое число вызовов.

- **Input context** — входные токены всех учитываемых вызовов и оценка их стоимости; это не только последний пользовательский вопрос.
- **Summary usage** — входные и выходные токены отдельного summarization-вызова и его стоимость.
- **All usage** — общие токены и стоимость всех вызовов Agent.

Summary usage уже входит в All usage и не прибавляется к нему второй раз. Передача готового summary в основном контексте оплачивается как часть input tokens соответствующего запроса.

```text
uncached_input = input_tokens - cached_input_tokens
input USD = (uncached_input * input_rate + cached_input * cached_rate) / 1 000 000
output USD = output_tokens * output_rate / 1 000 000
total USD = input USD + output USD
```

В шаблоне используются существующие настройки проекта: `$0.20` для input, `$0.02` для cached input и `$1.20` для output за миллион токенов. Это настраиваемые значения, а не подтверждение актуального тарифа модели: проверьте свои ставки и обновите конфиг при необходимости.

Оценка приложения не заменяет счёт OpenAI. При сетевом таймауте API мог уже обработать запрос, но приложение не получить его usage; повторные попытки тоже могут расходовать токены. Счётчики приложения сбрасываются при перезапуске, фактические расходы аккаунта — нет.

## Требования

- Windows и компилятор C++17;
- libcurl;
- SQLite3;
- WinSock2;
- CMake 3.15+ — только для сборки через CMake.

Для окружения MSYS2 UCRT64 зависимости можно установить командой:

```bash
pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-curl mingw-w64-ucrt-x86_64-sqlite3 mingw-w64-ucrt-x86_64-cmake
```

Каталог выбранного toolchain должен быть доступен в `PATH`; используйте совместимые компилятор, headers и библиотеки одного окружения.

## Сборка и запуск

Все команды выполняются из корня проекта. Перед заменой работающего `main.exe` остановите сервер через `Ctrl+C` — Windows блокирует исполняемый файл запущенного процесса.

### Через g++

```powershell
g++ -std=c++17 main.cpp agent.cpp api_client.cpp web_server.cpp memory_store.cpp invariant_store.cpp mcp_client.cpp reminder_store.cpp reminder_scheduler.cpp reminder_events.cpp -o main.exe -lcurl -lws2_32 -lsqlite3 -lbcrypt -lcrypt32 -pthread
.\load-env.ps1
.\main.exe
```

### Запуск MCP-server

Один раз создайте отдельное Python 3.11 окружение и установите официальный SDK:

```powershell
py -3.11 -m venv .venv-mcp
.\.venv-mcp\Scripts\python.exe -m pip install -r mcp_server\requirements.txt
```

Запустите MCP-server в первом терминале:

```powershell
.\.venv-mcp\Scripts\python.exe mcp_server\server.py
```

Во втором терминале соберите и запустите C++ backend приведёнными выше командами, затем откройте `http://127.0.0.1:8080` и нажмите `Connect` в блоке MCP. Раскройте `Available tools`, выберите `create_reminder`, укажите `text` и будущий `run_at` и явно нажмите `Run tool`. Ответ и статус вызова появятся в `Recent calls`. По умолчанию C++ подключается к `http://127.0.0.1:8000/mcp`; другой адрес можно задать переменной среды `MCP_SERVER_URL` до запуска backend.

### Через CMake

```powershell
cmake --preset mingw-debug
cmake --build --preset mingw-debug
.\load-env.ps1
.\build\mingw-debug\openai_cli.exe
```

У multi-config генератора executable может находиться, например, в `build\Debug\openai_cli.exe`. Рабочей папкой при запуске всё равно должен быть корень проекта, где находятся конфиг и статические файлы.

Откройте `http://127.0.0.1:8080`. После изменений HTML/CSS/JavaScript обновите страницу через `Ctrl+F5`; после изменений C++ или конфигурации перезапустите сервер, а для C++ сначала пересоберите executable.

## Автоматические проверки

Тесты памяти и policies используют подменённый API-клиент и отдельную временную SQLite-базу. Тесты HTTP-парсера не запускают сервер. Реальный ключ не нужен, обращения к OpenAI и расходы отсутствуют; рабочая база не изменяется.

```powershell
g++ -std=c++17 -Wall -Wextra -pedantic -I. tests\agent_tests.cpp agent.cpp memory_store.cpp invariant_store.cpp -o build\agent-tests.exe -lsqlite3
.\build\agent-tests.exe
g++ -std=c++17 -Wall -Wextra -pedantic -I. tests\web_parser_tests.cpp agent.cpp api_client.cpp memory_store.cpp invariant_store.cpp mcp_client.cpp reminder_store.cpp reminder_scheduler.cpp reminder_events.cpp -o build\web-parser-tests.exe -lcurl -lws2_32 -lsqlite3 -lbcrypt -lcrypt32 -pthread
.\build\web-parser-tests.exe
g++ -std=c++17 -Wall -Wextra -pedantic -I. tests\mcp_client_tests.cpp -o build\mcp-client-tests.exe -lcurl -lws2_32
.\build\mcp-client-tests.exe
```

Для нового клона сначала создайте каталог `build` через `New-Item -ItemType Directory -Path build -Force`. При CMake тесты включены по умолчанию: после сборки выполните `ctest --test-dir build --output-on-failure` (для multi-config добавьте `-C Debug`).

## Локальный HTTP API

Все POST-запросы используют `Content-Type: application/json`. Успех возвращает состояние Agent в JSON; ошибка содержит `error` и, для API-маршрутов, состояние. Невалидный ввод возвращает HTTP 400; ошибка генерации ответа — HTTP 500. Веб-сервер не содержит авторизации: Windows bind локальный, Linux bind доступен на всех IPv4 interfaces.

| Метод и маршрут | JSON-тело | Действие |
| --- | --- | --- |
| `GET /api/state` | — | Состояние выбранного чата и общие настройки/статистика |
| `POST /api/mcp/connect` | — | MCP handshake и первоначальный `tools/list` |
| `POST /api/mcp/disconnect` | — | Закрыть MCP-сессию и очистить cached tools |
| `GET /api/mcp/tools` | — | Обновить или вернуть текущее состояние MCP tools |
| `POST /api/mcp/call` | `{"name":"create_reminder","arguments":"{\"text\":\"Проверка\",\"run_at\":\"2099-01-01T12:00:00+03:00\"}"}` | Вызвать выбранный tool вручную и вернуть историю результатов |
| `POST /api/reminders` | `{"text":"Тренировка","run_at":"2026-09-26T18:00:00+03:00"}` | Создать reminder через существующий MCP `tools/call` |
| `GET /api/reminders` | — | Прочитать только pending reminders, без истории notifications |
| `POST /api/reminders/delete` | `{"id":"1"}` | Отменить и удалить запланированный reminder |
| `GET /api/reminders/events` | WebSocket upgrade | Snapshot и realtime updates для открытого UI |
| `POST /api/chats` | `{"name":"Проект C++"}` | Создать именованный чат и выбрать его |
| `POST /api/chats/select` | `{"chat_id":"1"}` | Выбрать существующий чат |
| `POST /api/chats/delete` | `{"chat_id":"1"}` | Удалить чат, его рабочую память и RAM-историю; общая long-term memory сохраняется |
| `POST /api/chat` | `{"chat_id":"1","message":"Создай приложение на C++17 и SQLite"}` | Изменить содержимое задачи/чата без ручного transition; первая задача формирует `PLANNING` |
| `POST /api/personalization` | `{"text":"Отвечай по-русски, кратко, с примерами C++."}` | Сохранить общую персонализацию; пустой `text` очищает её |
| `POST /api/task/transition` | `{"chat_id":"1","action":"APPROVE_PLAN"}` | Выполнить разрешённый lifecycle-action с серверной проверкой |
| `POST /api/task/plan` | `{"chat_id":"1","task_request":"Описание задачи"}` | Совместимый маршрут: отправить первое planning-сообщение в чат |
| `POST /api/task/revise` | `{"chat_id":"1","feedback":"Что изменить"}` | Переделать текущий план |

Публичные actions кнопок: `APPROVE_PLAN`, `REGENERATE_PLAN`, `EXECUTION_FINISHED`, `VALIDATION_PASSED`, `VALIDATION_FAILED`, `PAUSE`, `RESUME`, `CREATE_TASK`. Все они проходят один серверный валидатор; клиент не может выбрать произвольное состояние. Старые `/api/task/approve`, `/api/task/pause`, `/api/task/resume` оставлены как совместимые обёртки; ручные `/validation`, `/validate`, `/execution` не обходят конечную машину.

`chat_id` — строковый ID, полученный из `chats`. Ручных маршрутов сохранения памяти нет: после каждого успешного хода Agent сам обновляет short-term и вызывает router для working/long-term facts.

Для совместимости `POST /api/chat` без `chat_id` работает с текущим выбранным чатом. При явном ID он должен существовать; пустой или неверный ID не заменяется другим чатом. Интерфейс всегда передаёт ID.

### Состояние

```http
GET /api/state
```

Основные поля ответа:

- `chats`: список `{id, name}`, `active_chat_id`, `active_chat_name`;
- `personalization`: общая настройка роли, языка и стиля;
- `task_state`: uppercase-этап, `resume_state`, план, validation report и признак завершённого execution выбранного проекта;
- `project_summary`: отдельный persistent summary, обновляемый только через Pause;
- `conversation`: история выбранного чата; у сообщений есть `id`, `role`, `content`, статусы `short_term`, `working`, `long_term`;
- `working_memory`: facts только выбранного чата в формате `{key, value}`;
- `long_term_memory`: общие facts в том же формате;
- `memory`: размер raw-окна, ожидающие сжатия сообщения, наличие summary, счётчик успешных запросов и число facts;
- `usage`: общие `input_tokens`, `cached_input_tokens`, `output_tokens`, `total_tokens`, `cost_usd`; вложенный `summary` показывает токены и стоимость сжатия как часть общих затрат;
- `input_rejected`, `input_suspicious`, `warnings`: результат последних проверок и предупреждения.

У успешного `/api/chat` дополнительно возвращаются `model` и `answer`. Полный текст summary не выдаётся в API состояния — только признак наличия.

## Ограничения и диагностика

- Один процесс содержит один Agent: вкладки браузера разделяют список чатов, выбранный чат, общий режим, персонализацию и долговременную память. Это не многопользовательский сервис. Изоляция рабочей памяти относится к чатам, а не к пользователям.
- HTTP-запросы обрабатываются последовательно; LLM-вызовы могут занять время. Проверка ответа и обновление памяти расходуют дополнительные токены.
- Summary и автоматическое распределение facts зависят от модели и могут терять детали или ошибаться. Их результаты следует считать вспомогательной памятью, а не гарантированно точной базой знаний.
- Общие долговременные facts и рабочие facts выбранного чата включаются в контекст; embeddings и vector database не используются. Большое количество сохранённых данных увеличивает стоимость и размер контекста.
- Если порт `8080` занят, проверьте, не запущен ли старый сервер. Изменения исходников не применяются к уже запущенному executable.
- Ошибка `Permission denied` при сборке `main.exe` обычно означает, что executable ещё работает: остановите его перед повторной сборкой.
- Если интерфейс показывает старые элементы, перезапустите обновлённый сервер и выполните `Ctrl+F5`.
- API-ключ остаётся только на стороне C++ и не передаётся браузеру. На Windows сервер доступен только локально; на Linux слушает все IPv4-интерфейсы. Авторизации нет: публичный доступ к порту 8080 открывает доступ к общему Agent и его API; без дополнительной защиты это не production-сервис.
