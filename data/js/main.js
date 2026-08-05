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

    // 扫描状态兜底校准 (每 5s, 无条件): 其他设备/串口启动的 sweep 也能被
    // 检测到 (本页未主动 start 时无高频轮询); updateSweepUI 内有并发保护。
    setInterval(() => {
      if (window.ConfigPanel) window.ConfigPanel.updateSweepUI();
    }, 5000);

    // 日志轮询 (每 2s)
    setInterval(pollLog, 2000);

    // 定期刷新配置面板
    setInterval(() => {
      if (window.ConfigPanel) window.ConfigPanel.refresh();
    }, 10000);
  }

  // ---- 日志轮询 (增量拉取) ----
  // 后端 /api/log 带写序号: 首次全量, 之后 ?after=<seq> 只返回新行,
  // 避免每次 2s 轮询都传输整个环形缓冲 (sweep 高频日志下尤为明显)。
  // wrapped=true 表示客户端序号已滚出环形窗口 (被覆盖), 清屏全量重绘。
  let lastLogSeq = null;

  async function pollLog() {
    try {
      const text = (lastLogSeq === null)
        ? await window.API.getLog()
        : await window.API.getLogAfter(lastLogSeq);
      if (!text || text.code || !Array.isArray(text.lines)) return;

      const lines = text.lines.filter(l => l.trim());
      const wrapped = (text.wrapped === true);
      if (wrapped) {
        window.Terminal.clear();
        window.Terminal.log('--- log buffer wrapped, redrawing ---', 'system');
      }

      for (let i = 0; i < lines.length; i++) {
        const l = lines[i];
        let cls = 'system';
        if (l.includes('TX>'))      cls = 'tx';
        else if (l.includes('ERR')) cls = 'error';
        else if (l.includes('SWEEP')) cls = 'info';
        else if (l.includes('OK'))  cls = 'rx';
        window.Terminal.log(l, cls);
      }
      lastLogSeq = (typeof text.seq === 'number') ? text.seq : lastLogSeq;

      // sweep 兜底检测: 本页面未主动 start (其他设备/串口启动) 时, 从新增
      // 日志行发现 sweep 开始 → 开启状态轮询。关闭不做: 轮询自身发现
      // running=false 或本页 stop 会可靠关闭, 避免历史缓冲误判。
      if (lines.length && !wrapped) {
        const joined = lines.join('\n');
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
