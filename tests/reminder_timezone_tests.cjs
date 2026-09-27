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
const makeElement = (tag = 'div') => ({
  tagName: tag, value: '', textContent: '', disabled: false, hidden: false, dataset: {}, children: [],
  listeners: {},
  attributes: {},
  addEventListener(type, callback) { this.listeners[type] = callback; },
  setAttribute(name, value) { this.attributes[name] = value; },
  getAttribute(name) { return this.attributes[name] || null; },
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
  document: {
    querySelector(id) {
      assert.ok(!['#reminderList', '#reminderNotifications'].includes(id), 'Removed UI must not be accessed');
      return element(id);
    },
    createElement(tag) {
      assert.notEqual(tag, 'pre', 'Reminder list must not show JSON');
      return makeElement(tag);
    },
  },
  window: { isSecureContext: true, addEventListener() {}, Notification: FakeNotification },
  Notification: FakeNotification,
  setTimeout(callback) { timers.push(callback); return timers.length; }, clearTimeout() {},
  localStorage: { getItem() { return null; } },
  currentMcpStatus: 'Disconnected', currentMcpTools: [], mcpPending: false,
  location: { protocol: 'http:', host: '127.0.0.1:8080' },
  WebSocket: class { close() {} },
});
vm.runInContext(fs.readFileSync(path.join(__dirname, '../reminders.js'), 'utf8'), context);
element('#mcpCallLog').hidden = true;
element('#mcpCallsToggle').setAttribute('aria-expanded', 'false');
context.mcpCallsToggle = element('#mcpCallsToggle');
context.mcpCallLog = element('#mcpCallLog');
const appSource = fs.readFileSync(path.join(__dirname, '../app.js'), 'utf8');
const toggleStart = appSource.indexOf("mcpCallsToggle.addEventListener('click'");
assert.ok(toggleStart >= 0);
vm.runInContext(appSource.slice(toggleStart, appSource.indexOf('\n});', toggleStart) + 4), context);
assert.ok(!appSource.includes('renderMcpCalls'), 'Raw MCP call JSON must not be rendered');

assert.equal(element('#reminderRunAt').value, '2026-09-26T18:01:00');
vm.runInContext('setReminderOffset(2)', context);
assert.equal(element('#reminderRunAt').value, '2026-09-26T18:02:00');
assert.equal(vm.runInContext("parseReminderMoscowTime('2026-09-26T18:00:00').toISOString()", context), '2026-09-26T15:00:00.000Z');
assert.equal(vm.runInContext("parseReminderMoscowTime('2026-09-27T00:30').toISOString()", context), '2026-09-26T21:30:00.000Z');
assert.ok(vm.runInContext("Number.isNaN(parseReminderMoscowTime('').getTime())", context));
vm.runInContext(`applyReminderEvent({type:'snapshot', reminders:[
  {id:1,text:'Pending',run_at:'2026-09-26T15:01:00Z',status:'pending'},
  {id:2,text:'Old completed',run_at:'2026-09-26T14:00:00Z',status:'triggered'}
], notifications:[{id:2,reminder_id:2,text:'Old completed',triggered_at:'2026-09-26T14:00:00Z'}]})`, context);
assert.equal(element('#mcpCallLog').children.length, 1, 'Show pending items only');
assert.equal(element('#mcpCallLog').children[0].children[0].children[0].textContent, 'Pending');
assert.match(element('#mcpCallLog').children[0].children[0].children[1].textContent, /26\.09\.2026.*18:01:00 МСК/);
assert.equal(element('#mcpCallLog').children[0].children[1].textContent, 'Удалить');
assert.equal(element('#mcpCallsToggle').textContent, 'Напоминания (1)');
assert.equal(element('#mcpCallLog').hidden, true, 'Snapshot must not open the list');
element('#mcpCallsToggle').listeners.click();
assert.equal(element('#mcpCallLog').hidden, false);
assert.equal(element('#mcpCallsToggle').getAttribute('aria-expanded'), 'true');
element('#mcpCallsToggle').listeners.click();
assert.equal(element('#mcpCallLog').hidden, true);
assert.equal(delivered.length, 0, 'History must not produce notifications');
const event = {type:'triggered', reminders:[], notifications:[{id:1,reminder_id:1,text:'Pending',triggered_at:'2026-09-26T15:01:00Z'}]};
context.testEvent = event;
vm.runInContext('applyReminderEvent(testEvent); applyReminderEvent(testEvent)', context);
assert.equal(delivered.length, 1, 'Transient event must show exactly one browser notification');
assert.equal(delivered[0].title, 'Reminder');
assert.equal(delivered[0].options.body, 'Pending');
assert.equal(delivered[0].options.tag, 'reminder-1');
assert.equal(element('#mcpCallLog').children.length, 0, 'Completed reminder must disappear');
assert.equal(element('#mcpCallsToggle').textContent, 'Напоминания (0)');
assert.equal(element('#mcpCallLog').hidden, true, 'Notification must not open the list');
assert.equal(timers.length, 0, 'There must be no UI toast/expiry timer');
assert.equal(vm.runInContext('typeof deleteReminder', context), 'function');
assert.equal(vm.runInContext('typeof showReminderNotifications', context), 'undefined');

