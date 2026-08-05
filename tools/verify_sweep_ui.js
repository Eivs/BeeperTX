// 验证前端 UI 逻辑:
//  - updateSweepUI 状态显示 (IDLE 不误显示, 进度 done/total)
//  - sweep 轮询门控: 仅 sweep 进行中轮询 /api/sweep/status
//  - sweep 期间暂停 /api/status, 退出后按需立即恢复
//  - Presets: 选中即加载, Save 覆盖 + 确认, New 新建
// 通过最小 DOM stub 在 node 中执行真实 config-panel.js / main.js
const fs = require('fs');
const path = require('path');

// ---- 最小 DOM stub ----
const elements = new Map();
class FakeEl {
  constructor(id) {
    this.id = id;
    this.textContent = '';
    this.style = { display: '' };
    this.classList = { _s: new Set(), add(c) { this._s.add(c); }, remove(c) { this._s.delete(c); }, toggle(c, on) { on ? this._s.add(c) : this._s.delete(c); }, contains(c) { return this._s.has(c); } };
    this.disabled = false;
    this.value = '';
    this.innerHTML = '';
    this.dataset = {};
    this._listeners = {};
    this.addEventListener = (ev, fn) => { this._listeners[ev] = fn; };
    this.appendChild = () => {};
    this.closest = () => this;  // 按钮自身即目标
  }
}

// btn-group stub: 支持动态重建 (rebuildBaudGroup)
class FakeGroup {
  constructor(field) {
    this.dataset = { field };
    this._buttons = [];
    this._listeners = {};
    this.addEventListener = (ev, fn) => { this._listeners[ev] = fn; };
    this.appendChild = (b) => { this._buttons.push(b); };
    this.setAttribute = () => {};
  }
  set innerHTML(v) { this._buttons = []; }  // rebuild 清空
  get innerHTML() { return ''; }
  querySelectorAll(sel) {
    if (sel.includes('button')) return this._buttons;
    return [];
  }
  querySelector(sel) {
    if (sel.includes('button')) return this._buttons.find(b => b.classList.contains('active')) || null;
    return null;
  }
}
const allGroups = [new FakeGroup('encoding'), new FakeGroup('phase'),
                   new FakeGroup('tone'), new FakeGroup('msgType'),
                   new FakeGroup('baudRate')];
const baudGroup = allGroups.find(g => g.dataset.field === 'baudRate');
const encGroup  = allGroups.find(g => g.dataset.field === 'encoding');

// 预置 encoding 组按钮 (模拟 HTML 静态按钮, initBtnGroups 会给它们绑定)
function mkBtn(val, active) {
  const b = new FakeEl('btn');
  b.dataset.value = val;
  b.textContent = val;
  if (active) b.classList.add('active');
  return b;
}
encGroup._buttons = [mkBtn('pocsag', true), mkBtn('flex')];

global.document = {
  getElementById(id) { if (!elements.has(id)) elements.set(id, new FakeEl(id)); return elements.get(id); },
  querySelectorAll(sel) { if (sel.includes('btn-group')) return allGroups; return []; },
  querySelector(sel) {
    if (sel.includes('data-field=')) {
      const m = sel.match(/data-field="([^"]+)"/);
      return allGroups.find(g => g.dataset.field === m[1]) || null;
    }
    return null;
  },
  createElement() { return new FakeEl('dyn'); },
  readyState: 'complete',
};

// localStorage stub
const store = {};
global.localStorage = {
  getItem: k => (k in store ? store[k] : null),
  setItem: (k, v) => { store[k] = String(v); },
  removeItem: k => { delete store[k]; },
};

// confirm/prompt stub (记录调用)
let confirmCalls = 0;
let promptCalls = 0;
global.confirm = () => { confirmCalls++; return true; };
global.prompt = () => { promptCalls++; return null; };

// sweep 状态回调捕获 (main.js 注册, 先占位)
let sweepActiveFlag = null;

let sweepStatusResp = { code: 0, running: false, mode: 0, current: 0, total: 0, done: 0 };
let setConfigCalls = 0;
let getLogResp = '';
let getStatusCalls = 0;

global.window = {
  Terminal: { log() {} },
  __setSweepActive: (a) => { sweepActiveFlag = a; },
  API: {
    getStatus: async () => { getStatusCalls++; return { code: 0, txCount: 5 }; },
    getLog: async () => getLogResp,
    wifiStatus: async () => ({ code: 0, ip: '1.2.3.4', state: 1 }),
    getConfig: async () => ({ code: 0, encoding: 'pocsag', phase: 'normal', tone: 'A', msgType: 'numeric', freqKHz: 152675, baudRate: 1200, address: 1000, txPower: 100, message: '' }),
    setConfig: async () => { setConfigCalls++; return { code: 0 }; },
    sweepStatus: async () => sweepStatusResp,
    sweepStop: async () => ({ code: 0 }),
  },
};

