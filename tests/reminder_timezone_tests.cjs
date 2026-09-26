// Exercise the actual UI script without a browser, network or real data.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const fixedNow = Date.parse('2026-09-26T15:00:00Z');
class ExplicitTimezoneDate extends Date {
  constructor(value) {
    if (typeof value === 'string' && !/(Z|[+-]\d{2}:\d{2})$/.test(value)) {
      throw new Error('Ambiguous date parsed using the host timezone');
    }
    super(value);
  }
  static now() { return fixedNow; }
}
// Host-local getters must never be used by Moscow clock helpers.
for (const name of ['getFullYear', 'getMonth', 'getDate', 'getHours', 'getMinutes', 'getSeconds']) {
  ExplicitTimezoneDate.prototype[name] = () => { throw new Error('Host-local clock getter used: ' + name); };
}

const elements = new Map();
const makeElement = () => ({
  value: '', textContent: '', disabled: false, hidden: false, dataset: {}, children: [],
  addEventListener() {}, setAttribute() {},
  replaceChildren() { this.children = []; this.textContent = ''; },
  append(...items) { this.children.push(...items); },
  querySelectorAll() {
    return this.children.flatMap((item) => [item, ...item.querySelectorAll()]).filter((item) => item.dataset.reminderId);
  },
});
const element = (id) => {
  if (!elements.has(id)) elements.set(id, makeElement());
  return elements.get(id);
};
const delivered = [];
class FakeNotification {
  static permission = 'granted';
  constructor(title, options) { delivered.push({title, options}); }
}
const timers = [];
const context = vm.createContext({
  Date: ExplicitTimezoneDate, Intl, Set, Object,
  document: { querySelector: element, createElement: makeElement },
  window: { isSecureContext: true, addEventListener() {}, Notification: FakeNotification },
  Notification: FakeNotification,
  setTimeout(callback) { timers.push(callback); return timers.length; }, clearTimeout() {},
  localStorage: { getItem() { return null; } },
  currentMcpStatus: 'Disconnected', currentMcpTools: [], mcpPending: false,
  location: { protocol: 'http:', host: '127.0.0.1:8080' },
  WebSocket: class { close() {} },
});
vm.runInContext(fs.readFileSync(path.join(__dirname, '../reminders.js'), 'utf8'), context);

assert.equal(element('#reminderRunAt').value, '2026-09-26T18:01:00');
vm.runInContext('setReminderOffset(2)', context);
assert.equal(element('#reminderRunAt').value, '2026-09-26T18:02:00');
assert.equal(vm.runInContext("parseReminderMoscowTime('2026-09-26T18:00:00').toISOString()", context), '2026-09-26T15:00:00.000Z');
assert.equal(vm.runInContext("parseReminderMoscowTime('2026-09-27T00:30').toISOString()", context), '2026-09-26T21:30:00.000Z');
assert.ok(vm.runInContext("Number.isNaN(parseReminderMoscowTime('').getTime())", context));
assert.match(vm.runInContext("displayReminderTime('2026-09-26T15:00:00Z')", context), /26\.09\.2026.*18:00:00 МСК/);
assert.match(vm.runInContext("displayReminderTime('2026-09-26T21:30:00Z')", context), /27\.09\.2026.*00:30:00 МСК/);
vm.runInContext(`applyReminderEvent({type:'snapshot', reminders:[
  {id:1,text:'Pending',run_at:'2026-09-26T15:01:00Z',status:'pending'},
  {id:2,text:'Old completed',run_at:'2026-09-26T14:00:00Z',status:'triggered'}
], notifications:[{id:2,reminder_id:2,text:'Old completed',triggered_at:'2026-09-26T14:00:00Z'}]})`, context);
assert.equal(element('#reminderList').children.length, 1);
assert.equal(element('#reminderList').children[0].children[2].textContent, 'Удалить');
assert.equal(delivered.length, 0, 'History must not produce notifications');
const event = {type:'triggered', reminders:[], notifications:[{id:1,reminder_id:1,text:'Pending',triggered_at:'2026-09-26T15:01:00Z'}]};
context.testEvent = event;
vm.runInContext('applyReminderEvent(testEvent); applyReminderEvent(testEvent)', context);
assert.equal(element('#reminderList').children.length, 0);
assert.equal(element('#reminderNotifications').children.length, 1);
assert.equal(delivered.length, 1, 'Transient event must show exactly one browser notification');
timers.at(-1)();
assert.equal(element('#reminderNotifications').children.length, 0, 'UI toast must expire, not become history');
module.exports = (async () => {
  const requests = [];
  context.fetch = async (url, options) => {
    requests.push({url, body:JSON.parse(options.body)});
    return {ok:true, json:async () => ({reminders:[],notifications:[],error:''})};
  };
  context.testDeleteButton = makeElement();
  await vm.runInContext('deleteReminder(42, testDeleteButton)', context);
  assert.deepEqual(requests, [{url:'/api/reminders/delete',body:{id:'42'}}]);
  assert.equal(context.testDeleteButton.disabled, false);
  context.fetch = async () => ({ok:false,json:async () => ({error:'Reminder not found'})});
  await vm.runInContext('deleteReminder(42, testDeleteButton)', context);
  assert.equal(element('#reminderError').textContent, 'Reminder not found');
  assert.equal(context.testDeleteButton.disabled, false, 'Delete control must recover after a rejected request');
  return 'PASS: Moscow time, pending-only UI, delete request/error recovery, transient notification expiry and no duplicate browser notifications';
})();
module.exports.then(console.log);
