const form = document.querySelector('#chatForm');
const input = document.querySelector('#messageInput');
const sendButton = document.querySelector('#sendButton');
const chatLog = document.querySelector('#chatLog');
const memoryDescription = document.querySelector('#memoryDescription');
const chatNotice = document.querySelector('#chatNotice');
const ragToggle = document.querySelector('#ragToggle');
const ragReindex = document.querySelector('#ragReindex');
const ragStatus = document.querySelector('#ragStatus');
const ragUpload = document.querySelector('#ragUpload');
const ragFileInput = document.querySelector('#ragFileInput');
const ragUploads = document.querySelector('#ragUploads');
const ragFileList = document.querySelector('#ragFileList');
let ragUploadPending = false;
let ragUploadsLoading = false;
let ragUploadsTimer;
let ragEnabled = false;
let ragIndexing = false;
const chatSelect = document.querySelector('#chatSelect');
const chatModeButtons = document.querySelectorAll('[data-chat-mode]');
const chatModeDescription = document.querySelector('#chatModeDescription');
const mcpPanel = document.querySelector('.mcp-panel');
const newChatForm = document.querySelector('#newChatForm');
const chatNameInput = document.querySelector('#chatNameInput');
const createChatButton = document.querySelector('#createChatButton');
const deleteChatButton = document.querySelector('#deleteChatButton');
const deleteChatConfirmation = document.querySelector('#deleteChatConfirmation');
const deleteChatConfirmationText = document.querySelector('#deleteChatConfirmationText');
const cancelDeleteChatButton = document.querySelector('#cancelDeleteChatButton');
const confirmDeleteChatButton = document.querySelector('#confirmDeleteChatButton');
const activeChatName = document.querySelector('#activeChatName');
const personalizationForm = document.querySelector('#personalizationForm');
const personalizationInput = document.querySelector('#personalizationInput');
const savePersonalizationButton = document.querySelector('#savePersonalizationButton');
const rawMessages = document.querySelector('#rawMessages');
const rawMessageLimit = document.querySelector('#rawMessageLimit');
const workingFacts = document.querySelector('#workingFacts');
const longTermFacts = document.querySelector('#longTermFacts');
const invariantFacts = document.querySelector('#invariantFacts');
const workingMemoryList = document.querySelector('#workingMemoryList');
const longTermMemoryList = document.querySelector('#longTermMemoryList');
const invariantList = document.querySelector('#invariantList');
const invariantForm = document.querySelector('#invariantForm');
const invariantText = document.querySelector('#invariantText');
const saveInvariantButton = document.querySelector('#saveInvariantButton');
const cancelInvariantEditButton = document.querySelector('#cancelInvariantEditButton');
const summaryStatus = document.querySelector('#summaryStatus');
const pendingSummaryMessages = document.querySelector('#pendingSummaryMessages');
const contextTokens = document.querySelector('#contextTokens');
const contextCost = document.querySelector('#contextCost');
const summaryTokens = document.querySelector('#summaryTokens');
const summaryCost = document.querySelector('#summaryCost');
const totalTokens = document.querySelector('#totalTokens');
const totalCost = document.querySelector('#totalCost');
const taskStatePanel = document.querySelector('#taskStatePanel');
const taskStateBadge = document.querySelector('#taskStateBadge');
const taskStateSteps = Array.from(document.querySelectorAll('.task-state-step'));
const taskStateActions = document.querySelector('#taskStateActions');
const workspace = document.querySelector('.workspace');
const chatSidebarToggle = document.querySelector('#chatSidebarToggle');
const mcpStatus = document.querySelector('#mcpStatus');
const mcpServerName = document.querySelector('#mcpServerName');
const mcpServerUrl = document.querySelector('#mcpServerUrl');
const mcpConnectButton = document.querySelector('#mcpConnectButton');
const mcpDisconnectButton = document.querySelector('#mcpDisconnectButton');
const mcpRefreshButton = document.querySelector('#mcpRefreshButton');
const mcpError = document.querySelector('#mcpError');
const mcpTools = document.querySelector('#mcpTools');
const mcpToolsToggle = document.querySelector('#mcpToolsToggle');
const mcpCallLog = document.querySelector('#mcpCallLog');
const mcpCallsToggle = document.querySelector('#mcpCallsToggle');
const reminderToolPanel = document.querySelector('#reminderToolPanel');
const reminderToolHome = reminderToolPanel.parentElement;
const reminderOverviewPanel = document.querySelector('#reminderOverviewPanel');
const reminderOverviewHome = reminderOverviewPanel.parentElement;
const reminderPipelineTools = ['get_upcoming_reminders', 'summarize_reminders', 'build_reminder_view'];

const TASK_PHASES = ['PLANNING', 'EXECUTION', 'VALIDATION', 'DONE'];
const TASK_STATES = [...TASK_PHASES, 'PAUSED'];
const TASK_TRANSITION_ENDPOINT = '/api/task/transition';

let requestPending = false;
let activeChatId = '';
let activeChatMode = 'task';
let availableChats = [];
let personalizationDirty = false;
let serverPersonalization = '';
let pendingDeleteChatId = '';
let currentTask = {
  state: 'PLANNING', resume_state: 'PLANNING', plan: '',
  validation_report: '', execution_completed: false, validation_passed: false
};
let editingInvariantKey = '';
let mcpPending = false;
let currentMcpStatus = 'Disconnected';
let currentMcpTools = [];

function mcpArgumentSummary(schema) {
  const properties = schema && typeof schema === 'object' && schema.properties &&
    typeof schema.properties === 'object' ? schema.properties : {};
  const required = new Set(Array.isArray(schema?.required) ? schema.required : []);
  const argumentsList = Object.entries(properties).map(([name, definition]) => {
    const type = definition && typeof definition.type === 'string' ? definition.type : 'value';
    return name + (required.has(name) ? '' : '?') + ': ' + type;
  });
  return '(' + argumentsList.join(', ') + ')';
}

function updateMcpControls() {
  const connected = currentMcpStatus === 'Connected';
  mcpConnectButton.disabled = mcpPending || connected;
  mcpDisconnectButton.disabled = mcpPending || currentMcpStatus === 'Disconnected';
  mcpRefreshButton.disabled = mcpPending || !connected;
  mcpTools.querySelectorAll('button[type="submit"]').forEach((button) => {
    button.disabled = mcpPending || !connected;
  });
  if (typeof updateReminderControls === 'function') updateReminderControls();
  if (typeof updateReminderOverviewControls === 'function') updateReminderOverviewControls();
}

