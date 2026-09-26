const reminderForm = document.querySelector('#reminderForm');
const reminderText = document.querySelector('#reminderText');
const reminderRunAt = document.querySelector('#reminderRunAt');
const createReminderButton = document.querySelector('#createReminderButton');
const reminderList = document.querySelector('#reminderList');
const reminderNotifications = document.querySelector('#reminderNotifications');
const reminderError = document.querySelector('#reminderError');
const reminderHint = document.querySelector('#reminderHint');
const reminderConnection = document.querySelector('#reminderConnection');
const reminderPermission = document.querySelector('#reminderNotificationPermission');
const enableReminderNotifications = document.querySelector('#enableReminderNotifications');
const reminderMoscowClock = new Intl.DateTimeFormat('en-CA', {
  timeZone: 'Europe/Moscow', year: 'numeric', month: '2-digit', day: '2-digit',
  hour: '2-digit', minute: '2-digit', second: '2-digit', hourCycle: 'h23'
});
const reminderMoscowDisplay = new Intl.DateTimeFormat('ru-RU', {
  timeZone: 'Europe/Moscow', year: 'numeric', month: '2-digit', day: '2-digit',
  hour: '2-digit', minute: '2-digit', second: '2-digit', hourCycle: 'h23'
});
let reminderPending = false;
let reminderStreamConnected = false;
let reminderStateInitialized = false;
let reminderReconnectDelay = 1000;
let reminderSocket;
let reminderClosing = false;
const seenReminderNotifications = new Set();
let reminderPermissionAsked = false;
try { reminderPermissionAsked = localStorage.getItem('reminder.permission.requested') === '1'; } catch {}

function setReminderOffset(minutes) {
  const moment = new Date(Date.now() + minutes * 60000);
  const parts = Object.fromEntries(reminderMoscowClock.formatToParts(moment).map((part) => [part.type, part.value]));
  reminderRunAt.value = parts.year + '-' + parts.month + '-' + parts.day + 'T' +
    parts.hour + ':' + parts.minute + ':' + parts.second;
}

function parseReminderMoscowTime(value) {
  // datetime-local has no timezone: interpret the entered clock time as MSK,
  // never as the browser/OS timezone. SQLite and the transport remain UTC.
  return new Date(value + '+03:00');
}

function updateReminderControls() {
  const toolAvailable = currentMcpStatus === 'Connected' && currentMcpTools.some((tool) => tool.name === 'create_reminder');
  createReminderButton.disabled = reminderPending || mcpPending || !toolAvailable || !reminderStreamConnected || !reminderStateInitialized;
  createReminderButton.textContent = reminderPending ? 'Создание…' : 'Create Reminder';
  reminderHint.textContent = currentMcpStatus !== 'Connected' ? 'Нажмите Connect в панели MCP.' :
    !toolAvailable ? 'Перезапустите MCP-server и нажмите Refresh tools: нужен create_reminder.' :
    !reminderStreamConnected || !reminderStateInitialized ? 'Ожидание соединения для уведомлений…' : 'Напоминание сохранится после Create Reminder.';
}

function updateReminderPermission() {
  const supported = 'Notification' in window && window.isSecureContext;
  const permission = supported ? Notification.permission : 'unsupported';
  enableReminderNotifications.hidden = permission !== 'default';
  enableReminderNotifications.disabled = reminderPermissionAsked;
  reminderPermission.textContent = permission === 'granted' ? 'Системные уведомления разрешены.' :
    permission === 'denied' ? 'Разрешите уведомления в настройках сайта. Внутри UI они появятся.' :
    permission === 'unsupported' ? 'Системные уведомления недоступны в этом браузере. Используйте локальный desktop-браузер.' :
    reminderPermissionAsked ? 'Запрос уже показан. Разрешение можно изменить в настройках сайта.' :
    'Разрешение будет запрошено один раз при создании напоминания.';
}

async function requestReminderNotificationPermission() {
  if (!('Notification' in window) || !window.isSecureContext || Notification.permission !== 'default' || reminderPermissionAsked) {
    updateReminderPermission(); return;
  }
  reminderPermissionAsked = true;
  try { localStorage.setItem('reminder.permission.requested', '1'); } catch {}
  updateReminderPermission();
  try { await Notification.requestPermission(); } catch {}
  updateReminderPermission();
}

function displayReminderTime(value) {
  const moment = new Date(value);
  return Number.isNaN(moment.getTime()) ? String(value || '') : reminderMoscowDisplay.format(moment) + ' МСК';
}

