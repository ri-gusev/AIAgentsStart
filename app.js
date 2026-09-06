const form = document.querySelector('#chatForm');
const input = document.querySelector('#messageInput');
const sendButton = document.querySelector('#sendButton');
const cards = [...document.querySelectorAll('.answer-card')];

function setLoading() {
  cards.forEach((card) => {
    const content = card.querySelector('.answer-content');
    content.classList.add('loading');
    content.innerHTML = '<div class="loader" aria-label="Загрузка"><i></i><i></i><i></i></div>';
  });
}

function showResults(answers) {
  cards.forEach((card) => {
    const temperature = Number(card.dataset.temperature);
    const result = answers.find((item) => item.temperature === temperature);
    const content = card.querySelector('.answer-content');
    content.classList.remove('loading');
    content.textContent = result?.answer || `Ошибка: ${result?.error || 'ответ не получен'}`;
    content.scrollTop = 0;
  });
}

async function compareAnswers(question) {
  setLoading();
  sendButton.disabled = true;
  input.disabled = true;

  try {
    const response = await fetch('/api/chat', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ message: question })
    });
    const data = await response.json();
    if (!response.ok) throw new Error(data.error || 'Ошибка локального сервера');
    showResults(data.answers || []);
  } catch (error) {
    showResults([0, 1, 2].map((temperature) => ({ temperature, error: error.message })));
  } finally {
    sendButton.disabled = false;
    input.disabled = false;
    input.focus();
  }
}

form.addEventListener('submit', (event) => {
  event.preventDefault();
  const question = input.value.trim();
  if (question && !sendButton.disabled) compareAnswers(question);
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