function makeToolArgumentField(name, definition, required) {
  const label = document.createElement('label');
  label.className = 'mcp-argument-field';
  const caption = document.createElement('span');
  caption.textContent = name + (required ? '' : ' (optional)');
  label.append(caption);
  const type = typeof definition?.type === 'string' ? definition.type : 'string';
  let control;
  if (Array.isArray(definition?.enum)) {
    control = document.createElement('select');
    definition.enum.forEach((choice) => {
      const option = document.createElement('option');
      option.value = String(choice);
      option.textContent = String(choice);
      control.append(option);
    });
  } else if (type === 'boolean') {
    control = document.createElement('input');
    control.type = 'checkbox';
  } else if (type === 'object' || type === 'array') {
    control = document.createElement('textarea');
    control.placeholder = type === 'array' ? '[]' : '{}';
  } else {
    control = document.createElement('input');
    control.type = type === 'integer' || type === 'number' ? 'number' : 'text';
    if (type === 'integer') control.step = '1';
    if (type === 'number') control.step = 'any';
  }
  control.name = name;
  control.dataset.type = type;
  control.required = required;
  if (definition?.minimum !== undefined && control.type === 'number') control.min = definition.minimum;
  if (definition?.maximum !== undefined && control.type === 'number') control.max = definition.maximum;
  label.append(control);
  return label;
}

function renderMcpState(data = {}) {
  refreshMcpServers();
  const expandedTools = new Set(Array.from(mcpTools.querySelectorAll('.mcp-tool'))
    .filter((item) => item.open).map((item) => item.dataset.toolName));
  // Move the existing UI, rather than clone/recreate it: form values, event
  // listeners and form values survive tools refreshes.
  reminderToolHome.append(reminderToolPanel);
  reminderToolPanel.hidden = true;
  reminderOverviewHome.append(reminderOverviewPanel);
  reminderOverviewPanel.hidden = true;
  currentMcpTools = Array.isArray(data.tools) ? data.tools : [];
  currentMcpStatus = ['Connected', 'Disconnected', 'Error'].includes(data.status)
    ? data.status : 'Error';
  mcpStatus.textContent = currentMcpStatus;
  mcpStatus.dataset.status = currentMcpStatus.toLowerCase();
  mcpServerName.textContent = typeof data.server?.name === 'string'
    ? data.server.name : 'Local Tools Server';
  mcpServerUrl.textContent = typeof data.server?.url === 'string' ? data.server.url : '';
  const error = typeof data.error === 'string' ? data.error : '';
  mcpError.textContent = error;
  mcpError.hidden = !error;
  const visibleTools = currentMcpTools.filter((tool) => !reminderPipelineTools.includes(tool.name));
  const showOverview = currentMcpStatus === 'Connected' &&
    (currentMcpTools.some((tool) => tool.name === 'create_reminder') ||
     reminderPipelineTools.every((name) => currentMcpTools.some((tool) => tool.name === name)));
  // This is a backend workflow, not an additional server-side MCP tool.
  if (showOverview) {
    const overview = { name: 'reminder_overview', description: 'Сводка ближайших планов за 24 часа.', workflow: true };
    const createIndex = visibleTools.findIndex((tool) => tool.name === 'create_reminder');
    visibleTools.splice(createIndex >= 0 ? createIndex + 1 : 0, 0, overview);
  }
  mcpToolsToggle.textContent = 'Available tools (' + visibleTools.length + ')';
  mcpTools.replaceChildren();
  if (!Array.isArray(data.tools) || !data.tools.length) {
    mcpTools.textContent = currentMcpStatus === 'Connected'
      ? 'No tools published.' : 'Connect to discover tools.';
  } else {
    visibleTools.forEach((tool) => {
      const item = document.createElement('details');
      item.className = 'mcp-tool';
      item.dataset.toolName = String(tool.name || '');
      item.open = expandedTools.has(item.dataset.toolName);
      const summary = document.createElement('summary');
      const signature = document.createElement('strong');
      signature.textContent = tool.workflow ? 'Reminder Overview' : tool.name === 'create_reminder' ?
        'Create Reminder' : String(tool.name || '') + mcpArgumentSummary(tool.inputSchema);
      const description = document.createElement('p');
      description.textContent = typeof tool.description === 'string' ? tool.description : '';
      summary.append(signature);
      item.append(summary, description);
      if (tool.workflow) {
        reminderOverviewPanel.hidden = false;
        item.append(reminderOverviewPanel);
        mcpTools.append(item);
        return;
      }
      if (tool.name === 'create_reminder') {
        reminderToolPanel.hidden = false;
        item.append(reminderToolPanel);
        mcpTools.append(item);
        return;
      }
      const form = document.createElement('form');
      form.className = 'mcp-tool-form';
      const schema = tool.inputSchema && typeof tool.inputSchema === 'object' ? tool.inputSchema : {};
      const properties = schema.properties && typeof schema.properties === 'object' ? schema.properties : {};
      const required = new Set(Array.isArray(schema.required) ? schema.required : []);
      Object.entries(properties).forEach(([name, definition]) => {
        form.append(makeToolArgumentField(name, definition, required.has(name)));
      });
      const run = document.createElement('button');
      run.type = 'submit';
      run.className = 'secondary-button';
      run.textContent = 'Run tool';
      form.append(run);
      form.addEventListener('submit', (event) => {
        event.preventDefault();
        const args = {};
        for (const control of form.elements) {
          if (!control.name) continue;
          if (control.type === 'checkbox') args[control.name] = control.checked;
          else if (control.dataset.type === 'object' || control.dataset.type === 'array') {
            try { args[control.name] = JSON.parse(control.value || (control.dataset.type === 'array' ? '[]' : '{}')); }
            catch { mcpError.textContent = 'Invalid JSON for ' + control.name; mcpError.hidden = false; return; }
          } else if (control.type === 'number' && control.value === '' && !control.required) {
            continue;
          } else if (control.type === 'number') {
            args[control.name] = control.dataset.type === 'integer' ? Number.parseInt(control.value, 10) : Number(control.value);
          } else if (control.value !== '') args[control.name] = control.value;
        }
        runMcpTool(String(tool.name), args, run);
      });
      item.append(form);
      mcpTools.append(item);
    });
  }
  updateMcpControls();
}

