const form = document.querySelector('#chatForm');
const input = document.querySelector('#messageInput');
const sendButton = document.querySelector('#sendButton');
const chatLog = document.querySelector('#chatLog');
const compressionToggle = document.querySelector('#compressionToggle');
const compressionMode = document.querySelector('#compressionMode');
const rawTurns = document.querySelector('#rawTurns');
const summaryState = document.querySelector('#summaryState');
const totalTokens = document.querySelector('#totalTokens');
const summaryTokens = document.querySelector('#summaryTokens');

let compressionEnabled = false;
let chatPending = false;
let compressionRequestPending = false;
let stateRequestPending = false;

function updateControlState() {
  sendButton.disabled = chatPending || compressionRequestPending;
  input.disabled = chatPending;
  compressionToggle.disabled = chatPending || compressionRequestPending || stateRequestPending;
}

function formatTokenCount(value) {
  return Number(value || 0).toLocaleString();
}

function renderAgentState(data) {
  if (!data?.memory) return;
  compressionEnabled = Boolean(data.memory.compression_enabled);
  compressionToggle.setAttribute('aria-pressed', String(compressionEnabled));
  compressionMode.textContent = compressionEnabled ? 'On' : 'Off';
  rawTurns.textContent = String(data.memory.raw_turns || 0);
  summaryState.textContent = data.memory.summary_present
    ? (compressionEnabled ? 'Active' : 'Stored')
    : 'Empty';
  totalTokens.textContent = formatTokenCount(data.usage?.total_tokens);
  summaryTokens.textContent = formatTokenCount(data.usage?.summary?.total_tokens);
}

async function loadAgentState() {
  stateRequestPending = true;
  updateControlState();
  try {
    const response = await fetch('/api/compression', { cache: 'no-store' });
    const data = await response.json();
    if (!response.ok) throw new Error(data.error || 'Could not read agent state');
    renderAgentState(data);
  } catch (error) {
    compressionMode.textContent = 'Unavailable';
    compressionToggle.title = error.message;
  } finally {
    stateRequestPending = false;
    updateControlState();
  }
}

async function toggleCompression() {
  compressionRequestPending = true;
  updateControlState();
  try {
    const response = await fetch('/api/compression/toggle', { method: 'POST' });
    const data = await response.json();
    if (!response.ok) throw new Error(data.error || 'Could not change compression mode');
    renderAgentState(data);
    compressionToggle.title = '';
  } catch (error) {
    compressionMode.textContent = 'Error — retry';
    compressionToggle.title = error.message;
  } finally {
    compressionRequestPending = false;
    updateControlState();
    input.focus();
  }
}

function scrollToLatest() {
  chatLog.scrollTop = chatLog.scrollHeight;
}

function addMessage(role, text = '') {
  chatLog.querySelector('.chat-empty')?.remove();
  const message = document.createElement('article');
  message.className = `chat-message ${role}`;

  const label = document.createElement('p');
  label.className = 'message-role';
  label.textContent = role === 'user' ? 'You' : 'Agent';

  const content = document.createElement('div');
  content.className = 'message-content';
  if (role === 'loading') {
    content.classList.add('loading');
    content.innerHTML = '<div class="loader" aria-label="Loading"><i></i><i></i><i></i></div>';
  } else {
    content.textContent = text;
  }

  message.append(label, content);
  chatLog.append(message);
  scrollToLatest();
  return message;
}

async function askAgent(question) {
  addMessage('user', question);
  const pendingMessage = addMessage('loading');
  chatPending = true;
  updateControlState();
  input.value = '';
  input.style.height = 'auto';

  try {
    const response = await fetch('/api/chat', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ message: question })
    });
    const data = await response.json();
    renderAgentState(data);
    if (!response.ok) throw new Error(data.error || 'Local server error');

    pendingMessage.className = 'chat-message assistant';
    pendingMessage.querySelector('.message-role').textContent = data.model || 'Agent';
    const content = pendingMessage.querySelector('.message-content');
    content.classList.remove('loading');
    content.textContent = data.answer;
    scrollToLatest();
  } catch (error) {
    pendingMessage.className = 'chat-message error';
    pendingMessage.querySelector('.message-role').textContent = 'Error';
    const content = pendingMessage.querySelector('.message-content');
    content.classList.remove('loading');
    content.textContent = error.message;
  } finally {
    chatPending = false;
    updateControlState();
    input.focus();
  }
}

form.addEventListener('submit', (event) => {
  event.preventDefault();
  const question = input.value.trim();
  if (question && !sendButton.disabled) askAgent(question);
});

compressionToggle.addEventListener('click', toggleCompression);

input.addEventListener('keydown', (event) => {
  if (event.key === 'Enter' && !event.shiftKey) {
    event.preventDefault();
    form.requestSubmit();
  }
});

input.addEventListener('input', () => {
  input.style.height = 'auto';
  input.style.height = `${Math.min(input.scrollHeight, 110)}px`;
});

loadAgentState();

window.addEventListener('focus', () => {
  if (!sendButton.disabled && !compressionToggle.disabled) loadAgentState();
});
