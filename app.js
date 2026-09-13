const form = document.querySelector('#chatForm');
const input = document.querySelector('#messageInput');
const sendButton = document.querySelector('#sendButton');
const chatLog = document.querySelector('#chatLog');
const strategyButtons = [...document.querySelectorAll('[data-strategy]')];
const strategyDescription = document.querySelector('#strategyDescription');
const rawMessages = document.querySelector('#rawMessages');
const longTermFacts = document.querySelector('#longTermFacts');
const branchCount = document.querySelector('#branchCount');
const branchBar = document.querySelector('#branchBar');
const branchHint = document.querySelector('#branchHint');
const branchList = document.querySelector('#branchList');
const contextTokens = document.querySelector('#contextTokens');
const contextCost = document.querySelector('#contextCost');
const totalTokens = document.querySelector('#totalTokens');
const totalCost = document.querySelector('#totalCost');

const strategyDescriptions = {
  1: 'Only the latest 10 messages are used. SQLite and summaries are disabled.',
  2: 'The latest 10 messages are combined with durable goals and important facts from SQLite.',
  3: 'Each checkpoint creates isolated branches. A branch cannot read its siblings.'
};

let activeStrategy = 1;
let chatPending = false;
let statePending = false;
let strategyPending = false;
let branchPending = false;

function updateControlState() {
  const controlsPending = chatPending || statePending || strategyPending || branchPending;
  sendButton.disabled = controlsPending;
  input.disabled = chatPending || strategyPending || branchPending;
  strategyButtons.forEach((button) => { button.disabled = controlsPending; });
  branchList.querySelectorAll('button').forEach((button) => { button.disabled = controlsPending; });
}

function formatTokenCount(value) {
  return Number(value || 0).toLocaleString();
}

function formatUsd(value) {
  return `$${Number(value || 0).toFixed(6)}`;
}

function scrollToLatest() {
  chatLog.scrollTop = chatLog.scrollHeight;
}

function addMessage(role, text = '', scroll = true) {
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
  if (scroll) scrollToLatest();
  return message;
}

function renderConversation(messages = []) {
  chatLog.replaceChildren();
  if (!messages.length) {
    const empty = document.createElement('div');
    empty.className = 'chat-empty';
    empty.textContent = 'This strategy has no messages yet.';
    chatLog.append(empty);
    return;
  }
  messages.forEach((message) => addMessage(message.role, message.content, false));
  scrollToLatest();
}

function renderBranches(memory = {}) {
  const branches = Array.isArray(memory.branches) ? memory.branches : [];
  branchBar.hidden = activeStrategy !== 3;
  branchCount.textContent = activeStrategy === 3 ? String(branches.length) : 'Disabled';
  branchList.replaceChildren();

  if (activeStrategy !== 3) return;
  branchHint.textContent = branches.length === 2
    ? 'Checkpoint detected. Switch between two isolated answers.'
    : 'A choice will appear when the agent detects a real dichotomy.';

  branches.forEach((branch) => {
    const button = document.createElement('button');
    button.type = 'button';
    button.textContent = branch.label;
    button.dataset.branchId = String(branch.id);
    button.setAttribute('aria-current', String(Boolean(branch.active)));
    button.addEventListener('click', () => switchBranch(branch.id));
    branchList.append(button);
  });
}

function renderAgentState(data, renderChat = false) {
  if (!data || typeof data.strategy !== 'number') return;
  activeStrategy = data.strategy;
  strategyButtons.forEach((button) => {
    button.setAttribute('aria-selected', String(Number(button.dataset.strategy) === activeStrategy));
  });
  strategyDescription.textContent = strategyDescriptions[activeStrategy];
  rawMessages.textContent = String(data.memory?.raw_messages || 0);
  longTermFacts.textContent = activeStrategy === 2
    ? String(data.memory?.long_term_facts || 0)
    : 'Disabled';
  renderBranches(data.memory);
  contextTokens.textContent = `${formatTokenCount(data.usage?.input_tokens)} tokens`;
  contextCost.textContent = formatUsd(data.usage?.cost_usd?.input);
  totalTokens.textContent = `${formatTokenCount(data.usage?.total_tokens)} tokens`;
  totalCost.textContent = formatUsd(data.usage?.cost_usd?.total);
  if (renderChat) renderConversation(data.conversation);
}

async function loadAgentState() {
  statePending = true;
  updateControlState();
  try {
    const response = await fetch('/api/state', { cache: 'no-store' });
    const data = await response.json();
    if (!response.ok) throw new Error(data.error || 'Could not read agent state');
    renderAgentState(data, true);
  } catch (error) {
    strategyDescription.textContent = `Server unavailable: ${error.message}`;
  } finally {
    statePending = false;
    updateControlState();
  }
}

async function switchStrategy(strategy) {
  if (strategy === activeStrategy || strategyPending) return;
  strategyPending = true;
  updateControlState();
  try {
    const response = await fetch('/api/strategy', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ strategy })
    });
    const data = await response.json();
    if (!response.ok) throw new Error(data.error || 'Could not switch strategy');
    renderAgentState(data, true);
  } catch (error) {
    strategyDescription.textContent = `Strategy switch failed: ${error.message}`;
  } finally {
    strategyPending = false;
    updateControlState();
    input.focus();
  }
}

async function switchBranch(branchId) {
  if (branchPending) return;
  branchPending = true;
  updateControlState();
  try {
    const response = await fetch('/api/branch', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ branch_id: branchId })
    });
    const data = await response.json();
    if (!response.ok) throw new Error(data.error || 'Could not switch branch');
    renderAgentState(data, true);
  } catch (error) {
    branchHint.textContent = `Branch switch failed: ${error.message}`;
  } finally {
    branchPending = false;
    updateControlState();
    input.focus();
  }
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
    renderAgentState(data, false);
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

strategyButtons.forEach((button) => {
  button.addEventListener('click', () => switchStrategy(Number(button.dataset.strategy)));
});

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
  if (!chatPending && !statePending && !strategyPending && !branchPending) loadAgentState();
});