async function runMcpTool(name, args, button) {
  if (mcpPending) return;
  if (name === 'create_reminder' && typeof requestReminderNotificationPermission === 'function') {
    requestReminderNotificationPermission();
  }
  mcpPending = true;
  button.disabled = true;
  updateMcpControls();
  mcpTools.querySelectorAll('button[type="submit"]').forEach((runButton) => { runButton.disabled = true; });
  try {
    const response = await fetch('/api/mcp/call', {
      method: 'POST', cache: 'no-store', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ name, arguments: JSON.stringify(args) })
    });
    renderMcpState(await response.json());
  } catch {
    mcpError.textContent = 'C++ backend is unavailable.';
    mcpError.hidden = false;
  } finally {
    mcpPending = false;
    updateMcpControls();
  }
}

async function requestMcp(path, method = 'GET') {
  if (activeChatMode !== 'assistant') return;
  if (mcpPending) return;
  mcpPending = true;
  updateMcpControls();
  try {
    const response = await fetch(path, { method, cache: 'no-store' });
    const data = await response.json();
    renderMcpState(data);
  } catch {
    renderMcpState({
      status: 'Error',
      server: { name: mcpServerName.textContent, url: mcpServerUrl.textContent },
      tools: [],
      error: 'C++ backend is unavailable.'
    });
  } finally {
    mcpPending = false;
    updateMcpControls();
  }
}

function resetInvariantForm() {
  editingInvariantKey = '';
  invariantForm.reset();
  saveInvariantButton.textContent = 'Add invariant';
  cancelInvariantEditButton.hidden = true;
}

function editInvariant(invariant) {
  editingInvariantKey = invariant.key;
  invariantText.value = invariant.value;
  saveInvariantButton.textContent = 'Save changes';
  cancelInvariantEditButton.hidden = false;
  invariantText.focus();
}

function renderInvariants(invariants) {
  invariantList.replaceChildren();
  if (!Array.isArray(invariants) || !invariants.length) {
    invariantList.textContent = 'No invariants yet.';
    return;
  }
  invariants.forEach((invariant) => {
    if (!invariant || typeof invariant.key !== 'string') return;
    const item = document.createElement('article');
    item.className = 'invariant-item';
    const value = document.createElement('p');
    value.textContent = typeof invariant.value === 'string' ? invariant.value : '';
    const actions = document.createElement('div');
    actions.className = 'invariant-item-actions';
    const edit = document.createElement('button');
    edit.type = 'button'; edit.className = 'secondary-button'; edit.textContent = 'Edit';
    edit.disabled = requestPending;
    edit.addEventListener('click', () => editInvariant(invariant));
    const remove = document.createElement('button');
    remove.type = 'button'; remove.className = 'danger-button'; remove.textContent = 'Delete';
    remove.disabled = requestPending;
    remove.addEventListener('click', () => {
      if (!requestPending && activeChatId && window.confirm('Delete invariant "' + invariant.key + '"?')) {
        mutateState('/api/invariants/delete', { project_id: activeChatId, key: invariant.key },
                    'Could not delete invariant.');
      }
    });
    actions.append(edit, remove);
    item.append(value, actions);
    invariantList.append(item);
  });
}

function closeDeleteConfirmation(restoreFocus = false) {
  pendingDeleteChatId = '';
  deleteChatConfirmation.hidden = true;
  deleteChatButton.setAttribute('aria-expanded', 'false');
  if (restoreFocus && !deleteChatButton.disabled) deleteChatButton.focus();
}

function openDeleteConfirmation() {
  if (requestPending || !activeChatId) return;
  pendingDeleteChatId = activeChatId;
  const chatName = chatSelect.selectedOptions[0]?.textContent || activeChatName.textContent || 'выбранный чат';
  deleteChatConfirmationText.textContent = '«' + chatName + '»: история, рабочая память, план, task state и project summary будут удалены. Общая долговременная память сохранится.';
  deleteChatConfirmation.hidden = false;
  deleteChatButton.setAttribute('aria-expanded', 'true');
  cancelDeleteChatButton.focus();
}

function updateControlState() {
  const busy = requestPending;
  ragToggle.disabled = busy;
  ragReindex.disabled = busy || ragIndexing;
  ragUpload.disabled = busy || ragUploadPending;
  const taskLocked = activeChatMode === 'task' && (currentTask.state === 'PAUSED' || currentTask.state === 'DONE');
  sendButton.disabled = busy || !activeChatId || taskLocked;
  input.disabled = busy || taskLocked;
  chatSelect.disabled = busy;
  chatModeButtons.forEach((button) => { button.disabled = busy; });
  chatNameInput.disabled = busy;
  createChatButton.disabled = busy;
  deleteChatButton.disabled = busy || !activeChatId;
  cancelDeleteChatButton.disabled = busy;
  confirmDeleteChatButton.disabled = busy || !pendingDeleteChatId;
  personalizationInput.disabled = busy;
  savePersonalizationButton.disabled = busy;
  invariantText.disabled = busy;
  saveInvariantButton.disabled = busy || !activeChatId;
  cancelInvariantEditButton.disabled = busy;
  invariantList.querySelectorAll('button').forEach((button) => { button.disabled = busy; });
  taskStateActions.querySelectorAll('.task-state-action').forEach((button) => {
    button.disabled = busy || !activeChatId;
  });
}

function formatTokenCount(value) { return Number(value || 0).toLocaleString(); }
function formatUsd(value) { return '$' + Number(value || 0).toFixed(6); }
function scrollToLatest() { chatLog.scrollTop = chatLog.scrollHeight; }

function setChatSidebarCollapsed(collapsed) {
  workspace.classList.toggle('chats-collapsed', collapsed);
  chatSidebarToggle.setAttribute('aria-expanded', String(!collapsed));
  chatSidebarToggle.textContent = collapsed ? 'Показать чаты' : 'Скрыть чаты';
}

