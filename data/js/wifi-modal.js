/**
 * wifi-modal.js — WiFi 配网弹窗
 *
 * - 扫描附近网络
 * - 选择/输入 SSID + 密码
 * - 提交凭据并提示重启
 */
(function() {
  'use strict';

  // ---- 打开/关闭 ----
  function show() {
    document.getElementById('wifi-modal').classList.remove('hidden');
    document.getElementById('wifi-status-text').textContent = 'Click Scan to find networks.';
    handleScan();
  }

  function hide() {
    document.getElementById('wifi-modal').classList.add('hidden');
  }

  // ---- 扫描 (异步: 后台触发 + 轮询) ----
  async function handleScan() {
    const status = document.getElementById('wifi-status-text');
    const sel = document.getElementById('sel-wifi-ssid');

    status.textContent = 'Scan triggered, waiting for results...';
    status.className = 'terminal-alert';

    // 第一次调用触发后台扫描, 返回缓存 (可能为空)
    await window.API.wifiScan();

    // 轮询等待扫描完成 (最多 6 秒)
    let attempts = 0;
    let data = [];
    while (attempts < 12) {
      await new Promise(r => setTimeout(r, 500));
      try {
        const resp = await window.API.wifiScan();
        // wifiScan 返回的可能是 JSON 数组字符串或对象
        let result = resp;
        if (typeof result === 'string') {
          try { result = JSON.parse(result); } catch(e) { /* keep string */ }
        }
        if (Array.isArray(result) && result.length > 0) {
          data = result;
          break;
        }
        // 也可能是 {scanResult: "[]"} 包装
        if (result && typeof result.scanResult === 'string') {
          try {
            const parsed = JSON.parse(result.scanResult);
            if (Array.isArray(parsed) && parsed.length > 0) {
              data = parsed;
              break;
            }
          } catch(e) {}
        }
      } catch(e) {}
      attempts++;
    }

    if (data.length > 0) {
      sel.innerHTML = '<option value="">-- select network --</option>';
      data.sort((a, b) => (b.rssi || 0) - (a.rssi || 0));
      data.forEach(net => {
        const opt = document.createElement('option');
        opt.value = net.ssid;
        opt.textContent = `${net.ssid} (${net.rssi}dBm ${net.secure ? '🔒' : '🔓'})`;
        sel.appendChild(opt);
      });
      status.textContent = `Found ${data.length} network(s).`;
      status.className = 'terminal-alert terminal-alert-primary';
    } else {
      status.textContent = 'No networks found. Try manual entry or scan again.';
      status.className = 'terminal-alert terminal-alert-error';
    }
  }

  // ---- SSID 选择同步 ----
  function onSsidSelect() {
    const sel = document.getElementById('sel-wifi-ssid');
    const inp = document.getElementById('inp-wifi-ssid');
    if (sel.value) {
      inp.value = sel.value;
    }
  }

  // ---- 连接 ----
  async function handleConnect() {
    const status = document.getElementById('wifi-status-text');
    const ssid = document.getElementById('inp-wifi-ssid').value.trim();
    const pass = document.getElementById('inp-wifi-pass').value;

    if (!ssid) {
      status.textContent = 'Please enter SSID.';
      status.className = 'terminal-alert terminal-alert-error';
      return;
    }

    status.textContent = 'Saving credentials & connecting...';
    status.className = 'terminal-alert';

    try {
      const resp = await window.API.wifiSet(ssid, pass);
      if (!resp.code) {
        status.textContent = 'Saved! Device will reboot. Reconnect to new IP.';
        status.className = 'terminal-alert terminal-alert-primary';
        document.getElementById('btn-wifi-connect').disabled = true;
      } else {
        status.textContent = 'Failed: ' + (resp.detail || 'unknown');
        status.className = 'terminal-alert terminal-alert-error';
      }
    } catch(e) {
      status.textContent = 'Error: ' + e.message;
      status.className = 'terminal-alert terminal-alert-error';
    }
  }

  // ---- 初始化 ----
  function init() {
    document.getElementById('btn-wifi-config')?.addEventListener('click', show);
    document.getElementById('btn-wifi-cancel')?.addEventListener('click', hide);
    document.getElementById('btn-wifi-scan')?.addEventListener('click', handleScan);
    document.getElementById('btn-wifi-connect')?.addEventListener('click', handleConnect);
    document.getElementById('sel-wifi-ssid')?.addEventListener('change', onSsidSelect);
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', init);
  } else {
    init();
  }

})();
