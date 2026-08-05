/**
 * main.js — BeeperTX Web 控制面板入口
 *
 * - 状态轮询 (每 3s, sweep 运行期间暂停, 退出后按需立即恢复)
 * - 连接状态指示灯
 * - 协调整体初始化
 */
(function() {
  'use strict';

  // ---- 状态轮询 ----
  // sweep 运行中暂停 /api/status: 减少 AsyncTCP 回调负载 (发射时序敏感);
  // 退出后由 ConfigPanel 通过 __setSweepActive(false) 触发一次立即刷新。
  let sweepActive = false;

  async function pollStatus() {
    if (sweepActive) return;
    try {
      const status = await window.API.getStatus();
      if (!status || status.code) return;

      updateStatusBar(status);
    } catch(e) {
      // 非关键, 静默忽略
    }
  }

  // ConfigPanel 在 sweep 启动/停止时回调
  window.__setSweepActive = function(active) {
    sweepActive = active;
    if (!active) pollStatus();  // 退出后按需立即刷新一次
  };

  function updateStatusBar(s) {
    // 连接状态
    const dot = document.getElementById('conn-dot');
    const txt = document.getElementById('conn-text');
    dot.className = 'conn-dot connected';
    txt.textContent = 'Connected';

    // IP (从 WiFi status 获取)
    updateWiFiInfo();

    // TX 计数
    document.getElementById('tx-count').textContent = s.txCount || 0;
  }

  async function updateWiFiInfo() {
    try {
      const w = await window.API.wifiStatus();
      if (!w || w.code) return;
      document.getElementById('wifi-ip').textContent = w.ip || '--';

      // 更新连接点
      const dot = document.getElementById('conn-dot');
      if (w.state === 3) {  // AP_MODE
        dot.className = 'conn-dot ap';
      }
    } catch(e) { /* ignore */ }
  }

  // ---- 初始化 ----
  function init() {
    // 定时轮询状态
    pollStatus();
    setInterval(pollStatus, 3000);

    // 扫描状态轮询 (每 1.5s, 仅 sweep 进行中才调用 /api/sweep/status)
    setInterval(() => {
      if (window.ConfigPanel && window.ConfigPanel.isSweepPolling()) {
        window.ConfigPanel.updateSweepUI();
      }
    }, 1500);

    // 日志轮询 (每 2s)
    setInterval(pollLog, 2000);

    // 定期刷新配置面板
    setInterval(() => {
      if (window.ConfigPanel) window.ConfigPanel.refresh();
    }, 10000);
  }

  // ---- 日志轮询 ----
  async function pollLog() {
    try {
      const text = await window.API.getLog();
      if (!text || text.code) return;
      const lines = text.split('\n').filter(l => l.trim());
      if (!window._lastLogLines) window._lastLogLines = [];

      // 判断上次拉取的内容是否是当前内容的连续前缀 (环形缓冲无回绕):
      // 是 → 只追加新增行; 否 → 缓冲已回绕 (旧行被挤出), 清屏全量重绘。
      // 原实现用 lines.length < prevLen 判回绕: 缓冲区写满后长度恒为 50,
      // 永不为 true → 追加循环一次都不执行, 新日志从此不再显示。
      let isPrefix = lines.length >= window._lastLogLines.length;
      for (let i = 0; isPrefix && i < window._lastLogLines.length; i++) {
        if (lines[i] !== window._lastLogLines[i]) isPrefix = false;
      }

      const start = isPrefix ? window._lastLogLines.length : 0;
      if (!isPrefix) {
        window.Terminal.clear();
        window.Terminal.log('--- log buffer wrapped, redrawing ---', 'system');
      }

      for (let i = start; i < lines.length; i++) {
        const l = lines[i];
        let cls = 'system';
        if (l.includes('TX>'))      cls = 'tx';
        else if (l.includes('ERR')) cls = 'error';
        else if (l.includes('SWEEP')) cls = 'info';
        else if (l.includes('OK'))  cls = 'rx';
        window.Terminal.log(l, cls);
      }
      window._lastLogLines = lines;

      // sweep 兜底检测: 本页面未主动 start (其他设备/串口启动) 时, 从新增
      // 日志行发现 sweep 开始 → 开启状态轮询。关闭不做: 轮询自身发现
      // running=false 或本页 stop 会可靠关闭, 避免历史缓冲误判。
      if (start < lines.length) {
        const joined = lines.slice(start).join('\n');
        if (joined.includes('SWEEP START') &&
            window.ConfigPanel && window.ConfigPanel.setSweepPolling) {
          window.ConfigPanel.setSweepPolling(true);
        }
      }
    } catch(e) { /* ignore */ }
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', init);
  } else {
    init();
  }

})();
