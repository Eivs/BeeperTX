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

  // 码率按编码区分 (与后端 isBaudRateValid 一致):
  // POCSAG 512/1200 (默认 1200), FLEX 1600/2400/3200 (默认 1600)
  const BAUD_OPTIONS = { pocsag: [512, 1200], flex: [1600, 2400, 3200] };
  const BAUD_DEFAULT = { pocsag: 1200, flex: 1600 };

  let currentConfig = {};

  // ---- 按钮组初始化 ----
  function initBtnGroups() {
    document.querySelectorAll('.btn-group[data-field]').forEach(group => {
      const field = group.dataset.field;
      if (field === 'encoding') return;  // encoding 由 applyEncodingDefaults 原子提交 (含默认值)
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
        // 频率输入为 MHz (UI 显示单位), 提交时转 kHz (后端存储单位)
        if (cfg.field === 'freqKHz') val = Math.round(val * 1000);
        updateField(cfg.field, val);
      });
    });

    // 编码切换 → 立即视觉反馈 + 稍后原子提交 (encoding+默认值, 一次请求)
    const encGroup = document.querySelector('.btn-group[data-field="encoding"]');
    if (encGroup) {
      encGroup.addEventListener('click', async (e) => {
        const btn = e.target.closest('button[data-value]');
        if (!btn) return;
        const enc = btn.dataset.value;
        // 立即更新选中态 (视觉反馈)
        encGroup.querySelectorAll('button').forEach(b => b.classList.remove('active'));
        btn.classList.add('active');
        // 稍后等后端确认后再更新默认值 (含 encoding 一起原子提交)
        setTimeout(() => applyEncodingDefaults(enc), 100);
      });
    }
  }

  /** 按编码显示/隐藏字段: FLEX 无极性 (强制直发) 且无 Tone 概念 (POCSAG 功能码专属) */
  function updateEncodingUI(enc) {
    const isFlex = (enc === 'flex');
    const phaseGroup = document.getElementById('group-phase');
    if (phaseGroup) phaseGroup.style.display = isFlex ? 'none' : '';
    const toneGroup = document.getElementById('group-tone');
    if (toneGroup) toneGroup.style.display = isFlex ? 'none' : '';
  }

  /** 控件锁定态 (sweep 中 true): 动态重建的按钮需继承, 否则 10s 周期
   *  refresh 重建 Baud 组时锁定被冲掉 (sweep 中选项偶尔变可选) */
  let controlsLocked = false;

  /** 按编码重建 Baud Rate 按钮组 (POCSAG: 512/1200, FLEX: 1600/2400/3200) */
  function rebuildBaudGroup(enc) {
    const group = document.querySelector('.btn-group[data-field="baudRate"]');
    if (!group) return;
    const opts = BAUD_OPTIONS[enc] || BAUD_OPTIONS.pocsag;
    // 当前选中值若仍在选项内则保留, 否则交给调用方设默认
    const activeBtn = group.querySelector('button.active');
    const curVal = activeBtn ? activeBtn.dataset.value : null;
    const keep = opts.some(v => String(v) === curVal) ? curVal : null;

    group.innerHTML = '';
    opts.forEach(v => {
      const b = document.createElement('button');
      b.className = 'btn btn-sm';
      b.dataset.value = String(v);
      b.textContent = String(v);
      if (String(v) === keep) b.classList.add('active');
      b.disabled = controlsLocked;  // 继承锁定态 (sweep 中 refresh 重建后仍禁用)
      b.addEventListener('click', () => {
        group.querySelectorAll('button').forEach(x => x.classList.remove('active'));
        b.classList.add('active');
        updateField('baudRate', b.dataset.value);
      });
      group.appendChild(b);
    });
  }

  /** 根据编码切换设置 Polar/Baud 默认值 (与 encoding 原子提交) */
  async function applyEncodingDefaults(enc) {
    const patch = { encoding: enc };
    if (enc === 'pocsag') {
      // POCSAG: phase 恢复正相默认; tone 保留用户选择 (FLEX 期间未提交)
      patch.phase    = 'normal';
      patch.baudRate = BAUD_DEFAULT.pocsag;
    } else if (enc === 'flex') {
      // FLEX: 无极性概念, phase 由后端强制直发; tone 不适用 (保留值, 切回恢复)
      patch.baudRate = BAUD_DEFAULT.flex;
    } else return;
    try {
      const resp = await window.API.setConfig(patch);
      if (!resp.code) {
        // 本地即时更新 UI (不需要等待 refreshPanel)
        setBtnGroup('encoding', enc);                  // 确认后端已接受
        rebuildBaudGroup(enc);                         // 切换码率选项
        updateEncodingUI(enc);                         // FLEX: 隐藏 Phase + Tone
        if (enc === 'flex') setBtnGroup('phase', 'inverted');  // 隐藏但保持正确状态
        setBtnGroup('baudRate', String(patch.baudRate));  // 选中默认码率
        window.Terminal.log(`encoding=${enc}: applied defaults`, 'rx');
      } else {
        window.Terminal.log(`encoding=${enc} rejected: ${resp.detail || ''}`, 'error');
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
      rebuildBaudGroup(cfg.encoding);  // 选项随当前编码
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
    updateEncodingUI(cfg.encoding);  // FLEX: 隐藏 Phase + Tone

    // 输入框
    setInput('inp-freq',  (cfg.freqKHz / 1000).toFixed(3));  // kHz → MHz 显示
    setInput('inp-power', cfg.txPower);
    setInput('inp-addr',  cfg.address);
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

  /** 重建下拉; keepSel 存在时保持该项选中 */
  function populatePresetSelect(keepSel) {
    const sel = document.getElementById('sel-preset');
    const presets = loadPresets();
    sel.innerHTML = '<option value="">-- saved presets --</option>';
    Object.keys(presets).forEach(name => {
      const opt = document.createElement('option');
      opt.value = name;
      opt.textContent = name;
      sel.appendChild(opt);
    });
    if (keepSel && presets[keepSel]) sel.value = keepSel;
  }

  /** 从当前表单收集配置 (存 preset 用, 不依赖后端往返) */
  function collectFormConfig() {
    const cfg = {};
    ['encoding','phase','tone','msgType','baudRate'].forEach(f => {
      const group = document.querySelector(`.btn-group[data-field="${f}"]`);
      const active = group && group.querySelector('button.active');
      if (active) {
        cfg[f] = (f === 'baudRate') ? Number(active.dataset.value) : active.dataset.value;
      }
    });
    const inputMap = { 'inp-freq': 'freqKHz', 'inp-power': 'txPower', 'inp-addr': 'address' };
    Object.entries(inputMap).forEach(([id, field]) => {
      const el = document.getElementById(id);
      if (el && el.value !== '') {
        let v = Number(el.value);
        // preset 内部与后端一致存 kHz: 频率输入为 MHz, 保存时转换
        if (field === 'freqKHz') v = Math.round(v * 1000);
        cfg[field] = v;
      }
    });
    const msg = document.getElementById('inp-message');
    if (msg && msg.value) cfg.message = msg.value;
    return cfg;
  }

  /**
   * Save: 已加载 preset (下拉有选中) → 覆盖 (确认);
   * 未加载 (下拉空) → 用当前表单新建 (prompt 名称)。
   */
  async function handleSavePreset() {
    const sel = document.getElementById('sel-preset');
    const name = sel.value;

    // 未加载任何 preset → 新建
    if (!name) {
      const newName = prompt('New preset name:');
      if (!newName) return;
      const presets = loadPresets();
      if (presets[newName] && !confirm('preset "' + newName + '" already exists, overwrite?')) return;
      presets[newName] = collectFormConfig();
      savePresets(presets);
      populatePresetSelect(newName);
      window.Terminal.log(`preset "${newName}" created`, 'rx');
      return;
    }

    // 已加载 preset → 覆盖 (确认)
    const presets = loadPresets();
    if (!presets[name]) {
      window.Terminal.log(`preset "${name}" does not exist, select empty to create`, 'warn');
      return;
    }
    if (!confirm('Overwrite preset "' + name + '"?')) return;
    presets[name] = collectFormConfig();
    savePresets(presets);
    populatePresetSelect(name);
    window.Terminal.log(`preset "${name}" saved`, 'rx');
  }

  /** 选中预设即加载 (sel-preset change) */
  async function handleLoadPreset() {
    const sel = document.getElementById('sel-preset');
    const name = sel.value;
    if (!name) return;
    const presets = loadPresets();
    const cfg = presets[name];
    if (!cfg) return;
    await window.API.setConfig(cfg);
    await refreshPanel();
    populatePresetSelect(name);
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
  let sweepMode = 'addr';  // 当前选中的模式
  let sweepPolling = false;   // 是否轮询 /api/sweep/status (仅 sweep 进行中)
  let sweepWasRunning = false;  // 上一轮轮询的 running 状态 (检测停止沿)

  function getSweepMode() {
    const addrBtn = document.getElementById('btn-sweep-addr');
    return (addrBtn && addrBtn.classList.contains('active')) ? 'addr' : 'freq';
  }

  function initSweepControls() {
    // 模式按钮组: 二选一互斥
    const addrBtn = document.getElementById('btn-sweep-addr');
    const freqBtn = document.getElementById('btn-sweep-freq');
    if (addrBtn) {
      addrBtn.addEventListener('click', () => {
        addrBtn.classList.add('active');
        if (freqBtn) freqBtn.classList.remove('active');
      });
    }
    if (freqBtn) {
      freqBtn.addEventListener('click', () => {
        freqBtn.classList.add('active');
        if (addrBtn) addrBtn.classList.remove('active');
      });
    }

    // START / STOP 切换按钮
    const toggleBtn = document.getElementById('btn-sweep-toggle');
    if (toggleBtn) {
      toggleBtn.addEventListener('click', () => {
        if (sweepRunning) {
          handleSweepStop();
        } else {
          handleSweepStart();
        }
      });
    }
  }

  async function handleSweepStart() {
    if (sweepRunning) return;
    const mode = getSweepMode();
    const label = (mode === 'addr') ? 'Address Sweep' : 'Frequency Sweep';
    if (!confirm('Start ' + label + '?')) return;
    try {
      const resp = await window.API.sweepStart(mode);
      if (!resp.code) {
        sweepPolling = true;  // 开始轮询 sweep/status
        if (window.__setSweepActive) window.__setSweepActive(true);  // 暂停 status 轮询
        setSweepRunningUI(true);
        window.Terminal.log('SWEEP ' + mode + ' started: ' + resp.start, 'info');
      } else {
        window.Terminal.log('SWEEP start failed: code=' + resp.code, 'error');
        // 设备可能在运行中 (ERR_BUSY, 其他页面/串口启动): 以设备端状态同步 UI
        await updateSweepUI();
      }
    } catch(e) {
      window.Terminal.log('SWEEP error: ' + e.message, 'error');
    }
  }

  /**
   * 停止扫描。发射期间网络可能抖动导致 stop 请求丢失, 重试至多 3 次,
   * 每次以设备端 sweep/status 为准确认; 结束后无论结果如何都以设备端
   * 真实状态恢复 UI (停止成功 → 全部控件恢复; 失败 → 保持 STOP 可重试)。
   */
  async function handleSweepStop() {
    let stopped = false;
    for (let attempt = 1; attempt <= 3 && !stopped; attempt++) {
      try {
        await window.API.sweepStop();
        const s = await window.API.sweepStatus();
        // 状态读取失败 (s 为空) 视为未停止, 继续重试
        stopped = !!(s && s.running !== true && s.running !== 'true');
      } catch(e) {
        window.Terminal.log('SWEEP stop error: ' + e.message, 'error');
      }
      if (!stopped && attempt < 3) await new Promise(r => setTimeout(r, 400));
    }
    if (stopped) {
      window.Terminal.log('SWEEP stopped', 'warn');
    } else {
      window.Terminal.log('SWEEP stop failed: device still running', 'error');
    }
    // 强制走停止沿处理 (关闭 sweep 轮询 + 恢复 status 轮询 + 恢复控件)
    sweepWasRunning = true;
    await updateSweepUI();
  }

  /** 启用/禁用配置面板控件 (扫中禁用) */
  function setConfigEnabled(enabled) {
    controlsLocked = !enabled;  // 供 rebuildBaudGroup 重建时继承
    // 按钮组
    document.querySelectorAll('#config-panel .btn-group[data-field] button').forEach(b => {
      b.disabled = !enabled;
    });
    // 输入框 (跳过 sweep 区域内的)
    ['inp-freq','inp-power','inp-addr'].forEach(id => {
      const el = document.getElementById(id);
      if (el) el.disabled = !enabled;
    });
    // Send 按钮
    const sendBtn = document.getElementById('btn-send');
    if (sendBtn) sendBtn.disabled = !enabled;
    // 预设按钮 + 下拉 (扫中禁用, 避免加载配置与发射冲突)
    ['btn-preset-save','btn-preset-del'].forEach(id => {
      const el = document.getElementById(id);
      if (el) el.disabled = !enabled;
    });
    const selPreset = document.getElementById('sel-preset');
    if (selPreset) selPreset.disabled = !enabled;
  }

  function setSweepRunningUI(running) {
    sweepRunning = running;
    const toggleBtn = document.getElementById('btn-sweep-toggle');
    const modeBtns = document.querySelectorAll('#btn-sweep-addr, #btn-sweep-freq');
    const statusLine = document.getElementById('sweep-status-line');

    // 模式按钮: 扫中禁用
    modeBtns.forEach(b => b.disabled = running);

    // 切换按钮
    if (toggleBtn) {
      toggleBtn.textContent = running ? 'STOP' : 'START';
      toggleBtn.classList.toggle('btn-primary', !running);
      toggleBtn.classList.toggle('btn-error', running);
    }

    // 状态行
    if (statusLine) statusLine.style.display = running ? 'block' : 'none';

    // 配置面板控件
    setConfigEnabled(!running);

    if (!running) {
      const modeText = document.getElementById('sweep-mode-text');
      const curText  = document.getElementById('sweep-current-text');
      if (modeText) modeText.textContent = '--';
      if (curText)  curText.textContent  = '--';
    }
  }

  /** 由 main.js 定时轮询调用, 刷新扫描状态 UI (仅 sweep 进行中; 停止沿自动关闭) */
  let sweepUiSyncing = false;
  async function updateSweepUI() {
    if (sweepUiSyncing) return;  // 并发保护: 1.5s 轮询与 stop/start 收尾同步可能重叠
    sweepUiSyncing = true;
    try {
      const s = await window.API.sweepStatus();
      if (!s || s.code) return;

      const running = s.running === true || s.running === 'true';
      // 轮询开关 + status 轮询暂停/恢复, 由 running 沿驱动:
      // 运行 → 保持轮询 + 暂停 status; 停止 → 关闭轮询 + 恢复 status。
      if (running) {
        sweepPolling = true;
        if (!sweepWasRunning && window.__setSweepActive) {
          window.__setSweepActive(true);
        }
      } else {
        sweepPolling = false;
        if (sweepWasRunning && window.__setSweepActive) {
          window.__setSweepActive(false);  // 恢复 status 轮询 (立即刷新)
        }
      }
      sweepWasRunning = running;

      setSweepRunningUI(running);

      if (!running) return;  // 非运行态: setSweepRunningUI 已隐藏状态行

      // 仅运行态按 mode 显示 (mode: 1=Addr, 2=Freq)
      const modeText = document.getElementById('sweep-mode-text');
      const curText  = document.getElementById('sweep-current-text');

      if (modeText) {
        modeText.textContent = (s.mode === 1) ? 'Addr' : 'Freq';
      }
      if (curText) {
        if (s.mode === 1) {
          // 追码: 显示当前批次区间 (如 990-1005), 避免"下一批起点"误导
          const bStart = Number(s.batchStart);
          const bEnd   = Number(s.batchEnd);
          if (isFinite(bStart) && isFinite(bEnd) && bEnd >= bStart) {
            curText.textContent = 'addr=' + bStart + (bEnd > bStart ? '-' + bEnd : '');
          } else {
            curText.textContent = 'addr=' + s.current;
          }
        } else {
          let freqMHz = (s.current / 1000).toFixed(3);
          curText.textContent = freqMHz + ' MHz';
        }
      }
    } catch(e) { /* ignore poll errors */ }
    finally { sweepUiSyncing = false; }
  }

  // ---- 初始化入口 ----
  function init() {
    initBtnGroups();
    initInputs();
    populatePresetSelect();

    document.getElementById('btn-preset-save')?.addEventListener('click', handleSavePreset);
    document.getElementById('btn-preset-del')?.addEventListener('click', handleDelPreset);
    // 选中预设即加载 (不再需要 Load 按钮)
    document.getElementById('sel-preset')?.addEventListener('change', handleLoadPreset);

    // Sweep 控制
    initSweepControls();

    // 初始加载
    refreshPanel();

    // 初始状态同步: 设备可能已在扫描中 (本页刷新/其他设备启动), 直接对齐 UI,
    // 避免刷新后误以为空闲 (按钮显示 START, 点启动报 ERR_BUSY)。
    updateSweepUI();

    window.ConfigPanel = {
      refresh: refreshPanel,
      updateSweepUI: updateSweepUI,
      isSweepPolling: () => sweepPolling,
      setSweepPolling: (v) => { sweepPolling = !!v; }
    };
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', init);
  } else {
    init();
  }

})();