function renderReminderState(data) {
  const error = typeof data.error === 'string' ? data.error : '';
  reminderError.textContent = error;
  reminderError.hidden = !error;
  reminderList.replaceChildren();
  const reminders = Array.isArray(data.reminders) ? data.reminders : [];
  if (!reminders.length) reminderList.textContent = 'Пока нет напоминаний.';
  reminders.forEach((reminder) => {
    const item = document.createElement('article');
    item.className = 'reminder-item';
    item.dataset.status = String(reminder.status);
    const text = document.createElement('strong');
    text.textContent = String(reminder.text);
    const details = document.createElement('small');
    details.textContent = displayReminderTime(reminder.run_at) + ' · ' + String(reminder.status);
    item.append(text, details);
    reminderList.append(item);
  });
  reminderNotifications.replaceChildren();
  const notifications = Array.isArray(data.notifications) ? data.notifications : [];
  if (!notifications.length) reminderNotifications.textContent = 'Пока нет уведомлений.';
  [...notifications].reverse().forEach((notification) => {
    const item = document.createElement('article');
    item.className = 'reminder-item';
    const text = document.createElement('strong');
    text.textContent = '🔔 ' + String(notification.text);
    const time = document.createElement('small');
    time.textContent = displayReminderTime(notification.triggered_at);
    item.append(text, time);
    reminderNotifications.append(item);
  });
}

function applyReminderEvent(data) {
  renderReminderState(data);
  const notifications = Array.isArray(data.notifications) ? data.notifications : [];
  notifications.forEach((notification) => {
    const id = String(notification.id);
    if (seenReminderNotifications.has(id)) return;
    seenReminderNotifications.add(id);
    // First snapshot restores history silently; reconnects deliver missed events.
    if (!reminderStateInitialized || !('Notification' in window) || Notification.permission !== 'granted') return;
    try {
      const toast = new Notification('Reminder', {
        body: String(notification.text), tag: 'reminder-' + String(notification.reminder_id)
      });
      toast.onclick = () => { window.focus(); };
      toast.onerror = () => { reminderPermission.textContent = 'Браузер не смог показать системное уведомление. Оно доступно внутри UI.'; };
    } catch { reminderPermission.textContent = 'Системное уведомление недоступно. Оно доступно внутри UI.'; }
  });
  reminderStateInitialized = true;
  updateReminderControls();
}

function connectReminderEvents() {
  if (reminderClosing) return;
  reminderSocket = new WebSocket((location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + '/api/reminders/events');
  reminderSocket.onopen = () => {
    reminderStreamConnected = true;
    reminderReconnectDelay = 1000;
    reminderConnection.textContent = 'Connected';
    updateReminderControls();
  };
  reminderSocket.onmessage = (event) => {
    try { applyReminderEvent(JSON.parse(event.data)); } catch {
      reminderError.textContent = 'Получено некорректное уведомление.'; reminderError.hidden = false;
    }
  };
  reminderSocket.onclose = () => {
    reminderStreamConnected = false;
    reminderConnection.textContent = 'Переподключение…';
    updateReminderControls();
    if (!reminderClosing) setTimeout(connectReminderEvents, reminderReconnectDelay);
    reminderReconnectDelay = Math.min(10000, reminderReconnectDelay * 2);
  };
  reminderSocket.onerror = () => { reminderSocket.close(); };
}

reminderForm.addEventListener('submit', async (event) => {
  event.preventDefault();
  if (createReminderButton.disabled) return;
  const moment = parseReminderMoscowTime(reminderRunAt.value);
  if (Number.isNaN(moment.getTime()) || moment.getTime() <= Date.now()) {
    reminderError.textContent = 'Выберите время в будущем (МСК).'; reminderError.hidden = false; return;
  }
  // Called directly from the user's click; registration is not delayed by the prompt.
  requestReminderNotificationPermission();
  reminderPending = true;
  mcpPending = true;
  updateMcpControls();
  try {
    const response = await fetch('/api/reminders', {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ text: reminderText.value.trim(), run_at: moment.toISOString() })
    });
    const data = await response.json();
    if (data.mcp) renderMcpState(data.mcp);
    renderReminderState(data);
    if (response.ok) { reminderText.value = ''; setReminderOffset(1); }
  } catch {
    reminderError.textContent = 'Backend недоступен. Напоминание не подтверждено.';
    reminderError.hidden = false;
  } finally {
    reminderPending = false;
    mcpPending = false;
    updateMcpControls();
  }
});
document.querySelector('#reminderPlusOne').addEventListener('click', () => setReminderOffset(1));
document.querySelector('#reminderPlusTwo').addEventListener('click', () => setReminderOffset(2));
enableReminderNotifications.addEventListener('click', requestReminderNotificationPermission);
window.addEventListener('pagehide', () => { reminderClosing = true; reminderSocket?.close(); });
window.addEventListener('pageshow', (event) => { if (event.persisted) { reminderClosing = false; connectReminderEvents(); } });
setReminderOffset(1);
updateReminderPermission();
updateReminderControls();
connectReminderEvents();