function addMessage(role, text = '', scroll = true) {
  chatLog.querySelector('.chat-empty')?.remove();
  const message = document.createElement('article');
  const safeRole = ['user', 'assistant', 'loading', 'error'].includes(role) ? role : 'assistant';
  message.className = 'chat-message ' + safeRole;
  const label = document.createElement('p');
  label.className = 'message-role';
  label.textContent = safeRole === 'user' ? 'Вы' : safeRole === 'error' ? 'Ошибка' : 'Agent';
  const content = document.createElement('div');
  content.className = 'message-content';
  if (safeRole === 'loading') {
    content.classList.add('loading');
    content.innerHTML = '<div class="loader" aria-label="Ожидание"><i></i><i></i><i></i></div>';
  } else {
    content.textContent = typeof text === 'string' ? text : '';
  }
  message.append(label, content);
  chatLog.append(message);
  if (scroll) scrollToLatest();
  return message;
}

function taskAction(label, action, errorText, { variant = '', requiresPlan = false } = {}) {
  const button = document.createElement('button');
  button.type = 'button';
  button.className = 'task-state-action' + (variant ? ' ' + variant : '');
  button.textContent = label;
  button.dataset.requiresPlan = String(requiresPlan);
  button.disabled = requestPending || !activeChatId;
  if (requiresPlan && !currentTask.plan) {
    button.title = 'Сервер проверит наличие утверждаемого плана.';
  }
  button.addEventListener('click', async () => {
    const accepted = await mutateState(
      TASK_TRANSITION_ENDPOINT, { chat_id: activeChatId, action }, errorText,
      { keepPosition: false }
    );
    if (accepted && action === 'CREATE_TASK') input.focus();
  });
  return button;
}

function renderTaskActions() {
  taskStateActions.replaceChildren();
  if (activeChatMode !== 'task') return;
  if (currentTask.state === 'PLANNING') {
    taskStateActions.append(
      taskAction('Approve Plan / Начать выполнение', 'APPROVE_PLAN',
                 'Не удалось утвердить план.', { variant: 'primary', requiresPlan: true }),
      taskAction('Regenerate Plan / Переделать план', 'REGENERATE_PLAN',
                 'Не удалось переделать план.', { requiresPlan: true }),
      taskAction('Пауза', 'PAUSE', 'Не удалось поставить задачу на паузу.',
                 { variant: 'pause' })
    );
  } else if (currentTask.state === 'EXECUTION') {
    taskStateActions.append(
      ...(currentTask.execution_completed ? [taskAction(
        'Проверить / Перейти к validation', 'EXECUTION_FINISHED',
        'Не удалось запустить validation.', { variant: 'primary' })] : []),
      taskAction('Пауза', 'PAUSE', 'Не удалось поставить задачу на паузу.',
                 { variant: 'pause' })
    );
  } else if (currentTask.state === 'VALIDATION') {
    if (currentTask.validation_report) {
      taskStateActions.append(
        taskAction(currentTask.validation_passed ? 'Завершить задачу / DONE' : 'Вернуться к execution',
                   currentTask.validation_passed ? 'VALIDATION_PASSED' : 'VALIDATION_FAILED',
                   currentTask.validation_passed
                     ? 'Не удалось завершить задачу.'
                     : 'Не удалось вернуть задачу в execution.',
                   { variant: 'primary' })
      );
    }
  } else if (currentTask.state === 'DONE') {
    taskStateActions.append(
      taskAction('New Task / Новая задача', 'CREATE_TASK',
                 'Не удалось создать новую задачу.', { variant: 'primary' })
    );
  } else if (currentTask.state === 'PAUSED') {
    taskStateActions.append(
      taskAction('Resume / Продолжить', 'RESUME',
                 'Не удалось продолжить задачу.', { variant: 'primary' })
    );
  }
}

function renderConversation(messages = [], keepPosition = false) {
  const previousTop = chatLog.scrollTop;
  chatLog.replaceChildren();
  if (!Array.isArray(messages) || !messages.length) {
    if (activeChatMode === 'task' && currentTask.plan) {
      addMessage('assistant', currentTask.plan, false);
    } else {
      const empty = document.createElement('div');
      empty.className = 'chat-empty';
      empty.textContent = activeChatMode === 'task'
        ? 'Опишите задачу. Первое сообщение станет этапом PLANNING.'
        : 'Задайте вопрос или попросите выполнить действие через MCP.';
      chatLog.append(empty);
    }
  } else {
    messages.forEach((item) => {
      const message = addMessage(item.role, item.content, false);
      if (item.role === 'assistant') renderRagSources(message, item.rag_sources);
    });
  }
  if (keepPosition) chatLog.scrollTop = previousTop;
  else scrollToLatest();
}

function renderFacts(container, facts) {
  container.replaceChildren();
  if (!Array.isArray(facts) || !facts.length) {
    container.textContent = 'Пока нет данных.';
    return;
  }
  facts.forEach((fact) => {
    const item = document.createElement('div');
    item.className = 'fact-item';
    const key = document.createElement('strong');
    key.className = 'fact-key';
    key.textContent = typeof fact.key === 'string' ? fact.key : '';
    const value = document.createElement('span');
    value.textContent = typeof fact.value === 'string' ? fact.value : '';
    item.append(key, value);
    container.append(item);
  });
}

function renderNotice(data = {}) {
  const notices = [];
  if (data.input_suspicious) notices.push('Обнаружена возможная подмена инструкций. Правила Agent продолжают действовать.');
  if (Array.isArray(data.warnings)) {
    data.warnings.forEach((warning) => { if (typeof warning === 'string' && warning) notices.push(warning); });
  }
  chatNotice.textContent = notices.join(' ');
  chatNotice.hidden = notices.length === 0;
}

function showNotice(text) {
  chatNotice.textContent = text;
  chatNotice.hidden = !text;
}

function normalizeTaskState(value, fallback = 'PLANNING') {
  const normalized = typeof value === 'string' ? value.trim().toUpperCase() : '';
  return TASK_STATES.includes(normalized) ? normalized : fallback;
}

function renderTaskSteps() {
  const displayedState = currentTask.state === 'PAUSED'
    ? currentTask.resume_state : currentTask.state;
  const currentIndex = Math.max(0, TASK_PHASES.indexOf(displayedState));
  taskStateSteps.forEach((step, index) => {
    const stepState = step.dataset.taskState;
    const isCurrent = stepState === displayedState;
    const isCompleted = index < currentIndex ||
      (currentTask.state === 'DONE' && stepState === 'DONE');
    step.classList.toggle('is-current', isCurrent);
    step.classList.toggle('is-completed', isCompleted);
    step.classList.toggle('is-future', index > currentIndex);
    step.classList.toggle('is-paused', currentTask.state === 'PAUSED' && isCurrent);
    if (isCurrent) step.setAttribute('aria-current', 'step');
    else step.removeAttribute('aria-current');
    const marker = step.querySelector('.task-step-marker');
    if (marker) marker.textContent = isCompleted ? '✓' : String(index + 1);
  });
}

