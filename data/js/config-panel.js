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

  // ---- 初始化入口 ----
  function init() {
    initBtnGroups();
    initInputs();
    populatePresetSelect();

    document.getElementById('btn-preset-save')?.addEventListener('click', handleSavePreset);
    document.getElementById('btn-preset-load')?.addEventListener('click', handleLoadPreset);

    // 初始加载
    refreshPanel();

    window.ConfigPanel = { refresh: refreshPanel };
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', init);
  } else {
    init();
  }

})();
