// 验证前端 updateSweepUI 修复 (c5): IDLE 态不误显示 Freq, 进度用 done/total
// 通过最小 DOM stub 在 node 中执行真实 updateSweepUI 代码
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
    this._listeners = {};
    this.addEventListener = (ev, fn) => { this._listeners[ev] = fn; };
  }
}
global.document = {
  getElementById(id) { if (!elements.has(id)) elements.set(id, new FakeEl(id)); return elements.get(id); },
  querySelectorAll(sel) { return []; },
  querySelector(sel) { return null; },
  readyState: 'complete',
};
global.window = {
  Terminal: { log() {} },
  API: { sweepStatus: async () => ({ code: 0, running: false, mode: 0, current: 0, total: 0, done: 0 }) },
};

// ---- 载入真实代码 ----
const src = fs.readFileSync(path.join(__dirname, '../data/js/config-panel.js'), 'utf8');
eval(src);

let pass = 0, fail = 0;
function check(name, cond) { if (cond) { pass++; console.log('  PASS: ' + name); } else { fail++; console.log('  FAIL: ' + name); } }

(async () => {
  const ConfigPanel = window.ConfigPanel;

  // 场景 1: IDLE (running=false) → 状态行隐藏, 不显示 Freq
  window.API.sweepStatus = async () => ({ code: 0, running: false, mode: 0, current: 0, total: 0, done: 0 });
  document.getElementById('sweep-status-line').style.display = 'block';
  document.getElementById('sweep-mode-text').textContent = 'Freq';  // 残留旧值
  await ConfigPanel.updateSweepUI();
  check('IDLE 时状态行隐藏', document.getElementById('sweep-status-line').style.display === 'none');
  check('IDLE 时 mode 文本被重置(非 Freq)', document.getElementById('sweep-mode-text').textContent !== 'Freq');

  // 场景 2: 运行态 Addr → mode 文本 'Addr', 进度 done/total
  window.API.sweepStatus = async () => ({ code: 0, running: true, mode: 1, current: 1200, total: 9999000, done: 200 });
  await ConfigPanel.updateSweepUI();
  check('运行态 Addr: mode=Addr', document.getElementById('sweep-mode-text').textContent === 'Addr');
  check('运行态 Addr: current 显示 addr=', document.getElementById('sweep-current-text').textContent.startsWith('addr='));
  check('运行态 Addr: 进度 done/total', document.getElementById('sweep-count-text').textContent === '200/9999000');
  check('运行态 Addr: 状态行显示', document.getElementById('sweep-status-line').style.display !== 'none');

  // 场景 3: 运行态 Freq (mode=2) → mode 'Freq', 进度 done/total
  window.API.sweepStatus = async () => ({ code: 0, running: true, mode: 2, current: 152675, total: 1000, done: 42 });
  await ConfigPanel.updateSweepUI();
  check('运行态 Freq: mode=Freq', document.getElementById('sweep-mode-text').textContent === 'Freq');
  check('运行态 Freq: current 显示 MHz', document.getElementById('sweep-current-text').textContent.endsWith(' MHz'));
  check('运行态 Freq: 进度 done/total', document.getElementById('sweep-count-text').textContent === '42/1000');

  console.log(`\n结果: ${pass} PASS, ${fail} FAIL`);
  process.exit(fail ? 1 : 0);
})();
