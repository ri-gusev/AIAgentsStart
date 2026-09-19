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
const workingMemoryList = document.querySelector('#workingMemoryList');
const longTermMemoryList = document.querySelector('#longTermMemoryList');
const summaryStatus = document.querySelector('#summaryStatus');
const pendingSummaryMessages = document.querySelector('#pendingSummaryMessages');
const contextTokens = document.querySelector('#contextTokens');
const contextCost = document.querySelector('#contextCost');
const summaryTokens = document.querySelector('#summaryTokens');
const summaryCost = document.querySelector('#summaryCost');
const totalTokens = document.querySelector('#totalTokens');
const totalCost = document.querySelector('#totalCost');
const taskStateBadge = document.querySelector('#taskStateBadge');
const chatStateBanner = document.querySelector('#chatStateBanner');
const chatStateName = document.querySelector('#chatStateName');
const chatStateHint = document.querySelector('#chatStateHint');

let requestPending = false;
let activeChatId = '';
let personalizationDirty = false;
let serverPersonalization = '';
let pendingDeleteChatId = '';
let currentTask = { state: 'planning', plan: '', validation_report: '', paused: false };
let currentProjectSummary = '';

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
  const taskLocked = currentTask.paused || currentTask.state === 'DONE';
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
  chatLog.querySelectorAll('.message-task-action').forEach((button) => {
    button.disabled = busy || !activeChatId;
  });
}

function formatTokenCount(value) { return Number(value || 0).toLocaleString(); }
function formatUsd(value) { return '$' + Number(value || 0).toFixed(6); }
function scrollToLatest() { chatLog.scrollTop = chatLog.scrollHeight; }

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

function taskAction(label, endpoint, errorText, extraPayload = {}) {
  const button = document.createElement('button');
  button.type = 'button';
  button.className = 'message-task-action';
  button.textContent = label;
  button.disabled = requestPending || !activeChatId;
  button.addEventListener('click', () => {
    mutateState(endpoint, { chat_id: activeChatId, ...extraPayload }, errorText,
                { keepPosition: false });
  });
  return button;
}

function renderInlineTaskControls() {
  const lastAssistant = [...chatLog.querySelectorAll('.chat-message.assistant')].at(-1);
  if (!lastAssistant) return;

  const controls = document.createElement('div');
  controls.className = 'message-task-controls';
  const status = document.createElement('span');
  status.className = 'message-task-status';
  status.textContent = currentTask.paused ? currentTask.state + ' · PAUSE' : currentTask.state;
  controls.append(status);

  if (currentTask.paused) {
    controls.append(taskAction('Продолжить', '/api/task/resume', 'Не удалось продолжить задачу.'));
  } else if (currentTask.state === 'planning' && currentTask.plan) {
    controls.append(
      taskAction('Утвердить план', '/api/task/approve', 'Не удалось утвердить план.'),
      taskAction('Переделать план', '/api/task/revise', 'Не удалось переделать план.', { feedback: '' })
    );
  } else if (currentTask.state === 'execution') {
    controls.append(
      taskAction('Проверить', '/api/task/validation', 'Не удалось перейти к проверке.'),
      taskAction('Пауза', '/api/task/pause', 'Не удалось поставить задачу на паузу.')
    );
  } else if (currentTask.state === 'validation') {
    if (currentTask.validation_report) {
      controls.append(
        taskAction('Вернуться к execution', '/api/task/execution', 'Не удалось вернуться к выполнению.'),
        taskAction('Повторить проверку', '/api/task/validate', 'Не удалось повторить проверку.')
      );
    } else {
      controls.append(
        taskAction('Завершить проверку', '/api/task/validate', 'Не удалось выполнить проверку.'),
        taskAction('Пауза', '/api/task/pause', 'Не удалось поставить задачу на паузу.')
      );
    }
  } else if (currentTask.state === 'DONE') {
    status.textContent = 'DONE · задача завершена';
  }

  if (currentTask.validation_report || currentProjectSummary) {
    const details = document.createElement('details');
    details.className = 'message-task-details';
    const summary = document.createElement('summary');
    summary.textContent = currentProjectSummary ? 'Контекст задачи' : 'Результат проверки';
    const text = document.createElement('div');
    text.textContent = currentProjectSummary || currentTask.validation_report;
    details.append(summary, text);
    controls.append(details);
  }
  lastAssistant.append(controls);
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
      empty.textContent = 'Опишите задачу. Первое сообщение станет этапом planning.';
      chatLog.append(empty);
    }
  } else {
    messages.forEach((message) => addMessage(message.role, message.content, false));
  }
  renderInlineTaskControls();
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

function renderTaskState(data) {
  const allowedStates = ['planning', 'execution', 'validation', 'DONE'];
  const received = data?.task_state || {};
  const state = allowedStates.includes(received.state) ? received.state : 'planning';
  currentTask = {
    state,
    plan: typeof received.plan === 'string' ? received.plan : '',
    validation_report: typeof received.validation_report === 'string' ? received.validation_report : '',
    paused: received.paused === true
  };
  taskStateBadge.textContent = currentTask.paused ? state + ' · PAUSE' : state;
  taskStateBadge.classList.toggle('paused', currentTask.paused);
  chatStateBanner.dataset.state = currentTask.paused ? 'paused' : state;
  chatStateName.textContent = currentTask.paused ? state.toUpperCase() + ' · PAUSE' : state.toUpperCase();
  const stateHints = {
    planning: currentTask.plan ? 'Утвердите план или отправьте замечания для новой версии.' : 'Опишите задачу — Agent составит план.',
    execution: 'Agent выполняет задачу. После получения результата нажмите «Проверить».',
    validation: currentTask.validation_report ? 'Проверка не пройдена. Верните задачу в execution для исправлений.' : 'Результат проверяется относительно утверждённого плана.',
    DONE: 'Проверка пройдена, задача завершена.'
  };
  chatStateHint.textContent = currentTask.paused ? 'Проект на паузе; состояние и summary сохранены.' : stateHints[state];
  currentProjectSummary = typeof data.project_summary === 'string' ? data.project_summary : '';
  input.placeholder = state === 'planning'
    ? currentTask.plan ? 'Напишите, что изменить в плане…' : 'Опишите новую задачу…'
    : state === 'execution' ? 'Продолжите работу над задачей…'
      : state === 'validation' ? 'Добавьте данные перед проверкой…'
        : 'Задача завершена';
}

function renderAgentState(data, { renderChat = true, keepPosition = false, forcePersonalization = false } = {}) {
  if (!data || !data.memory) return false;
  activeChatId = String(data.active_chat_id || '');
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
      showNotice(data.input_rejected && !url.startsWith('/api/task/')
          ? 'Данные отклонены input policy. Не сохраняйте секреты и опасные команды.' : errorText);
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
      const error = data.input_rejected ? 'Сообщение отклонено input policy. Уберите секреты или запросы на выполнение опасных команд.' : 'Не удалось завершить запрос. Попробуйте ещё раз.';
      // Provider error bodies and rejected user text are never shown.
      if (data.memory) renderAgentState(data, { renderChat: false });
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

loadAgentState();
// Do not refresh on window focus: the refresh marks the UI busy and can swallow
// the first click on actions such as project deletion. Every mutation already
// returns the complete current Agent state.
