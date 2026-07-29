/**
 * api-client.js — REST API fetch 封装
 *
 * 所有与 BeeperTX 后端的通信通过此模块。
 * 使用 ES module 模式, 但通过全局 script 标签加载以兼容 ESP32 静态文件服务。
 * 暴露 window.API 对象。
 */
(function() {
  'use strict';

  const BASE = '';  // 同源

  async function _fetch(method, path, body) {
    const opts = {
      method,
      headers: { 'Content-Type': 'application/json' }
    };
    if (body) opts.body = JSON.stringify(body);
    const resp = await fetch(BASE + path, opts);
    const text = await resp.text();
    try { return JSON.parse(text); }
    catch(e) { return { code: -1, detail: text, raw: text }; }
  }

  window.API = {
    /** GET /api/ping */
    ping()       { return _fetch('GET', '/api/ping'); },

    /** GET /api/status → DeviceStatus */
    getStatus()  { return _fetch('GET', '/api/status'); },

    /** GET /api/config → TxConfig */
    getConfig()  { return _fetch('GET', '/api/config'); },

    /**
     * POST /api/config — 部分或全部更新配置
     * @param {Object} cfg — 键值对, 只传需要修改的字段
     */
    setConfig(cfg) { return _fetch('POST', '/api/config', cfg); },

    /**
     * POST /api/transmit — 发射
     * @param {string} [msg] — 可选消息覆盖
     */
    transmit(msg) {
      const body = msg ? { message: msg } : {};
      return _fetch('POST', '/api/transmit', body);
    },

    /**
     * POST /api/reset */
    reset()       { return _fetch('POST', '/api/reset'); },

    /** GET /api/wifi/status */
    wifiStatus()  { return _fetch('GET', '/api/wifi/status'); },

    /** GET /api/wifi/scan */
    wifiScan()    { return _fetch('GET', '/api/wifi/scan'); },

    /**
     * POST /api/wifi
     * @param {string} ssid
     * @param {string} password
     */
    wifiSet(ssid, password) { return _fetch('POST', '/api/wifi', { ssid, password }); },

    /** GET /api/log */
    getLog()      { return _fetch('GET', '/api/log'); },

    /**
     * POST /api/sweep/start
     * @param {string} mode — 'addr' 或 'freq'
     */
    sweepStart(mode) { return _fetch('POST', '/api/sweep/start', { mode }); },

    /** POST /api/sweep/stop */
    sweepStop()      { return _fetch('POST', '/api/sweep/stop'); },

    /** GET /api/sweep/status */
    sweepStatus()    { return _fetch('GET', '/api/sweep/status'); }
  };

})();