function renderTaskState(data) {
  const received = data?.task_state || {};
  const storedState = normalizeTaskState(received.state);
  const legacyPaused = received.paused === true;
  const state = storedState === 'PAUSED' || legacyPaused ? 'PAUSED' : storedState;
  let resumeState = normalizeTaskState(
    received.resume_state ?? received.paused_from_state ?? received.previous_state ??
      (storedState === 'PAUSED' ? 'PLANNING' : storedState)
  );
  if (resumeState === 'PAUSED') resumeState = 'PLANNING';
  currentTask = {
    state,
    resume_state: resumeState,
    plan: typeof received.plan === 'string' ? received.plan : '',
    validation_report: typeof received.validation_report === 'string' ? received.validation_report : '',
    execution_completed: received.execution_completed === true,
    validation_passed: received.validation_passed === true
  };
  taskStateBadge.textContent = state === 'PAUSED' ? 'PAUSED · ' + resumeState : state;
  taskStateBadge.classList.toggle('paused', state === 'PAUSED');
  taskStatePanel.classList.toggle('paused', state === 'PAUSED');
  taskStatePanel.classList.toggle('done', state === 'DONE');
  renderTaskSteps();
  renderTaskActions();
  const inputState = state === 'PAUSED' ? resumeState : state;
  input.placeholder = state === 'PAUSED' ? 'Задача на паузе'
    : inputState === 'PLANNING'
    ? currentTask.plan ? 'Напишите, что изменить в плане…' : 'Опишите новую задачу…'
    : inputState === 'EXECUTION' ? 'Добавьте уточнение к выполняемой задаче…'
      : inputState === 'VALIDATION' ? 'Добавьте уточнение к проверяемой задаче…'
        : 'Задача завершена';
}

function renderAgentState(data, { renderChat = true, keepPosition = false, forcePersonalization = false } = {}) {
  if (!data || !data.memory) return false;
  const previousActiveChatId = activeChatId;
  const previousChatMode = activeChatMode;
  activeChatId = String(data.active_chat_id || '');
  activeChatMode = data.active_chat_mode === 'assistant' ? 'assistant' : 'task';
  availableChats = Array.isArray(data.chats) ? data.chats : [];
  chatModeButtons.forEach((button) => {
    button.setAttribute('aria-pressed', String(button.dataset.chatMode === activeChatMode));
  });
  chatModeDescription.textContent = activeChatMode === 'task'
    ? 'Планирование → выполнение → проверка. MCP в этом чате недоступен.'
    : 'Обычный диалог и инструменты MCP. Без task state machine.';
  taskStatePanel.hidden = activeChatMode !== 'task';
  mcpPanel.hidden = activeChatMode !== 'assistant';
  if (previousActiveChatId && previousActiveChatId !== activeChatId) resetInvariantForm();
  activeChatName.textContent = String(data.active_chat_name || 'Чат');
  chatSelect.replaceChildren();
  availableChats.filter((chat) => (chat.mode || 'task') === activeChatMode).forEach((chat) => {
    const option = document.createElement('option');
    option.value = String(chat.id);
    option.textContent = String(chat.name);
    chatSelect.append(option);
  });
  chatSelect.value = activeChatId;
  if (pendingDeleteChatId &&
      (pendingDeleteChatId !== activeChatId || !chatSelect.querySelector('option:checked'))) {
    closeDeleteConfirmation();
  }
  serverPersonalization = typeof data.personalization === 'string' ? data.personalization : '';
  renderTaskState(data);
  if (activeChatMode === 'assistant') input.placeholder = 'Задайте вопрос или попросите выполнить действие через MCP…';
  if (!personalizationDirty || forcePersonalization) {
    personalizationInput.value = serverPersonalization;
    personalizationDirty = false;
  }
  const memory = data.memory;
  rawMessages.textContent = String(memory.raw_messages || 0);
  rawMessageLimit.textContent = String(memory.raw_message_limit || 5);
  workingFacts.textContent = String(memory.working_facts || 0);
  longTermFacts.textContent = String(memory.long_term_facts || 0);
  invariantFacts.textContent = String(memory.invariant_facts || 0);
  summaryStatus.textContent = memory.summary_present ? 'Активный' : 'Пустой';
  pendingSummaryMessages.textContent = String(memory.pending_summary_messages || 0);
  memoryDescription.textContent = 'Последние ' + (memory.raw_message_limit || 5) + ' сообщений и summary каждые ' + (memory.summary_every_requests || 5) + ' запросов — только этого чата. Рабочая память изолирована; долговременная общая.';
  contextTokens.textContent = formatTokenCount(data.usage?.input_tokens) + ' токенов';
  contextCost.textContent = formatUsd(data.usage?.cost_usd?.input);
  summaryTokens.textContent = formatTokenCount(data.usage?.summary?.total_tokens) + ' токенов';
  summaryCost.textContent = formatUsd(data.usage?.summary?.cost_usd?.total);
  totalTokens.textContent = formatTokenCount(data.usage?.total_tokens) + ' токенов';
  totalCost.textContent = formatUsd(data.usage?.cost_usd?.total);
  renderFacts(workingMemoryList, data.working_memory);
  renderFacts(longTermMemoryList, data.long_term_memory);
  renderInvariants(data.project_invariants);
  if (renderChat) renderConversation(data.conversation, keepPosition);
  renderNotice(data);
  updateControlState();
  if (activeChatMode === 'assistant' && previousChatMode !== 'assistant') requestMcp('/api/mcp/tools');
  return true;
}

async function loadAgentState() {
  if (requestPending) return;
  requestPending = true;
  updateControlState();
  const previousChatId = activeChatId;
  try {
    const response = await fetch('/api/state', { cache: 'no-store' });
    const data = await response.json();
    if (!response.ok || !renderAgentState(data, { keepPosition: !!previousChatId && previousChatId === String(data.active_chat_id) })) throw new Error('State request failed');
  } catch {
    showNotice('Сервер недоступен. Запустите локальный C++-сервер и обновите страницу.');
  } finally {
    requestPending = false;
    updateControlState();
  }
}

