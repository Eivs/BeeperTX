/**
 * main.js — BeeperTX Web 控制面板入口
 *
 * - 状态轮询 (每 3s)
 * - 连接状态指示灯
 * - 协调整体初始化
 */
(function() {
  'use strict';

  // ---- 状态轮询 ----
  async function pollStatus() {
    try {
      const status = await window.API.getStatus();
      if (!status || status.code) return;

      updateStatusBar(status);
    } catch(e) {
      // 非关键, 静默忽略
    }
  }

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
    // Console.loadLog();

    // 定时轮询状态
    pollStatus();
    setInterval(pollStatus, 3000);

    // 定期刷新配置面板
    setInterval(() => {
      if (window.ConfigPanel) window.ConfigPanel.refresh();
    }, 10000);
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', init);
  } else {
    init();
  }

})();
