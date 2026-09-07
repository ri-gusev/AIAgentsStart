const form = document.querySelector('#chatForm');
const input = document.querySelector('#messageInput');
const sendButton = document.querySelector('#sendButton');
const chatLog = document.querySelector('#chatLog');

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
  sendButton.disabled = true;
  input.disabled = true;
  input.value = '';
  input.style.height = 'auto';

  try {
    const response = await fetch('/api/chat', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ message: question })
    });
    const data = await response.json();
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
    sendButton.disabled = false;
    input.disabled = false;
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
