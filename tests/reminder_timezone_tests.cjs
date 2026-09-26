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
const element = (id) => {
  if (!elements.has(id)) elements.set(id, {
    value: '', textContent: '', disabled: false, hidden: false,
    addEventListener() {}, replaceChildren() {}, append() {}
  });
  return elements.get(id);
};
const context = vm.createContext({
  Date: ExplicitTimezoneDate, Intl, Set, Object,
  document: { querySelector: element },
  window: { isSecureContext: true, addEventListener() {} },
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
module.exports = 'PASS: UI Moscow quick offsets, input parsing, midnight rollover and display do not use the host timezone';
console.log(module.exports);
