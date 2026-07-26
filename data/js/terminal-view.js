/**
 * terminal-view.js — 终端视图管理
 *
 * - 日志行渲染 (颜色编码)
 * - 自动滚动 (暂停/恢复)
 * - 消息发送
 */
(function() {
  'use strict';

  const MAX_LINES = 200;
  let autoScroll = true;

  // ---- 日志 ----
  function log(text, cls) {
    const output = document.getElementById('terminal-output');
    if (!output) return;

    const line = document.createElement('div');
    line.className = 'line ' + (cls || 'system');
    const ts = new Date().toLocaleTimeString();
    line.textContent = `[${ts}] ${text}`;
    output.appendChild(line);

    // 限制行数
    while (output.children.length > MAX_LINES) {
      output.removeChild(output.firstChild);
    }

    // 自动滚动
    if (autoScroll) {
      output.scrollTop = output.scrollHeight;
    }
  }

  // ---- 滚动检测 ----
  function initScroll() {
    const output = document.getElementById('terminal-output');
    if (!output) return;

    output.addEventListener('scroll', () => {
      const atBottom = (output.scrollHeight - output.scrollTop - output.clientHeight) < 30;
      autoScroll = atBottom;
    });
  }

  let sending = false;

  // ---- 发送消息 ----
  async function handleSend() {
    if (sending) return;

    const inp = document.getElementById('inp-message');
    const btn = document.getElementById('btn-send');
    const msg = inp.value.trim();
    if (!msg) return;

    sending = true;
    btn.disabled = true;
    btn.textContent = 'Sending...';
    log('TX: ' + msg, 'tx');

    try {
      const resp = await window.API.transmit(msg);
      if (!resp.code) {
        // 显示完整发送参数 (来自服务器的 config)
        if (resp.enc) {
          var ph = resp.phase || 'NRM';
          var tp = resp.type || 'ALPHA';
          var tn = resp.tone || 'D';
          log(resp.enc + ' ' + ph + ' ' + tp + ' tone=' + tn + ' addr=' + resp.addr + ' ' + (resp.freq/1000).toFixed(3) + 'MHz ' + resp.baud + 'bps', 'rx');
        }
        log('Sent OK', 'rx');
      } else {
        log('Send failed: ' + (resp.detail || 'unknown'), 'error');
      }
    } catch(e) {
      log('Send error: ' + e.message, 'error');
    }

    inp.value = '';
    // 延迟恢复按钮 (TX 在主循环中异步执行, 约 1-2s)
    setTimeout(() => {
      sending = false;
      btn.disabled = false;
      btn.textContent = 'Transmit';
    }, 2500);
    inp.focus();
  }

  // ---- 清除 ----
  function clearLog() {
    const output = document.getElementById('terminal-output');
    if (output) output.innerHTML = '';
  }

  // ---- 初始化 ----
  function init() {
    initScroll();

    document.getElementById('btn-send')?.addEventListener('click', handleSend);
    document.getElementById('btn-clear-log')?.addEventListener('click', clearLog);

    const inp = document.getElementById('inp-message');
    if (inp) {
      inp.addEventListener('keydown', (e) => {
        if (e.key === 'Enter') handleSend();
      });
    }

    window.Terminal = { log, clear: clearLog };
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', init);
  } else {
    init();
  }

})();
