/**
 * config-panel.js — 配置面板管理
 *
 * - 初始化按钮组和输入框的值
 * - 绑定变更事件, 通过 API.setConfig() 发送到后端
 * - 预设保存/加载 (localStorage)
 */
(function() {
  'use strict';

  const PRESETS_KEY = 'beeper_tx_presets';

  let currentConfig = {};

  // ---- 按钮组初始化 ----
  function initBtnGroups() {
    document.querySelectorAll('.btn-group[data-field]').forEach(group => {
      const field = group.dataset.field;
      group.querySelectorAll('button[data-value]').forEach(btn => {
        btn.addEventListener('click', () => {
          // 更新 UI
          group.querySelectorAll('button').forEach(b => b.classList.remove('active'));
          btn.classList.add('active');
          // 发送到后端
          const val = btn.dataset.value;
          updateField(field, val);
        });
      });
    });
  }

  // ---- 输入框初始化 ----
  function initInputs() {
    const inputMap = {
      'inp-freq':     { field: 'freqKHz',   type: 'number' },
      'inp-power':    { field: 'txPower',   type: 'number' },
      'inp-addr':     { field: 'address',   type: 'number' }
    };

    Object.entries(inputMap).forEach(([id, cfg]) => {
      const el = document.getElementById(id);
      if (!el) return;
      el.addEventListener('change', () => {
        let val = cfg.type === 'number' ? Number(el.value) : el.value;
        updateField(cfg.field, val);
      });
    });

    // 编码切换 → 自动设置对应默认值
    const encGroup = document.querySelector('.btn-group[data-field="encoding"]');
    if (encGroup) {
      encGroup.addEventListener('click', async (e) => {
        const btn = e.target.closest('button[data-value]');
        if (!btn) return;
        const enc = btn.dataset.value;
        // 稍后等后端确认后再更新默认值
        setTimeout(() => applyEncodingDefaults(enc), 100);
      });
    }
  }

  /** 根据编码切换设置 Polar/Tone/Baud 默认值 */
  async function applyEncodingDefaults(enc) {
    const patch = {};
    if (enc === 'pocsag') {
      patch.phase    = 'normal';
      patch.tone     = 'A';
      patch.baudRate = 1200;
    } else if (enc === 'flex') {
      patch.phase    = 'inverted';
      patch.tone     = 'D';
      patch.baudRate = 1600;
    } else return;
    try {
      const resp = await window.API.setConfig(patch);
      if (!resp.code) {
        // 本地即时更新 UI (不需要等待 refreshPanel)
        setBtnGroup('phase',    patch.phase);
        setBtnGroup('tone',     patch.tone);
        setBtnGroup('baudRate', String(patch.baudRate));
        window.Terminal.log(`encoding=${enc}: applied defaults`, 'rx');
      }
    } catch(e) {
      window.Terminal.log(`encoding defaults failed: ${e.message}`, 'error');
    }
  }

  // ---- 字段更新 → API ----
  async function updateField(field, val) {
    const cfg = {};
    cfg[field] = val;
    try {
      const resp = await window.API.setConfig(cfg);
      // 成功: code 为 0 或不存在 (返回完整 config 对象)
      if (!resp.code) {
        window.Terminal.log(`config: ${field}=${val}`, 'rx');
      } else {
        window.Terminal.log(`config error: ${resp.detail || 'unknown'}`, 'error');
      }
    } catch(e) {
      window.Terminal.log(`config failed: ${e.message}`, 'error');
    }
  }

  // ---- 刷新面板 (从后端拉取) ----
  async function refreshPanel() {
    try {
      const cfg = await window.API.getConfig();
      if (!cfg || cfg.code) return;
      currentConfig = cfg;
      applyConfig(cfg);
    } catch(e) { /* ignore */ }
  }

  function applyConfig(cfg) {
    // 按钮组
    setBtnGroup('encoding',   cfg.encoding);
    setBtnGroup('phase',      cfg.phase);
    setBtnGroup('tone',       cfg.tone ? cfg.tone.toUpperCase() : 'A');
    setBtnGroup('msgType',    cfg.msgType);
    setBtnGroup('baudRate',   String(cfg.baudRate));

    // 输入框
    setInput('inp-freq',     cfg.freqKHz);
    setInput('inp-power',    cfg.txPower);
    setInput('inp-addr',     cfg.address);
  }

  function setBtnGroup(field, val) {
    const group = document.querySelector(`.btn-group[data-field="${field}"]`);
    if (!group) return;
    group.querySelectorAll('button').forEach(b => {
      b.classList.toggle('active', b.dataset.value === val);
    });
  }

  function setInput(id, val) {
    const el = document.getElementById(id);
    if (el) el.value = val;
  }

  // ---- 预设管理 ----
  function loadPresets() {
    try {
      const raw = localStorage.getItem(PRESETS_KEY);
      return raw ? JSON.parse(raw) : {};
    } catch(e) { return {}; }
  }

  function savePresets(presets) {
    localStorage.setItem(PRESETS_KEY, JSON.stringify(presets));
  }

  function populatePresetSelect() {
    const sel = document.getElementById('sel-preset');
    const presets = loadPresets();
    sel.innerHTML = '<option value="">-- saved presets --</option>';
    Object.keys(presets).forEach(name => {
      const opt = document.createElement('option');
      opt.value = name;
      opt.textContent = name;
      sel.appendChild(opt);
    });
  }

  async function handleSavePreset() {
    const name = prompt('Preset name:');
    if (!name) return;
    const cfg = await window.API.getConfig();
    const presets = loadPresets();
    presets[name] = cfg;
    savePresets(presets);
    populatePresetSelect();
    window.Terminal.log(`preset "${name}" saved`, 'rx');
  }

  async function handleLoadPreset() {
    const sel = document.getElementById('sel-preset');
    const name = sel.value;
    if (!name) return;
    const presets = loadPresets();
    const cfg = presets[name];
    if (!cfg) return;
    await window.API.setConfig(cfg);
    await refreshPanel();
    window.Terminal.log(`preset "${name}" loaded`, 'rx');
  }

  async function handleDelPreset() {
    const sel = document.getElementById('sel-preset');
    const name = sel.value;
    if (!name || !confirm('Delete preset "' + name + '"?')) return;
    const presets = loadPresets();
    delete presets[name];
    savePresets(presets);
    populatePresetSelect();
    window.Terminal.log(`preset "${name}" deleted`, 'rx');
  }

  // ---- Sweep 扫描控制 ----
  let sweepRunning = false;

  async function handleSweepAddr() {
    if (sweepRunning) return;
    if (!confirm('Start 追地址码 from current address?')) return;
    try {
      const resp = await window.API.sweepStart('addr');
      if (!resp.code) {
        setSweepRunning(true);
        window.Terminal.log('SWEEP addr started: ' + resp.start, 'info');
      } else {
        window.Terminal.log('SWEEP start failed: code=' + resp.code, 'error');
      }
    } catch(e) {
      window.Terminal.log('SWEEP error: ' + e.message, 'error');
    }
  }

  async function handleSweepFreq() {
    if (sweepRunning) return;
    if (!confirm('Start 追频率 from current frequency?\n步进 +25kHz，上限 300MHz')) return;
    try {
      const resp = await window.API.sweepStart('freq');
      if (!resp.code) {
        setSweepRunning(true);
        window.Terminal.log('SWEEP freq started: ' + resp.start + 'kHz', 'info');
      } else {
        window.Terminal.log('SWEEP start failed: code=' + resp.code, 'error');
      }
    } catch(e) {
      window.Terminal.log('SWEEP error: ' + e.message, 'error');
    }
  }

  async function handleSweepStop() {
    try {
      await window.API.sweepStop();
      window.Terminal.log('SWEEP stopped', 'warn');
    } catch(e) {
      window.Terminal.log('SWEEP stop error: ' + e.message, 'error');
    }
  }

  function setSweepRunning(running) {
    sweepRunning = running;
    const addrBtn = document.getElementById('btn-sweep-addr');
    const freqBtn = document.getElementById('btn-sweep-freq');
    const stopBtn = document.getElementById('btn-sweep-stop');
    const statusLine = document.getElementById('sweep-status-line');

    if (addrBtn) addrBtn.disabled = running;
    if (freqBtn) freqBtn.disabled = running;
    if (stopBtn) stopBtn.disabled = !running;
    if (statusLine) statusLine.style.display = running ? 'block' : 'none';
    if (!running) {
      const modeText = document.getElementById('sweep-mode-text');
      const curText  = document.getElementById('sweep-current-text');
      const cntText  = document.getElementById('sweep-count-text');
      if (modeText) modeText.textContent = '--';
      if (curText)  curText.textContent  = '--';
      if (cntText)  cntText.textContent  = '0';
    }
  }

  /** 由 main.js 定时轮询调用, 刷新扫描状态 UI */
  async function updateSweepUI() {
    try {
      const s = await window.API.sweepStatus();
      if (!s || s.code) return;

      const running = s.running === true || s.running === 'true';
      setSweepRunning(running);

      if (running) {
        const modeText = document.getElementById('sweep-mode-text');
        const curText  = document.getElementById('sweep-current-text');
        const cntText  = document.getElementById('sweep-count-text');

        if (modeText) {
          modeText.textContent = (s.mode === 1) ? '追地址码' : '追频率';
        }
        if (curText) {
          if (s.mode === 1) {
            // 追码: 显示 addr + freq
            curText.textContent = 'addr=' + s.current;
          } else {
            // 追频: s.current 是频率 kHz
            let freqMHz = (s.current / 1000).toFixed(3);
            curText.textContent = freqMHz + ' MHz';
          }
        }
        if (cntText) {
          cntText.textContent = s.totalSent;
        }
      }
    } catch(e) { /* ignore poll errors */ }
  }

  // ---- 初始化入口 ----
  function init() {
    initBtnGroups();
    initInputs();
    populatePresetSelect();

    document.getElementById('btn-preset-save')?.addEventListener('click', handleSavePreset);
    document.getElementById('btn-preset-load')?.addEventListener('click', handleLoadPreset);
    document.getElementById('btn-preset-del')?.addEventListener('click', handleDelPreset);

    // Sweep 按钮
    document.getElementById('btn-sweep-addr')?.addEventListener('click', handleSweepAddr);
    document.getElementById('btn-sweep-freq')?.addEventListener('click', handleSweepFreq);
    document.getElementById('btn-sweep-stop')?.addEventListener('click', handleSweepStop);

    // 初始加载
    refreshPanel();

    window.ConfigPanel = { refresh: refreshPanel, updateSweepUI: updateSweepUI };
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', init);
  } else {
    init();
  }

})();
