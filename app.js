const form = document.querySelector('#chatForm');
const input = document.querySelector('#messageInput');
const sendButton = document.querySelector('#sendButton');
const chatLog = document.querySelector('#chatLog');
const memoryDescription = document.querySelector('#memoryDescription');
const chatNotice = document.querySelector('#chatNotice');
const chatSelect = document.querySelector('#chatSelect');
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

const TASK_PHASES = ['PLANNING', 'EXECUTION', 'VALIDATION', 'DONE'];
const TASK_STATES = [...TASK_PHASES, 'PAUSED'];
const TASK_TRANSITION_ENDPOINT = '/api/task/transition';

let requestPending = false;
let activeChatId = '';
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
  mcpToolsToggle.textContent = 'Available tools (' + (Array.isArray(data.tools) ? data.tools.length : 0) + ')';
  mcpTools.replaceChildren();
  if (!Array.isArray(data.tools) || !data.tools.length) {
    mcpTools.textContent = currentMcpStatus === 'Connected'
      ? 'No tools published.' : 'Connect to discover tools.';
  } else {
    data.tools.forEach((tool) => {
      const item = document.createElement('details');
      item.className = 'mcp-tool';
      const summary = document.createElement('summary');
      const signature = document.createElement('strong');
      signature.textContent = String(tool.name || '') + mcpArgumentSummary(tool.inputSchema);
      const description = document.createElement('p');
      description.textContent = typeof tool.description === 'string' ? tool.description : '';
      summary.append(signature);
      item.append(summary, description);
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
  renderMcpCalls(data.calls);
  updateMcpControls();
}

function renderMcpCalls(calls) {
  mcpCallLog.replaceChildren();
  if (!Array.isArray(calls) || calls.length === 0) {
    mcpCallLog.textContent = 'No tool calls yet.';
    return;
  }
  calls.forEach((call) => {
    const item = document.createElement('article');
    item.className = 'mcp-call';
    const header = document.createElement('div');
    header.className = 'mcp-call-header';
    const name = document.createElement('strong');
    name.textContent = String(call.name || 'Unknown tool');
    const status = document.createElement('span');
    status.className = 'mcp-call-status' + (call.success === true ? '' : ' error');
    status.textContent = call.success === true ? 'Success' : 'Error';
    header.append(name, status);
    const args = document.createElement('pre');
    args.textContent = typeof call.arguments === 'string' ? call.arguments : '{}';
    item.append(header, args);
    if (call.result !== null && call.result !== undefined) {
      const result = document.createElement('pre');
      result.textContent = JSON.stringify(call.result, null, 2);
      item.append(result);
    }
    if (call.success !== true && typeof call.error === 'string' && call.error) {
      const error = document.createElement('p');
      error.className = 'mcp-call-error';
      error.textContent = call.error;
      item.append(error);
    }
    mcpCallLog.append(item);
  });
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
  const taskLocked = currentTask.state === 'PAUSED' || currentTask.state === 'DONE';
  sendButton.disabled = busy || !activeChatId || taskLocked;
  input.disabled = busy || taskLocked;
  chatSelect.disabled = busy;
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
    if (currentTask.plan) {
      addMessage('assistant', currentTask.plan, false);
    } else {
      const empty = document.createElement('div');
      empty.className = 'chat-empty';
      empty.textContent = 'Опишите задачу. Первое сообщение станет этапом PLANNING.';
      chatLog.append(empty);
    }
  } else {
    messages.forEach((item) => {
      addMessage(item.role, item.content, false);
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
  activeChatId = String(data.active_chat_id || '');
  if (previousActiveChatId && previousActiveChatId !== activeChatId) resetInvariantForm();
  activeChatName.textContent = String(data.active_chat_name || 'Чат');
  chatSelect.replaceChildren();
  if (Array.isArray(data.chats)) data.chats.forEach((chat) => {
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
  const pendingMessage = addMessage('loading');
  requestPending = true;
  updateControlState();
  // No optimistic echo: only accepted messages from C++ enter the transcript.
  input.value = '';
  input.style.height = 'auto';
  renderNotice();
  try {
    const response = await fetch('/api/chat', {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ chat_id: requestChatId, message: question })
    });
    const data = await response.json();
    if (!response.ok) {
      pendingMessage.remove();
      const serverError = typeof data.error === 'string' ? data.error : '';
      const lifecycleError = data.input_rejected && /^(Переход между этапами|Task stages can change|Task changes are not allowed|Task is paused|Task is complete|Create a plan|Complete execution|Only an executing task)/.test(serverError);
      const error = lifecycleError ? serverError : data.input_rejected
        ? 'Сообщение отклонено input policy. Уберите секреты или запросы на выполнение опасных команд.'
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
  } catch {
    pendingMessage.remove();
    addMessage('error', 'Не удалось связаться с сервером. Сообщение не добавлено в видимую историю.');
  } finally {
    requestPending = false;
    updateControlState();
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
  if (await mutateState('/api/chats', { name }, 'Не удалось создать чат.', { keepPosition: false })) {
    input.value = '';
    input.style.height = 'auto';
    input.focus();
  }
});
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
mcpConnectButton.addEventListener('click', () => requestMcp('/api/mcp/connect', 'POST'));
mcpDisconnectButton.addEventListener('click', () => requestMcp('/api/mcp/disconnect', 'POST'));
mcpRefreshButton.addEventListener('click', () => requestMcp('/api/mcp/tools'));

loadAgentState();
requestMcp('/api/mcp/tools');
// Do not refresh on window focus: the refresh marks the UI busy and can swallow
// the first click on actions such as project deletion. Every mutation already
// returns the complete current Agent state.