async function mutateState(url, payload, errorText, { forcePersonalization = false, keepPosition = true } = {}) {
  if (requestPending) return false;
  requestPending = true;
  updateControlState();
  renderNotice();
  try {
    const response = await fetch(url, {
      method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(payload)
    });
    const data = await response.json();
    if (!response.ok) {
      // Another tab may have changed the shared mode/active chat. Reconcile safe
      // server state even on rejection so stale buttons cannot remain enabled.
      if (data.memory) renderAgentState(data, { keepPosition, forcePersonalization });
      if (url === '/api/personalization') {
        // Do not retain a rejected draft (it may contain a detected secret).
        personalizationInput.value = serverPersonalization;
        personalizationDirty = false;
      }
      const serverTransitionError = url === TASK_TRANSITION_ENDPOINT
        ? safeTransitionError(data.error, errorText) : '';
      showNotice(serverTransitionError || (data.input_rejected && !url.startsWith('/api/task/')
          ? 'Данные отклонены input policy. Не сохраняйте секреты и опасные команды.' : errorText));
      return false;
    }
    if (!renderAgentState(data, { keepPosition, forcePersonalization })) throw new Error('State response missing');
    return true;
  } catch {
    if (url === '/api/personalization') {
      personalizationInput.value = serverPersonalization;
      personalizationDirty = false;
    }
    showNotice(errorText);
    return false;
  } finally {
    requestPending = false;
    updateControlState();
  }
}

function safeTransitionError(value, fallback) {
  if (typeof value !== 'string') return fallback;
  const message = value.replace(/[\u0000-\u001f\u007f]/g, ' ').replace(/\s+/g, ' ').trim();
  if (!message || message.length > 240 ||
      /(?:bearer\s+|api[-_ ]?key|password|private[-_ ]?key|\btoken\b|\bsk-[a-z0-9_-]+)/i.test(message)) {
    return fallback;
  }
  return message;
}

async function askAgent(question) {
  if (requestPending || !activeChatId) return;
  const requestChatId = activeChatId;
  const useRag = ragEnabled;
  const pendingMessage = addMessage('loading');
  requestPending = true;
  updateControlState();
  // No optimistic echo: only accepted messages from C++ enter the transcript.
  input.value = '';
  input.style.height = 'auto';
  renderNotice();
  try {
    let response, data;
    while (true) {
      response = await fetch('/api/chat', {
        method: 'POST', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ chat_id: requestChatId, message: question, rag: useRag })
      });
      data = await response.json();
      if (response.status !== 202 || !data.rag_indexing) break;
      pendingMessage.querySelector('.message-role').textContent = 'Создаём индекс проекта…';
      await waitForRagIndex();
      pendingMessage.querySelector('.message-role').textContent = 'Agent';
    }
    if (!response.ok) {
      pendingMessage.remove();
      const serverError = typeof data.error === 'string' ? data.error : '';
      const openaiStatus = /OpenAI API returned HTTP (\d{3})/.exec(serverError);
      const openaiParam = /; param=(tools|messages|model|tool_choice|parallel_tool_calls|response_format|max_completion_tokens|max_tokens)(?:;|$)/.exec(serverError);
      const lifecycleError = data.input_rejected && /^(Переход между этапами|Task stages can change|Task changes are not allowed|Task is paused|Task is complete|Create a plan|Complete execution|Only an executing task)/.test(serverError);
      const error = data.rag_error ? 'RAG: ' + (serverError || 'Не удалось получить контекст проекта.')
        : lifecycleError ? serverError : data.input_rejected
        ? 'Сообщение отклонено input policy. Уберите секреты или запросы на выполнение опасных команд.'
        : openaiStatus ? 'Ошибка OpenAI: HTTP ' + openaiStatus[1] +
          (openaiParam ? ' · параметр ' + openaiParam[1] : '') + '. Подробности — в ответе /api/chat.'
        : 'Не удалось завершить запрос. Попробуйте ещё раз.';
      // Provider error bodies and rejected user text are never shown.
      // Rebuild the transcript as well: another tab may have changed the
      // lifecycle while this request was in flight, so stale inline actions
      // must never remain below old messages.
      if (data.memory) renderAgentState(data);
      addMessage('error', error);
      return;
    }
    if (!renderAgentState(data)) throw new Error('State response missing');
  } catch (error) {
    pendingMessage.remove();
    addMessage('error', error.ragError ? 'RAG: ' + error.message
      : 'Не удалось связаться с сервером. Сообщение не добавлено в видимую историю.');
  } finally {
    requestPending = false;
    updateControlState();
    refreshMcpServers();
    input.focus();
  }
}