FakeNotification.permission = 'denied';
vm.runInContext("updateReminderPermission(); applyReminderEvent({type:'triggered', notifications:[{id:3,text:'Denied'}]})", context);
assert.equal(delivered.length, 1);
assert.doesNotMatch(element('#reminderNotificationPermission').textContent, /внутри UI|Внутри UI/);
FakeNotification.permission = 'granted';

const html = fs.readFileSync(path.join(__dirname, '../index.html'), 'utf8');
assert.doesNotMatch(html, /id="(?:reminderList|reminderNotifications|reminderToolFallback)"|Запланированные напоминания/);
assert.ok(html.includes('id="reminderForm"') && html.includes('id="enableReminderNotifications"'));
assert.match(html, /id="mcpCallsToggle"[^>]*aria-expanded="false"/);
assert.match(html, /id="mcpCallLog"[^>]*hidden/);
assert.ok(!html.includes('Recent calls'));
module.exports = (async () => {
  const requests = [];
  context.currentMcpStatus = 'Connected';
  context.currentMcpTools = [{name:'create_reminder'}];
  context.updateMcpControls = () => vm.runInContext('updateReminderControls()', context);
  const mcpStates = [];
  context.renderMcpState = (data) => mcpStates.push(data);
  vm.runInContext('reminderSocket.onopen(); applyReminderEvent({type:"snapshot",reminders:[]})', context);
  assert.equal(element('#createReminderButton').disabled, false);
  element('#reminderText').value = '  Browser-only reminder  ';
  element('#reminderRunAt').value = '2026-09-26T18:02:00';
  context.fetch = async (url, options) => {
    requests.push({url, body:JSON.parse(options.body)});
    assert.equal(element('#createReminderButton').disabled, true, 'Creation must lock controls');
    return {ok:true, json:async () => ({mcp:{status:'Connected'},error:''})};
  };
  const submit = element('#reminderForm').listeners.submit;
  await submit({preventDefault() {}});
  assert.deepEqual(requests, [{url:'/api/reminders',body:{text:'Browser-only reminder',run_at:'2026-09-26T15:02:00.000Z'}}]);
  assert.equal(mcpStates.length, 1, 'MCP connection state is updated after creation');
  assert.equal(element('#reminderText').value, '');
  assert.equal(element('#reminderCreateStatus').hidden, false);
  assert.equal(element('#reminderCreateStatus').textContent, 'Напоминание создано.');
  assert.equal(element('#createReminderButton').disabled, false);

  element('#reminderRunAt').value = '2000-01-01T00:00:00';
  await submit({preventDefault() {}});
  assert.equal(requests.length, 1, 'Past date must not be sent');
  assert.match(element('#reminderError').textContent, /будущем.*МСК/);

  element('#reminderRunAt').value = '2026-09-26T18:02:00';
  context.fetch = async () => { throw new Error('Offline'); };
  await submit({preventDefault() {}});
  assert.match(element('#reminderError').textContent, /Backend недоступен/);
  assert.equal(element('#reminderCreateStatus').hidden, true);
  assert.equal(element('#createReminderButton').disabled, false, 'Controls recover after network error');

  context.currentMcpStatus = 'Disconnected';
  vm.runInContext("applyReminderEvent({type:'triggered',notifications:[{id:4,reminder_id:4,text:'After MCP disconnect'}]})", context);
  assert.equal(delivered.length, 2, 'Scheduler notifications work independently of MCP connection');
  assert.equal(element('#createReminderButton').disabled, true);
  assert.equal(element('#mcpCallLog').children.length, 0);

  context.pendingItems = [
    {id:42,text:'Delete me',run_at:'2026-09-26T15:02:00Z',status:'pending'},
    {id:43,text:'Keep me',run_at:'2026-09-26T15:03:00Z',status:'pending'},
  ];
  vm.runInContext('applyReminderEvent({type:"snapshot",reminders:pendingItems})', context);
  const deleteRequests = [];
  context.fetch = async (url, options) => {
    deleteRequests.push({url,body:JSON.parse(options.body)});
    return {ok:true,json:async () => ({reminders:[],error:''})};
  };
  await element('#mcpCallLog').children[0].children[1].listeners.click();
  assert.deepEqual(deleteRequests, [{url:'/api/reminders/delete',body:{id:'42'}}]);
  assert.equal(element('#mcpCallLog').children.length, 1);
  assert.equal(element('#mcpCallLog').children[0].children[1].dataset.reminderId, '43');

  context.fetch = async () => ({ok:false,json:async () => ({error:'Reminder not found'})});
  await element('#mcpCallLog').children[0].children[1].listeners.click();
  assert.equal(element('#reminderListError').textContent, 'Reminder not found');
  assert.equal(element('#mcpCallLog').children[0].children[1].disabled, false);

  let finishDelete;
  let raceRequests = 0;
  context.fetch = async () => { raceRequests++; return new Promise((resolve) => { finishDelete = resolve; }); };
  const oldButton = element('#mcpCallLog').children[0].children[1];
  const deleting = oldButton.listeners.click();
  assert.equal(oldButton.disabled, true);
  await oldButton.listeners.click();
  assert.equal(raceRequests, 1, 'Repeated deletion must not send another request');
  vm.runInContext('applyReminderEvent({type:"update",reminders:[{id:44,text:"Newest",run_at:"2026-09-26T15:04:00Z",status:"pending"}]})', context);
  finishDelete({ok:true,json:async () => ({reminders:context.pendingItems,error:''})});
  await deleting;
  assert.equal(element('#mcpCallLog').children.length, 1);
  assert.equal(element('#mcpCallLog').children[0].children[1].dataset.reminderId, '44', 'Late response must not restore removed items');

  context.fetch = async () => { throw new Error('Offline'); };
  await element('#mcpCallLog').children[0].children[1].listeners.click();
  assert.equal(element('#mcpCallLog').children[0].children[1].disabled, false);
  assert.match(element('#reminderListError').textContent, /Проверьте backend/);
  context.currentMcpStatus = 'Connected';
  context.currentMcpTools = ['create_reminder', 'get_upcoming_reminders', 'summarize_reminders', 'build_reminder_view'].map((name) => ({name}));
  vm.runInContext('updateReminderOverviewControls()', context);
  const overviewButton = element('#reminderOverviewButton');
  assert.equal(overviewButton.disabled, false);
  const beforeList = element('#mcpCallLog').children;
  let finishOverview;
  context.fetch = (url) => {
    assert.equal(url, '/api/reminders/overview');
    return new Promise((resolve) => { finishOverview = resolve; });
  };
  const overviewRequest = overviewButton.listeners.click();
  assert.equal(overviewButton.disabled, true);
  assert.equal(overviewButton.textContent, 'Получение сводки…');
  assert.equal(element('#reminderOverviewResult').getAttribute('aria-busy'), 'true');
  await overviewButton.listeners.click();
  finishOverview({ok:true,json:async () => ({success:true,steps:['private'],result:{
    title:'Ближайшие планы',count:1,items:[{id:44,title:'<img src=x onerror=alert(1)>',date:'27.09.2026',time:'16:00'}]
  }})});
  await overviewRequest;
  assert.equal(overviewButton.disabled, false);
  const overview = element('#reminderOverviewResult');
  assert.equal(overview.hidden, false);
  assert.equal(overview.children[1].textContent, 'У вас 1 напоминание на ближайшие 24 часа');
  assert.equal(overview.children[2].children[0].textContent, '<img src=x onerror=alert(1)>');
  assert.equal(overview.children[2].children[1].textContent, '27.09.2026 · 16:00 МСК');
  assert.equal(element('#mcpCallLog').children, beforeList, 'Overview must not replace the management list');
  context.fetch = async () => ({ok:true,json:async () => ({success:true,result:{title:'Ближайшие планы',count:0,items:[]}})});
  await overviewButton.listeners.click();
  assert.equal(overview.children[1].textContent, 'На ближайшие 24 часа напоминаний нет.');
  for (const response of [
    {ok:false,json:async () => ({success:false,failed_step:'private',error:'internal JSON'})},
    {ok:true,json:async () => ({success:true,result:{count:2,items:[]}})}
  ]) {
    context.fetch = async () => response;
    await overviewButton.listeners.click();
    assert.equal(element('#reminderOverviewError').hidden, false);
    assert.ok(!element('#reminderOverviewError').textContent.includes('private'));
    assert.equal(overview.hidden, true);
    assert.equal(overviewButton.disabled, false);
  }
  context.fetch = async () => { throw new Error('Offline'); };
  await overviewButton.listeners.click();
  assert.equal(element('#reminderOverviewError').hidden, false);
  context.currentMcpStatus = 'Disconnected';
  vm.runInContext('updateReminderOverviewControls()', context);
  assert.equal(overviewButton.disabled, true);
  // Run the actual MCP renderer: the orchestrator is the second entry inside
  // Available tools, never a new SDK tool or a generic tools/call form.
  for (const name of ['mcpTools', 'mcpStatus', 'mcpServerName', 'mcpServerUrl', 'mcpError', 'mcpToolsToggle', 'reminderToolPanel', 'reminderOverviewPanel']) {
    context[name] = element('#' + name);
  }
  context.reminderToolHome = makeElement();
  context.reminderOverviewHome = makeElement();
  context.reminderPipelineTools = ['get_upcoming_reminders', 'summarize_reminders', 'build_reminder_view'];
  context.mcpArgumentSummary = () => '()';
  context.mcpTools.querySelectorAll = () => context.mcpTools.children;
  const renderStart = appSource.indexOf('function renderMcpState(');
  vm.runInContext(appSource.slice(renderStart, appSource.indexOf('async function runMcpTool', renderStart)), context);
  const tools = ['create_reminder', ...context.reminderPipelineTools].map((name) => ({name}));
  context.renderMcpState({status:'Connected', tools});
  assert.equal(element('#mcpToolsToggle').textContent, 'Available tools (2)');
  assert.deepEqual(context.mcpTools.children.map((item) => item.dataset.toolName), ['create_reminder', 'reminder_overview']);
  assert.equal(context.mcpTools.children[0].children[0].children[0].textContent, 'Create Reminder');
  const workflow = context.mcpTools.children[1];
  assert.equal(workflow.children[0].children[0].textContent, 'Reminder Overview');
  assert.ok(workflow.children.includes(context.reminderOverviewPanel));
  assert.ok(!workflow.children.some((item) => item.tagName === 'form'), 'Orchestrator must not use generic MCP call form');
  workflow.open = true;
  context.renderMcpState({status:'Connected', tools});
  assert.equal(context.mcpTools.children[1].open, true, 'Refresh must preserve expansion');
  context.renderMcpState({status:'Disconnected', tools:[]});
  assert.equal(context.reminderOverviewPanel.hidden, true);
  assert.equal(element('#mcpToolsToggle').textContent, 'Available tools (0)');
  context.renderMcpState({status:'Connected', tools:[{name:'create_reminder'}]});
  vm.runInContext('updateReminderOverviewControls()', context);
  assert.equal(element('#mcpToolsToggle').textContent, 'Available tools (2)', 'An old MCP server must not hide Overview');
  assert.equal(context.mcpTools.children[1].dataset.toolName, 'reminder_overview');
  assert.equal(context.reminderOverviewPanel.hidden, false);
  assert.equal(overviewButton.disabled, true, 'Missing pipeline tools must disable execution');
  assert.match(element('#reminderOverviewHint').textContent, /Перезапустите MCP-server/);
  return 'PASS: reminder creation/deletion/notifications plus overview loading, success, MSK, empty/error/network states and safe text rendering';
})();
module.exports.then(console.log);
