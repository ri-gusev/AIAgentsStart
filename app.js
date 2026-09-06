const form = document.querySelector('#chatForm');
const input = document.querySelector('#messageInput');
const messages = document.querySelector('#messages');
const sendButton = document.querySelector('#sendButton');
const clearButton = document.querySelector('#clearButton');

function addMessage(text, role) {
  const article = document.createElement('article');
  article.className = `message ${role}`;

  if (role === 'assistant') {
    const avatar = document.createElement('div');
    avatar.className = 'avatar';
    avatar.textContent = 'AI';
    article.append(avatar);
  }

  const bubble = document.createElement('div');
  bubble.className = 'bubble';
  bubble.textContent = text;
  article.append(bubble);
  messages.append(article);
  messages.scrollTop = messages.scrollHeight;
  return article;
}

function addTyping() {
  const article = document.createElement('article');
  article.className = 'message assistant';
  article.innerHTML = '<div class="avatar">AI</div><div class="bubble"><div class="typing"><span></span><span></span><span></span></div></div>';
  messages.append(article);
  messages.scrollTop = messages.scrollHeight;
  return article;
}

async function sendMessage(text) {
  addMessage(text, 'user');
  const typing = addTyping();
  sendButton.disabled = true;
  input.disabled = true;

  try {
    const response = await fetch('/api/chat', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ message: text })
    });
    const data = await response.json();
    typing.remove();
    addMessage(response.ok ? data.answer : `Ошибка: ${data.error || 'Не удалось получить ответ'}`, 'assistant');
  } catch {
    typing.remove();
    addMessage('Не удалось связаться с локальным C++ сервером.', 'assistant');
  } finally {
    sendButton.disabled = false;
    input.disabled = false;
    input.focus();
  }
}

form.addEventListener('submit', (event) => {
  event.preventDefault();
  const text = input.value.trim();
  if (!text || sendButton.disabled) return;
  input.value = '';
  input.style.height = 'auto';
  sendMessage(text);
});

input.addEventListener('keydown', (event) => {
  if (event.key === 'Enter' && !event.shiftKey) {
    event.preventDefault();
    form.requestSubmit();
  }
});

input.addEventListener('input', () => {
  input.style.height = 'auto';
  input.style.height = `${Math.min(input.scrollHeight, 140)}px`;
});

document.querySelectorAll('.suggestions button').forEach((button) => {
  button.addEventListener('click', () => sendMessage(button.textContent));
});

clearButton.addEventListener('click', () => {
  messages.querySelectorAll('.message:not(.welcome)').forEach((message) => message.remove());
  input.focus();
});