form.addEventListener('submit', (event) => {
  event.preventDefault();
  const question = input.value.trim();
  if (question && !sendButton.disabled) askAgent(question);
});
input.addEventListener('keydown', (event) => {
  if (event.key === 'Enter' && !event.shiftKey) { event.preventDefault(); form.requestSubmit(); }
});
input.addEventListener('input', () => {
  input.style.height = 'auto';
  input.style.height = Math.min(input.scrollHeight, 110) + 'px';
});
newChatForm.addEventListener('submit', async (event) => {
  event.preventDefault();
  const name = chatNameInput.value.trim();
  if (!name || requestPending) return;
  closeDeleteConfirmation();
  // Clear immediately so rejected names never remain as a visible draft.
  chatNameInput.value = '';
  if (await mutateState('/api/chats', { name, mode: activeChatMode }, 'Не удалось создать чат.', { keepPosition: false })) {
    input.value = '';
    input.style.height = 'auto';
    input.focus();
  }
});
chatModeButtons.forEach((button) => button.addEventListener('click', async () => {
  const mode = button.dataset.chatMode;
  if (requestPending || mode === activeChatMode) return;
  closeDeleteConfirmation();
  const chat = availableChats.find((item) => item.mode === mode);
  const accepted = chat
    ? await mutateState('/api/chats/select', { chat_id: String(chat.id) }, 'Не удалось переключить чат.', { keepPosition: false })
    : await mutateState('/api/chats', { name: mode === 'task' ? 'Задачи' : 'Ассистент / MCP', mode }, 'Не удалось создать чат.', { keepPosition: false });
  if (accepted) { input.value = ''; input.style.height = 'auto'; input.focus(); }
}));
chatSelect.addEventListener('change', async () => {
  const selectedChatId = chatSelect.value;
  if (requestPending || !selectedChatId || selectedChatId === activeChatId) return;
  closeDeleteConfirmation();
  const accepted = await mutateState('/api/chats/select', { chat_id: selectedChatId }, 'Не удалось переключить чат.', { keepPosition: false });
  if (accepted) {
    input.value = '';
    input.style.height = 'auto';
    input.focus();
  } else chatSelect.value = activeChatId;
});
deleteChatButton.addEventListener('click', openDeleteConfirmation);
cancelDeleteChatButton.addEventListener('click', () => closeDeleteConfirmation(true));
confirmDeleteChatButton.addEventListener('click', async () => {
  const chatId = pendingDeleteChatId;
  if (requestPending || !chatId) return;
  closeDeleteConfirmation();
  if (await mutateState('/api/chats/delete', { chat_id: chatId }, 'Не удалось удалить чат.', { keepPosition: false })) {
    input.value = '';
    input.style.height = 'auto';
    input.focus();
  }
});
personalizationInput.addEventListener('input', () => { personalizationDirty = true; });
personalizationForm.addEventListener('submit', (event) => {
  event.preventDefault();
  if (requestPending) return;
  const text = personalizationInput.value.trim();
  personalizationInput.value = serverPersonalization;
  personalizationDirty = false;
  mutateState('/api/personalization', { text }, 'Не удалось сохранить персонализацию.', { forcePersonalization: true });
});
invariantForm.addEventListener('submit', async (event) => {
  event.preventDefault();
  if (requestPending || !activeChatId) return;
  const text = invariantText.value.trim();
  if (!text) return;
  const updating = !!editingInvariantKey;
  const endpoint = updating ? '/api/invariants/update' : '/api/invariants';
  const key = updating ? editingInvariantKey : 'restriction.' + Date.now();
  const payload = {
    project_id: activeChatId,
    key,
    value: text,
    description: 'Explicit user-defined project restriction.'
  };
  if (updating) payload.current_key = editingInvariantKey;
  if (await mutateState(endpoint, payload,
                        updating ? 'Could not update invariant.' : 'Could not add invariant.')) {
    resetInvariantForm();
  }
});
cancelInvariantEditButton.addEventListener('click', resetInvariantForm);
chatSidebarToggle.addEventListener('click', () => {
  setChatSidebarCollapsed(!workspace.classList.contains('chats-collapsed'));
});
mcpToolsToggle.addEventListener('click', () => {
  const expanded = mcpToolsToggle.getAttribute('aria-expanded') === 'true';
  mcpToolsToggle.setAttribute('aria-expanded', String(!expanded));
  mcpTools.hidden = expanded;
});
mcpCallsToggle.addEventListener('click', () => {
  const expanded = mcpCallsToggle.getAttribute('aria-expanded') === 'true';
  mcpCallsToggle.setAttribute('aria-expanded', String(!expanded));
  mcpCallLog.hidden = expanded;
});
mcpConnectButton.addEventListener('click', () => requestMcp('/api/mcp/connect', 'POST'));
mcpDisconnectButton.addEventListener('click', () => requestMcp('/api/mcp/disconnect', 'POST'));
mcpRefreshButton.addEventListener('click', () => requestMcp('/api/mcp/tools'));

loadAgentState();

let mcpServersLoading = false;
let mcpServerActionPending = false;
let mcpServersSignature = '';

function renderMcpServers(data) {
  const container = document.querySelector('#mcpServers');
  const signature = JSON.stringify(data.servers) + String(mcpServerActionPending);
  if (signature !== mcpServersSignature) {
    mcpServersSignature = signature;
    const reminderActions = mcpConnectButton.parentElement;
    // Keep the original controls and listeners when rebuilding server cards.
    container.parentElement.append(reminderActions);
    container.replaceChildren();
    (Array.isArray(data.servers) ? data.servers : []).forEach((server) => {
    const card = document.createElement('div');
    card.className = 'mcp-server-card';
    const heading = document.createElement('div');
    heading.className = 'mcp-heading';
    const name = document.createElement('strong');
    name.textContent = String(server.name);
    const status = document.createElement('span');
    status.className = 'mcp-status';
    status.textContent = String(server.status);
    status.dataset.status = String(server.status).toLowerCase();
    heading.append(name, status);
    const detail = document.createElement('small');
    detail.textContent = String(server.tool_count) + ' tools · ' + String(server.url);
    card.append(heading, detail);
    if (server.id === 'reminder') card.append(reminderActions);
    if (server.id === 'codeforces') {
      const button = document.createElement('button');
      button.className = 'secondary-button';
      button.type = 'button';
      button.textContent = server.status === 'Connected' ? 'Disconnect' : 'Connect';
      button.disabled = mcpServerActionPending;
      button.addEventListener('click', async () => {
        if (mcpServerActionPending) return;
        mcpServerActionPending = true;
        button.disabled = true;
        try {
          const response = await fetch('/api/mcp/server/connection', {
            method: 'POST', headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ server_id: server.id, action: server.status === 'Connected' ? 'disconnect' : 'connect' })
          });
          if (!response.ok) throw new Error('Не удалось изменить подключение Codeforces MCP.');
          renderMcpServers(await response.json());
        } catch {
          const error = document.querySelector('#mcpServersError');
          error.textContent = 'Не удалось изменить подключение Codeforces MCP.';
          error.hidden = false;
        } finally {
          mcpServerActionPending = false;
          refreshMcpServers();
        }
      });
      card.append(button);
    }
    container.append(card);
    });
  }
  const timestamp = data.codeforces?.last_sync_at;
  document.querySelector('#codeforcesLastSync').textContent = timestamp == null ? 'Ещё не было' :
    new Intl.DateTimeFormat('ru-RU', { timeZone: 'Europe/Moscow', day: 'numeric', month: 'long',
      hour: '2-digit', minute: '2-digit' }).format(new Date(Number(timestamp) * 1000)) + ' МСК';
  document.querySelector('#codeforcesNewCount').textContent = String(data.codeforces?.new_count ?? 0);
  const log = document.querySelector('#mcpRoutingCalls');
  log.replaceChildren();
  const calls = Array.isArray(data.calls) ? data.calls : [];
  if (!calls.length) log.textContent = 'Вызовов пока нет.';
  calls.forEach((call) => {
    const row = document.createElement('p');
    row.textContent = String(call.server_id || 'unknown') + ' → ' + String(call.tool) +
      ' · ' + (call.success ? 'success' : 'error');
    log.append(row);
  });
}