// ---- 载入真实代码 ----
const cfgSrc = fs.readFileSync(path.join(__dirname, '../data/js/config-panel.js'), 'utf8');
eval(cfgSrc);

let pass = 0, fail = 0;
function check(name, cond) { if (cond) { pass++; console.log('  PASS: ' + name); } else { fail++; console.log('  FAIL: ' + name); } }

const sleep = ms => new Promise(r => setTimeout(r, ms));

(async () => {
  const ConfigPanel = window.ConfigPanel;
  const el = id => document.getElementById(id);

  // 初始状态: 不轮询 sweep/status (页面加载后无 sweep)
  check('初始不轮询 sweep/status', ConfigPanel.isSweepPolling() === false);
  await sleep(50);  // 等 refreshPanel 完成

  // ===== 新增: Baud Rate 随编码联动 =====
  const baudVals = () => baudGroup._buttons.map(b => b.dataset.value).join(',');
  const baudActive = () => { const a = baudGroup.querySelector('button.active'); return a ? a.dataset.value : null; };
  check('初始 (pocsag): baud 选项 512,1200', baudVals() === '512,1200');
  check('初始 (pocsag): 默认 1200 激活', baudActive() === '1200');

  // 切到 flex → 选项 1600/2400/3200, 默认 1600
  const flexBtn = encGroup._buttons.find(b => b.dataset.value === 'flex');
  encGroup._listeners.click({ target: flexBtn }); // 原子提交 encoding+默认值 (100ms)
  await sleep(200);
  check('切 flex: baud 选项 1600,2400,3200', baudVals() === '1600,2400,3200');
  check('切 flex: 默认 1600 激活', baudActive() === '1600');
  check('切 flex: encoding 按钮选中', encGroup.querySelector('button.active').dataset.value === 'flex');

  // 切回 pocsag → 选项 512/1200, 默认 1200
  const pocBtn = encGroup._buttons.find(b => b.dataset.value === 'pocsag');
  encGroup._listeners.click({ target: pocBtn });
  await sleep(200);
  check('切回 pocsag: baud 选项 512,1200', baudVals() === '512,1200');
  check('切回 pocsag: 默认 1200 激活', baudActive() === '1200');

  // ===== 原有用例: updateSweepUI 显示 =====
  // 场景 1: IDLE (running=false) → 状态行隐藏, 不显示 Freq
  sweepStatusResp = { code: 0, running: false, mode: 0, current: 0, total: 0, done: 0 };
  el('sweep-status-line').style.display = 'block';
  el('sweep-mode-text').textContent = 'Freq';  // 残留旧值
  await ConfigPanel.updateSweepUI();
  check('IDLE 时状态行隐藏', el('sweep-status-line').style.display === 'none');
  check('IDLE 时 mode 文本被重置(非 Freq)', el('sweep-mode-text').textContent !== 'Freq');

  // 场景 2: 运行态 Addr → mode 文本 'Addr', 进度 done/total
  sweepStatusResp = { code: 0, running: true, mode: 1, current: 1200, total: 9999000, done: 200 };
  await ConfigPanel.updateSweepUI();
  check('运行态 Addr: mode=Addr', el('sweep-mode-text').textContent === 'Addr');
  check('运行态 Addr: current 显示 addr=', el('sweep-current-text').textContent.startsWith('addr='));
  check('运行态 Addr: 进度 done/total', el('sweep-count-text').textContent === '200/9999000');
  check('运行态 Addr: 状态行显示', el('sweep-status-line').style.display !== 'none');

  // 场景 2b: 运行态 Addr + 批次区间
  sweepStatusResp = { code: 0, running: true, mode: 1, current: 1200, total: 9999000, done: 200, batchStart: 990, batchEnd: 1005 };
  await ConfigPanel.updateSweepUI();
  check('运行态 Addr: 批次区间显示', el('sweep-current-text').textContent === 'addr=990-1005');
  check('运行态 Addr: 区间模式进度不变', el('sweep-count-text').textContent === '200/9999000');

  // 场景 2c: 批次区间缺失/单地址 → 回退 current
  sweepStatusResp = { code: 0, running: true, mode: 1, current: 1200, total: 9999000, done: 200, batchStart: 1200, batchEnd: 1200 };
  await ConfigPanel.updateSweepUI();
  check('运行态 Addr: 单地址批次显示', el('sweep-current-text').textContent === 'addr=1200');

  // 场景 3: 运行态 Freq (mode=2)
  sweepStatusResp = { code: 0, running: true, mode: 2, current: 152675, total: 1000, done: 42 };
  await ConfigPanel.updateSweepUI();
  check('运行态 Freq: mode=Freq', el('sweep-mode-text').textContent === 'Freq');
  check('运行态 Freq: current 显示 MHz', el('sweep-current-text').textContent.endsWith(' MHz'));
  check('运行态 Freq: 进度 done/total', el('sweep-count-text').textContent === '42/1000');

  // ===== 新增: sweep 轮询门控 =====
  // 归零沿状态 (IDLE 一轮使 sweepWasRunning=false)
  sweepStatusResp = { code: 0, running: false, mode: 0, current: 0, total: 0, done: 0 };
  await ConfigPanel.updateSweepUI();

  // 运行态 → 开启轮询 + 通知 main 暂停 status
  sweepActiveFlag = null;
  sweepStatusResp = { code: 0, running: true, mode: 1, current: 1, total: 10, done: 1 };
  await ConfigPanel.updateSweepUI();
  check('运行态开启 sweep 轮询', ConfigPanel.isSweepPolling() === true);
  check('运行态通知暂停 /api/status', sweepActiveFlag === true);

  // 停止沿 → 关闭轮询 + 通知恢复 status
  sweepActiveFlag = null;
  sweepStatusResp = { code: 0, running: false, mode: 0, current: 0, total: 0, done: 0 };
  await ConfigPanel.updateSweepUI();
  check('停止后关闭 sweep 轮询', ConfigPanel.isSweepPolling() === false);
  check('停止沿通知恢复 /api/status', sweepActiveFlag === false);

  // 手动 stop: 回到运行态 → 点 STOP → 关闭轮询 + 恢复 status
  sweepStatusResp = { code: 0, running: true, mode: 1, current: 1, total: 10, done: 1 };
  await ConfigPanel.updateSweepUI();
  sweepActiveFlag = null;
  await el('btn-sweep-toggle')._listeners.click();
  await sleep(20);  // 等 async handler 完成
  check('手动 STOP 后关闭 sweep 轮询', ConfigPanel.isSweepPolling() === false);
  check('手动 STOP 后恢复 /api/status', sweepActiveFlag === false);

  // ===== 新增: Preset 语义 =====
  store['beeper_tx_presets'] = JSON.stringify({ p1: { freqKHz: 152675, txPower: 90 } });
  ConfigPanel.refresh = async () => {};
  setConfigCalls = 0;
  el('sel-preset').value = 'p1';
  await el('sel-preset')._listeners.change();  // 选中即加载
  check('选中 preset 即触发加载 (setConfig)', setConfigCalls === 1);
  check('加载后下拉保持选中', el('sel-preset').value === 'p1');

  // Save: 未选中 → 提示名称 (prompt), 取消则不新建
  confirmCalls = 0;
  promptCalls = 0;
  el('sel-preset').value = '';
  await el('btn-preset-save')._listeners.click();
  check('Save 未选中: 弹出名称输入', promptCalls === 1);
  check('Save 取消: 不弹确认不新建', confirmCalls === 0 &&
        JSON.parse(store['beeper_tx_presets']).p2 === undefined);

  // Save: 选中 → 确认后覆盖
  confirmCalls = 0;
  el('sel-preset').value = 'p1';
  await el('btn-preset-save')._listeners.click();
  check('Save 覆盖: 弹出确认', confirmCalls === 1);
  check('Save 覆盖: 配置已更新', JSON.parse(store['beeper_tx_presets']).p1 !== undefined);

  // Save: 未选中 + 输入名称 → 新建 preset
  global.prompt = () => { promptCalls++; return 'p2'; };
  el('sel-preset').value = '';
  await el('btn-preset-save')._listeners.click();
  check('Save 新建: 创建新 preset', JSON.parse(store['beeper_tx_presets']).p2 !== undefined);
  check('Save 新建: 新项保持选中', el('sel-preset').value === 'p2');

  // ===== main.js: status 轮询门控 + 日志兜底 =====
  const mainSrc = fs.readFileSync(path.join(__dirname, '../data/js/main.js'), 'utf8');
  eval(mainSrc);  // init 立即 pollStatus 一次

  getStatusCalls = 0;
  window.__setSweepActive(true);
  await sleep(20);
  check('sweep 中暂停 /api/status', getStatusCalls === 0);

  window.__setSweepActive(false);
  await sleep(20);
  check('sweep 退出立即刷新一次 /api/status', getStatusCalls === 1);

  // 日志兜底: 看到 SWEEP START (其他设备启动) → 开启轮询
  ConfigPanel.setSweepPolling(false);
  getLogResp = 'boot ok\nSWEEP START mode=addr start=1000\n';
  await sleep(2200);  // pollLog 每 2s 触发
  check('日志兜底: SWEEP START 开启轮询', ConfigPanel.isSweepPolling() === true);

  console.log(`\n结果: ${pass} PASS, ${fail} FAIL`);
  process.exit(fail ? 1 : 0);
})();
