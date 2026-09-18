const form = document.querySelector('#chatForm');
const input = document.querySelector('#messageInput');
const sendButton = document.querySelector('#sendButton');
const chatLog = document.querySelector('#chatLog');
const memoryDescription = document.querySelector('#memoryDescription');
const chatNotice = document.querySelector('#chatNotice');
const rawMessages = document.querySelector('#rawMessages');
const rawMessageLimit = document.querySelector('#rawMessageLimit');
const longTermFacts = document.querySelector('#longTermFacts');
const summaryStatus = document.querySelector('#summaryStatus');
const pendingSummaryMessages = document.querySelector('#pendingSummaryMessages');
const contextTokens = document.querySelector('#contextTokens');
const contextCost = document.querySelector('#contextCost');
const summaryTokens = document.querySelector('#summaryTokens');
const summaryCost = document.querySelector('#summaryCost');
const totalTokens = document.querySelector('#totalTokens');
const totalCost = document.querySelector('#totalCost');

let chatPending = false;
let statePending = false;

function updateControlState() {
  sendButton.disabled = chatPending || statePending;
  input.disabled = chatPending;
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
  const safeRole = ['user', 'assistant', 'loading', 'error'].includes(role) ? role : 'assistant';
  message.className = `chat-message ${safeRole}`;

  const label = document.createElement('p');
  label.className = 'message-role';
  label.textContent = safeRole === 'user' ? 'You' : safeRole === 'error' ? 'Error' : 'Agent';

  const content = document.createElement('div');
  content.className = 'message-content';
  if (safeRole === 'loading') {
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
  if (!Array.isArray(messages) || !messages.length) {
    const empty = document.createElement('div');
    empty.className = 'chat-empty';
    empty.textContent = 'Start the conversation with a question.';
    chatLog.append(empty);
    return;
  }
  messages.forEach((message) => addMessage(message.role, message.content, false));
  scrollToLatest();
}

function renderNotice(data = {}) {
  const notices = [];
  if (data.input_suspicious) {
    notices.push('Potential instruction override detected. Agent rules and memory controls remain unchanged.');
  }
  if (Array.isArray(data.warnings)) {
    data.warnings.forEach((warning) => {
      if (typeof warning === 'string' && warning) notices.push(warning);
    });
  }
  chatNotice.textContent = notices.join(' ');
  chatNotice.hidden = notices.length === 0;
}

function renderAgentState(data, renderChat = false) {
  if (!data || !data.memory) return;
  const memory = data.memory;
  rawMessages.textContent = String(memory.raw_messages || 0);
  rawMessageLimit.textContent = String(memory.raw_message_limit || 5);
  longTermFacts.textContent = String(memory.long_term_facts || 0);
  summaryStatus.textContent = memory.summary_present ? 'Active' : 'Empty';
  pendingSummaryMessages.textContent = String(memory.pending_summary_messages || 0);
  memoryDescription.textContent = `The latest ${memory.raw_message_limit || 5} messages stay verbatim. Older messages are summarized every ${memory.summary_every_requests || 5} requests. SQLite facts persist between server restarts.`;
  contextTokens.textContent = `${formatTokenCount(data.usage?.input_tokens)} tokens`;
  contextCost.textContent = formatUsd(data.usage?.cost_usd?.input);
  summaryTokens.textContent = `${formatTokenCount(data.usage?.summary?.total_tokens)} tokens`;
  summaryCost.textContent = formatUsd(data.usage?.summary?.cost_usd?.total);
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
    if (!response.ok) throw new Error('State request failed');
    renderAgentState(data, true);
  } catch {
    memoryDescription.textContent = 'Server unavailable. Start the local C++ server and refresh this page.';
  } finally {
    statePending = false;
    updateControlState();
  }
}

async function askAgent(question) {
  // Do not put submitted text into the visible history before C++ accepts it.
  const pendingMessage = addMessage('loading');
  chatPending = true;
  updateControlState();
  input.value = '';
  input.style.height = 'auto';
  renderNotice();

  try {
    const response = await fetch('/api/chat', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ message: question })
    });
    const data = await response.json();
    renderAgentState(data, false);
    renderNotice(data);
    if (!response.ok) {
      pendingMessage.remove();
      const error = data.input_rejected
        ? 'Message rejected by input policy. Remove secrets or requests to execute destructive commands.'
        : 'Could not complete the request. Please try again.';
      addMessage('error', error);
      return;
    }

    // Only the server's accepted, authoritative conversation is displayed.
    renderAgentState(data, true);
  } catch {
    pendingMessage.remove();
    addMessage('error', 'Could not reach the local server. Your message was not added to the displayed history.');
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
  if (!chatPending && !statePending) loadAgentState();
});