async function refreshMcpServers() {
  if (mcpServersLoading) return;
  mcpServersLoading = true;
  try {
    const response = await fetch('/api/mcp/servers');
    if (!response.ok) throw new Error();
    renderMcpServers(await response.json());
    document.querySelector('#mcpServersError').hidden = true;
  } catch {
    const error = document.querySelector('#mcpServersError');
    error.textContent = 'Статусы MCP недоступны. Проверьте backend.';
    error.hidden = false;
  } finally { mcpServersLoading = false; }
}
document.querySelector('#mcpServersRefresh').addEventListener('click', refreshMcpServers);
refreshMcpServers();
// Read-only cached backend state; never polls the public Codeforces REST API.
setInterval(() => { if (!document.hidden) refreshMcpServers(); }, 15000);
// Do not refresh on window focus: the refresh marks the UI busy and can swallow
// the first click on actions such as project deletion. Every mutation already
// returns the complete current Agent state.

// Retrieval stays in the existing chat flow. Only source metadata reaches this UI.
function renderRagSources(message, sources) {
  if (!Array.isArray(sources) || !sources.length) return;
  const details = document.createElement('details');
  details.className = 'rag-sources';
  const summary = document.createElement('summary');
  summary.textContent = 'Источники RAG';
  const list = document.createElement('ul');
  const seen = new Set();
  sources.forEach((source) => {
    const location = source.page > 0 ? ' · стр. ' + source.page
      : source.line_start > 0 ? ' · строки ' + source.line_start + '–' + source.line_end : '';
    const label = String(source.file || '') + ' — ' + String(source.section || 'Общий раздел') + location;
    if (seen.has(label)) return;
    seen.add(label);
    const item = document.createElement('li');
    item.textContent = label;
    list.append(item);
  });
  details.append(summary, list);
  message.append(details);
}

function showRagStatus(text, failed = false) {
  ragStatus.textContent = text;
  ragStatus.hidden = !text;
  ragStatus.classList.toggle('failed', failed);
}

async function waitForRagIndex() {
  ragIndexing = true;
  updateControlState();
  try {
    while (true) {
      const response = await fetch('/api/rag/status', { cache: 'no-store' });
      if (!response.ok) throw new Error('Статус индекса недоступен.');
      const state = await response.json();
      if (state.error) throw new Error(state.error);
      if (!state.running) {
        if (!state.ready) throw new Error('Индекс не создан. Повторите Reindex.');
        showRagStatus('Индекс готов.');
        return;
      }
      showRagStatus('Индексация: ' + (state.phase || '…') +
        (state.total ? ' · ' + state.progress + ' / ' + state.total : ''));
      await new Promise((resolve) => setTimeout(resolve, 1000));
    }
  } catch (error) {
    showRagStatus(error.message, true);
    error.ragError = true;
    throw error;
  } finally {
    ragIndexing = false;
    updateControlState();
  }
}

ragToggle.addEventListener('click', () => {
  ragEnabled = !ragEnabled;
  ragToggle.textContent = ragEnabled ? 'RAG ON' : 'RAG OFF';
  ragToggle.setAttribute('aria-checked', String(ragEnabled));
  if (!ragIndexing) showRagStatus(ragEnabled ? 'Ответы с контекстом проекта и загруженных документов.' : '');
});

ragReindex.addEventListener('click', async () => {
  if (requestPending || ragIndexing) return;
  ragIndexing = true;
  updateControlState();
  showRagStatus('Перестраиваем индекс проекта…');
  try {
    const response = await fetch('/api/rag/reindex', { method: 'POST' });
    const data = await response.json();
    if (!response.ok) throw new Error(data.error || 'Не удалось запустить индексацию.');
    await waitForRagIndex();
  } catch (error) {
    showRagStatus(error.message, true);
  } finally {
    ragIndexing = false;
    updateControlState();
  }
});

async function refreshRagUploads() {
  if (ragUploadsLoading) return;
  ragUploadsLoading = true;
  clearTimeout(ragUploadsTimer);
  try {
    const response = await fetch('/api/rag/uploads', { cache: 'no-store' });
    const data = await response.json();
    if (!response.ok || data.error) throw new Error(data.error || 'Статусы документов недоступны.');
    const files = Array.isArray(data.files) ? data.files : [];
    ragUploads.hidden = !files.length;
    ragFileList.replaceChildren();
    const labels = { uploaded: 'загружен', indexing: 'индексируется', indexed: 'проиндексирован', error: 'ошибка' };
    files.forEach((file) => {
      const item = document.createElement('li');
      item.dataset.status = file.status;
      item.textContent = file.name + ' — ' + (labels[file.status] || file.status) + (file.error ? ': ' + file.error : '');
      ragFileList.append(item);
    });
    if (files.some((file) => file.status === 'uploaded' || file.status === 'indexing'))
      ragUploadsTimer = setTimeout(refreshRagUploads, 1000);
  } catch (error) {
    showRagStatus(error.message, true);
  } finally { ragUploadsLoading = false; }
}

ragUpload.addEventListener('click', () => ragFileInput.click());
ragFileInput.addEventListener('change', async () => {
  const files = Array.from(ragFileInput.files || []);
  ragFileInput.value = '';
  if (!files.length || ragUploadPending) return;
  ragUploadPending = true;
  updateControlState();
  ragUploads.open = true;
  try {
    for (const file of files) {
      if (!/\.(txt|md|pdf|docx)$/i.test(file.name) || !file.size || file.size > 10 * 1024 * 1024) {
        showRagStatus(file.name + ': поддерживаются TXT, MD, PDF, DOCX до 10 МБ.', true);
        continue;
      }
      const response = await fetch('/api/rag/upload', {
        method: 'POST', headers: { 'Content-Type': 'application/octet-stream', 'X-File-Name': encodeURIComponent(file.name) }, body: file
      });
      const data = await response.json();
      if (!response.ok) { showRagStatus(file.name + ': ' + (data.error || 'Ошибка загрузки.'), true); continue; }
      await refreshRagUploads();
    }
  } catch {
    showRagStatus('Не удалось загрузить документ. Проверьте соединение с backend.', true);
  } finally { ragUploadPending = false; updateControlState(); }
});
refreshRagUploads();
