/**
 * @file    API.cpp
 * @brief   命令调度器实现 — CommandDispatcher + C 接口薄封装
 *
 * 架构 (2026-07 重构, 消除全局单例 Service Locator):
 *   - CommandDispatcher : 命令分发 + 状态持有 (配置/状态缓存/发射计数)
 *   - ConfigStore       : TxConfig 的 NVS 持久化 (CMD_SAVE_CONFIG)
 *   - LogRingBuffer     : 发射日志环形缓冲区
 *   - TxQueue           : 待发射/待扫描/待配网 的异步请求队列
 *
 * 依赖通过 setTransmitter/setWiFiManager/setSweeper 显式注入,
 * 不再使用模块级 static 可变全局。对外保留 api_* C 函数作为
 * 薄封装, 调用方 (SerialConsole/WebServer) 零改动接入。
 */

#include "API.h"
#include "Transmitter.h"
#include "WiFiManager.h"
#include "Si446x.h"
#include "Sweeper.h"
#include "ConfigStore.h"
#include "LogRingBuffer.h"
#include "TxQueue.h"
#include <Arduino.h>
#include <cstring>

namespace {

// ============================================================
// 参数校验
// ============================================================

bool isFreqValid(uint32_t kHz) {
  return kHz >= 119000 && kHz <= 1050000;
}

bool isBaudRateValid(EncodingType enc, uint16_t br) {
  // 码率按编码区分: POCSAG 512/1200, FLEX 1600/2400/3200
  if (enc == EncodingType::POCSAG) return br == 512 || br == 1200;
  return br == 1600 || br == 2400 || br == 3200;
}

bool isPocsagAddrValid(uint32_t addr) {
  return addr <= 0x1FFFFF;  // 21-bit
}

bool isTxPowerValid(uint8_t pwr) {
  return pwr <= 127;
}

/** 每命令期望的负载类型 (用于 union 判别校验) */
PayloadType expectedPayload(CmdID cmd) {
  switch (cmd) {
    case CmdID::CMD_SET_CONFIG:  return PayloadType::SET_CONFIG;
    case CmdID::CMD_TRANSMIT:    return PayloadType::TRANSMIT_MSG;
    case CmdID::CMD_SET_WIFI:    return PayloadType::WIFI_CRED;
    case CmdID::CMD_GET_LOG:     return PayloadType::LOG_COUNT;
    case CmdID::CMD_SWEEP_START: return PayloadType::SWEEP_START;
    default:                     return PayloadType::NONE;
  }
}

const char* cmdName(CmdID cmd) {
  switch (cmd) {
  case CmdID::CMD_PING:          return "PING";
  case CmdID::CMD_GET_STATUS:    return "STATUS";
  case CmdID::CMD_GET_CONFIG:    return "GET_CONFIG";
  case CmdID::CMD_RESET:         return "RESET";
  case CmdID::CMD_SET_CONFIG:    return "SET_CONFIG";
  case CmdID::CMD_SAVE_CONFIG:   return "SAVE_CONFIG";
  case CmdID::CMD_TRANSMIT:      return "TX";
  case CmdID::CMD_ABORT:         return "ABORT";
  case CmdID::CMD_SET_WIFI:      return "SET_WIFI";
  case CmdID::CMD_SCAN_WIFI:     return "SCAN_WIFI";
  case CmdID::CMD_GET_WIFI_STATUS: return "GET_WIFI_STATUS";
  case CmdID::CMD_GET_LOG:       return "GET_LOG";
  case CmdID::CMD_SWEEP_START:   return "SWEEP_START";
  case CmdID::CMD_SWEEP_STOP:    return "SWEEP_STOP";
  case CmdID::CMD_SWEEP_STATUS:  return "SWEEP_STATUS";
  default:                       return "UNKNOWN";
  }
}

// ============================================================
// CommandDispatcher — 命令分发 + 状态持有
// ============================================================

class CommandDispatcher {
public:
  void init();
  void dispatch(const CmdRequest* req, CmdResponse* resp);
  void loop();

  // ---- 依赖注入 ----
  void setTransmitter(Transmitter* tx) { m_tx = tx; }
  void setWiFiManager(WiFiManager* wifi) { m_wifi = wifi; }
  void setSweeper(Sweeper* sweeper) { m_sweeper = sweeper; }
  void setRadioReady(bool ready) { m_radioReady = ready; }

  // ---- 查询 ----
  TxConfig getConfig() const { return m_config; }
  void triggerScan() { m_queue.triggerScan(); }
  void getScanCache(char* buf, size_t size) { m_queue.getScanCache(buf, size); }

  // ---- 日志 ----
  void log(const char* line) {
    // 同步输出到串口 (方便调试)
    Serial.print(F("[LOG] "));
    Serial.println(line);
    m_log.add(line);
  }
  void getLog(char* buf, size_t size) { m_log.get(buf, size); }
  void getLogAfter(char* buf, size_t size, uint32_t after,
                   uint32_t* seqOut, bool* wrappedOut) {
    m_log.getAfter(buf, size, after, seqOut, wrappedOut);
  }

  // ---- 快捷入口 ----
  RespCode sweepStartSimple(uint8_t mode, uint32_t* startOut, int* detailOut);

private:
  // 依赖 (构造注入, 显式设置)
  Transmitter* m_tx      = nullptr;
  WiFiManager* m_wifi    = nullptr;
  Sweeper*     m_sweeper = nullptr;
  bool         m_radioReady = true;  //!< Si4463 可用 (main 初始化失败时置 false, 后台重试)

  // 组件
  ConfigStore   m_store;
  LogRingBuffer m_log;
  TxQueue       m_queue;

  // 状态
  TxConfig     m_config;
  DeviceStatus m_status;
  bool         m_statusCached = false;  //!< partId/romId/chipRev 已缓存 (静态信息)
  uint32_t     m_statusLastQueryMs = 0; //!< 上次实时 SPI 查询时刻 (冷却限速)
  uint32_t     m_txCount = 0;

  // 状态查询限速: 避免发射间隙/高频轮询对 Si4463 频繁发 SPI 命令
  static const uint32_t STATUS_QUERY_MIN_INTERVAL_MS = 500;

  // ---- Handlers ----
  void handlePing(const CmdRequest* req, CmdResponse* resp);
  void handleGetStatus(const CmdRequest* req, CmdResponse* resp);
  void handleGetConfig(const CmdRequest* req, CmdResponse* resp);
  void handleSetConfig(const CmdRequest* req, CmdResponse* resp);
  void handleSaveConfig(const CmdRequest* req, CmdResponse* resp);
  void handleTransmit(const CmdRequest* req, CmdResponse* resp);
  void handleAbort(const CmdRequest* req, CmdResponse* resp);
  void handleReset(const CmdRequest* req, CmdResponse* resp);
  void handleSetWiFi(const CmdRequest* req, CmdResponse* resp);
  void handleScanWiFi(const CmdRequest* req, CmdResponse* resp);
  void handleGetWifiStatus(const CmdRequest* req, CmdResponse* resp);
  void handleGetLog(const CmdRequest* req, CmdResponse* resp);
  void handleSweepStart(const CmdRequest* req, CmdResponse* resp);
  void handleSweepStop(const CmdRequest* req, CmdResponse* resp);
  void handleSweepStatus(const CmdRequest* req, CmdResponse* resp);

  Transmitter::Config makeTxConfig() const;
};

// ============================================================
// Handler: PING
// ============================================================

void CommandDispatcher::handlePing(const CmdRequest* req, CmdResponse* resp) {
  resp->cmd  = CmdID::CMD_PING;
  resp->code = RespCode::OK;
  // detail 返回系统运行毫秒数, 用于延迟估算
  resp->detail = (int32_t)millis();
}

// ============================================================
// Handler: GET_STATUS
// ============================================================

void CommandDispatcher::handleGetStatus(const CmdRequest* req, CmdResponse* resp) {
  resp->cmd  = CmdID::CMD_GET_STATUS;
  resp->code = RespCode::OK;

  // 静态芯片信息 (partId/romId/chipRev) 只在首次查询 (Radio 就绪后), 之后缓存。
  // 避免 webUI 每 3s 轮询触发 5 次 SPI 事务 (每次 doAPI 在 Si4463 忙时可
  // 忙等 400ms 且持跨核递归锁) → webUI 高延时 + 阻塞 txTask 的 enterTx/exitTx。
  if (!m_statusCached) {
    si446x_info_t info;
    Si446x_getInfo(&info);
    if (info.part != 0) {
      m_status.partId       = info.part;
      m_status.romId        = info.romId;
      m_status.chipRev      = info.chipRev;
      m_statusCached        = true;
    }
  }

  // 发射期间 (txTask 持锁/位敲击): 跳过实时 SPI 查询, 用缓存状态。
  // Si4463 处于 Direct TX 时不响应多数命令, doAPI 会忙等到 CTS 超时
  // (每个查询最多 400ms), 持跨核锁阻塞 txTask → 发射停滞 + WDT 复位
  // (用户实测"追码自动停止"的根因)。
  // 另: 非发射期也限速 (冷却窗口), 避免追码批间隙紧贴 Si4463 状态机
  // 恢复窗口插入查询 (CTS 不响应 → 忙等), 同时缓解 webUI 高延时。
  const uint32_t nowMs = millis();
  const bool txBusy = m_tx && m_tx->isBusy();
  if (!txBusy && (nowMs - m_statusLastQueryMs >= STATUS_QUERY_MIN_INTERVAL_MS)) {
    m_status.state   = (uint8_t)Si446x_getState();
    m_status.freqKHz = Si446x_getFrequencyKHz();
    m_status.txPower = Si446x_getTxPower();
    m_status.rssiDbm = Si446x_getRSSI();
    m_statusLastQueryMs = nowMs;
  } else if (txBusy) {
    m_status.state   = SI446X_STATE_TX;  // 正在发射
    // freqKHz/txPower/rssi 保持上次缓存值 (发射中 RSSI 无意义)
  }
  m_status.temperatureC = 0;          // Si4463 无温度传感器, 预留
  m_status.uptimeS      = millis() / 1000;
  m_status.txCount      = m_txCount;

  resp->status = m_status;
}

// ============================================================
// Handler: GET_CONFIG
// ============================================================

void CommandDispatcher::handleGetConfig(const CmdRequest* req, CmdResponse* resp) {
  resp->cmd    = CmdID::CMD_GET_CONFIG;
  resp->code   = RespCode::OK;
  resp->config = m_config;
}

// ============================================================
// Handler: SET_CONFIG
// ============================================================

void CommandDispatcher::handleSetConfig(const CmdRequest* req, CmdResponse* resp) {
  // 扫描期间禁止修改配置
  if (m_sweeper && m_sweeper->isRunning()) {
    resp->cmd = CmdID::CMD_SET_CONFIG; resp->code = RespCode::ERR_BUSY;
    resp->detail = -10;
    return;
  }
  // 发射中 (队列积压或 txTask 位敲击) 修改频率/功率会与 Direct TX 冲突:
  // Si4463 处于 TX 时 CTS 超时忙等 (每次最多 400ms), 命令静默失败。
  if (m_tx && m_tx->isBusy()) {
    resp->cmd = CmdID::CMD_SET_CONFIG; resp->code = RespCode::ERR_BUSY;
    resp->detail = -11;
    return;
  }

  const TxConfig& c = req->setConfig;

  if (!isFreqValid(c.freqKHz)) {
    resp->cmd = CmdID::CMD_SET_CONFIG; resp->code = RespCode::ERR_PARAM;
    resp->detail = -1;
    log("SET_CONFIG: invalid freq");
    return;
  }
  if (!isBaudRateValid(c.encoding, c.baudRate)) {
    resp->cmd = CmdID::CMD_SET_CONFIG; resp->code = RespCode::ERR_PARAM;
    resp->detail = -2;
    log("SET_CONFIG: invalid baudRate");
    return;
  }
  if (c.encoding == EncodingType::POCSAG && !isPocsagAddrValid(c.address)) {
    resp->cmd = CmdID::CMD_SET_CONFIG; resp->code = RespCode::ERR_PARAM;
    resp->detail = -3;
    log("SET_CONFIG: invalid POCSAG address");
    return;
  }
  if (!isTxPowerValid(c.txPower)) {
    resp->cmd = CmdID::CMD_SET_CONFIG; resp->code = RespCode::ERR_PARAM;
    resp->detail = -4;
    log("SET_CONFIG: invalid txPower");
    return;
  }

  // FLEX 无极性概念: 强制直发 (invert=false), 防 NORMAL 误配导致接收机无法同步
  // (web/串口两条入口都经此; 出厂默认 NORMAL 仅对 POCSAG 有意义)
  TxConfig effective = c;
  if (effective.encoding == EncodingType::FLEX) {
    effective.phase = PhaseType::INVERTED;
  }

  m_config = effective;

  // 立即生效: 更新频率和功率
  Si446x_setFrequencyKHz(effective.freqKHz);
  Si446x_setTxPower(effective.txPower);

  resp->cmd  = CmdID::CMD_SET_CONFIG;
  resp->code = RespCode::OK;
}

// ============================================================
// Handler: SAVE_CONFIG (将当前配置持久化到 NVS)
// ============================================================

void CommandDispatcher::handleSaveConfig(const CmdRequest* req, CmdResponse* resp) {
  resp->cmd  = CmdID::CMD_SAVE_CONFIG;
  resp->code = RespCode::OK;

  if (!m_store.save(m_config)) {
    resp->code = RespCode::ERR_HW;
    resp->detail = -1;
    log("SAVE_CONFIG: NVS write failed");
    return;
  }
  log("SAVE_CONFIG: saved to NVS");
}

// ============================================================
// Handler: TRANSMIT
// ============================================================

Transmitter::Config CommandDispatcher::makeTxConfig() const {
  // 枚举已统一 (ProtocolTypes.h), 无需强转。
  Transmitter::Config c;
  c.encoding = m_config.encoding;
  c.phase    = m_config.phase;
  c.baudRate = m_config.baudRate;
  c.tone     = m_config.tone;
  c.address  = m_config.address;
  memcpy(c.message, m_config.message, sizeof(c.message));
  c.msgType  = m_config.msgType;
  return c;
}

void CommandDispatcher::handleTransmit(const CmdRequest* req, CmdResponse* resp) {
  // 射频离线 (Si4463 初始化失败, 后台重试中) — 拒绝发射
  if (!m_radioReady) {
    resp->cmd = CmdID::CMD_TRANSMIT; resp->code = RespCode::ERR_HW;
    resp->detail = -2;
    return;
  }

  // 扫描期间禁止单次发射
  if (m_sweeper && m_sweeper->isRunning()) {
    resp->cmd = CmdID::CMD_TRANSMIT; resp->code = RespCode::ERR_BUSY;
    resp->detail = -10;
    return;
  }

  if (!m_queue.setPendingTx(req->transmitMsg)) {
    resp->cmd = CmdID::CMD_TRANSMIT; resp->code = RespCode::ERR_BUSY;
    resp->detail = -1;
    return;
  }
  if (!m_tx) {
    resp->cmd = CmdID::CMD_TRANSMIT; resp->code = RespCode::ERR_HW;
    m_queue.consumePendingTx(nullptr, 0);  // 撤销入队
    return;
  }

  resp->cmd    = CmdID::CMD_TRANSMIT;
  resp->code   = RespCode::OK;
  resp->detail = 0;  // 返回 0 表示已入队, 实际 count 在 TX 完成后增加
}

// ============================================================
// Handler: ABORT
// ============================================================

void CommandDispatcher::handleAbort(const CmdRequest* req, CmdResponse* resp) {
  resp->cmd  = CmdID::CMD_ABORT;
  resp->code = RespCode::OK;
  // 扫描运行中: 整体停止 (stop() 内部先中止发射, 再恢复频率);
  // 否则仅中止当前发射 (清空队列 + 设 abort 标志, ≤1 bit 内停止)。
  if (m_sweeper && m_sweeper->isRunning()) {
    m_sweeper->stop();
  } else if (m_tx) {
    m_tx->abortTx();
  }
}

// ============================================================
// Handler: GET_WIFI_STATUS (仅查询状态, 不触发扫描)
// ============================================================

void CommandDispatcher::handleGetWifiStatus(const CmdRequest* req, CmdResponse* resp) {
  resp->cmd = CmdID::CMD_GET_WIFI_STATUS;

  if (!m_wifi) {
    resp->code = RespCode::ERR_HW;
    resp->detail = -1;
    return;
  }

  memset(&resp->wifi, 0, sizeof(resp->wifi));
  resp->wifi.state = (uint8_t)m_wifi->state();
  resp->wifi.rssi  = -1;
  // ssid/ip 此前从未填充 → /api/wifi/status 的 ip 恒为空, webUI wifi-ip 不显示
  strncpy(resp->wifi.ssid, m_wifi->ssid(), sizeof(resp->wifi.ssid) - 1);
  strncpy(resp->wifi.ip,   m_wifi->ipAddress(), sizeof(resp->wifi.ip) - 1);
  resp->code = RespCode::OK;
}

// ============================================================
// Handler: RESET
// ============================================================

void CommandDispatcher::handleReset(const CmdRequest* req, CmdResponse* resp) {
  resp->cmd    = CmdID::CMD_RESET;
  resp->code   = RespCode::OK;
  resp->detail = 0;
  // 由传输层在返回响应后调用 ESP.restart()
}

// ============================================================
// Handler: SET_WIFI (非阻塞: 设置标记, 由主循环执行)
// ============================================================

void CommandDispatcher::handleSetWiFi(const CmdRequest* req, CmdResponse* resp) {
  resp->cmd = CmdID::CMD_SET_WIFI;

  if (!m_wifi) {
    resp->code = RespCode::ERR_HW;
    resp->detail = -1;
    return;
  }
  if (req->wifiCred.ssid[0] == '\0') {
    resp->code = RespCode::ERR_PARAM;
    resp->detail = -2;
    return;
  }

  if (!m_queue.setPendingWifi(req->wifiCred.ssid, req->wifiCred.password)) {
    resp->code = RespCode::ERR_BUSY;
    resp->detail = -3;
    return;
  }

  resp->code = RespCode::OK;
}

// ============================================================
// Handler: SCAN_WIFI (非阻塞: 触发后台扫描, 立即返回)
// ============================================================

void CommandDispatcher::handleScanWiFi(const CmdRequest* req, CmdResponse* resp) {
  resp->cmd = CmdID::CMD_SCAN_WIFI;

  if (!m_wifi) {
    resp->code = RespCode::ERR_HW;
    resp->detail = -1;
    return;
  }

  // 触发后台扫描 (由 api_loop 在主循环中执行)
  m_queue.triggerScan();

  // 立即返回当前 WiFi 状态 (扫描结果由 api_getScanCache() 提供)
  memset(&resp->wifi, 0, sizeof(resp->wifi));
  resp->wifi.state = (uint8_t)m_wifi->state();
  resp->code = RespCode::OK;
}

// ============================================================
// Handler: GET_LOG
// ============================================================

void CommandDispatcher::handleGetLog(const CmdRequest* req, CmdResponse* resp) {
  resp->cmd  = CmdID::CMD_GET_LOG;
  resp->code = RespCode::OK;
  // 日志通过 api_getLog() 导出 (避免在联合体中放大响应), 此处仅回执
  resp->detail = req->logCount;
}

// ============================================================
// Handler: SWEEP_START
// ============================================================

void CommandDispatcher::handleSweepStart(const CmdRequest* req, CmdResponse* resp) {
  resp->cmd = CmdID::CMD_SWEEP_START;

  if (!m_radioReady) {
    resp->code = RespCode::ERR_HW;
    resp->detail = -4;  // radio offline (Si4463 init 失败, 后台重试中)
    return;
  }
  if (!m_sweeper) {
    resp->code = RespCode::ERR_HW;
    resp->detail = -1;
    return;
  }
  if (m_sweeper->isRunning()) {
    resp->code = RespCode::ERR_BUSY;
    resp->detail = -2;
    return;
  }
  if (m_queue.isPendingTx()) {
    resp->code = RespCode::ERR_BUSY;
    resp->detail = -3;
    return;
  }

  // mode 校验
  uint8_t m = req->sweepStart.mode;
  if (m > 1) {
    resp->code = RespCode::ERR_PARAM;
    resp->detail = -4;
    return;
  }

  // 覆盖字段范围校验 (web 入口直接填 sweepStart, 无串口式解析校验;
  // 越界值强转会静默当 FLEX/其他协议跑)
  if ((req->sweepStart.encoding   >= 0 && req->sweepStart.encoding   > 1) ||
      (req->sweepStart.phase      >= 0 && req->sweepStart.phase      > 1) ||
      (req->sweepStart.tone       >= 0 && req->sweepStart.tone       > 3) ||
      (req->sweepStart.msgType    >= 0 && req->sweepStart.msgType    > 1) ||
      (req->sweepStart.intervalMs >= 0 &&
       (req->sweepStart.intervalMs < 1 || req->sweepStart.intervalMs > 60000))) {
    resp->code = RespCode::ERR_PARAM;
    resp->detail = -5;
    return;
  }
  // 码率按最终生效 encoding 校验 (未指定时继承当前配置)
  if (req->sweepStart.baudRate >= 0) {
    EncodingType effEnc = (req->sweepStart.encoding >= 0)
        ? static_cast<EncodingType>(req->sweepStart.encoding)
        : m_config.encoding;
    if (!isBaudRateValid(effEnc, (uint16_t)req->sweepStart.baudRate)) {
      resp->code = RespCode::ERR_PARAM;
      resp->detail = -5;
      return;
    }
  }

  // 构建 Sweeper 配置 — 发射参数从当前 m_config 继承, 覆盖字段可覆盖
  Sweeper::Config cfg;
  cfg.mode      = (m == 0) ? Sweeper::ADDR_SWEEP : Sweeper::FREQ_SWEEP;
  cfg.addrStart = m_config.address;
  cfg.freqStartKHz = m_config.freqKHz;
  cfg.encoding = m_config.encoding;
  cfg.phase    = m_config.phase;
  cfg.tone     = m_config.tone;
  cfg.msgType  = m_config.msgType;
  cfg.baudRate = m_config.baudRate;
  cfg.freqKHz   = m_config.freqKHz;
  cfg.address   = m_config.address;

  // 可选覆盖字段 (>=0 才覆盖, -1=未指定); sweepStart 字段为协议线格式
  // int16_t, 显式转回 ProtocolTypes 枚举 (值已由 handleSweepStart 校验)
  if (req->sweepStart.encoding  >= 0) cfg.encoding  = static_cast<EncodingType>(req->sweepStart.encoding);
  if (req->sweepStart.baudRate   >= 0) cfg.baudRate  = req->sweepStart.baudRate;
  if (req->sweepStart.phase      >= 0) cfg.phase     = static_cast<PhaseType>(req->sweepStart.phase);
  if (req->sweepStart.tone       >= 0) cfg.tone      = static_cast<ToneType>(req->sweepStart.tone);
  if (req->sweepStart.msgType    >= 0) cfg.msgType   = static_cast<MsgType>(req->sweepStart.msgType);
  if (req->sweepStart.intervalMs >= 0) cfg.intervalMs = (uint16_t)req->sweepStart.intervalMs;

  // FLEX 无极性概念: 强制直发 (与 SET_CONFIG 一致), 覆盖字段传 NORMAL 也无效
  if (cfg.encoding == EncodingType::FLEX) {
    cfg.phase = PhaseType::INVERTED;
  }

  int result = m_sweeper->start(cfg);
  if (result == 0) {
    resp->code = RespCode::OK;
    const char* modeName = (cfg.mode == Sweeper::ADDR_SWEEP) ? "addr" : "freq";
    char logLine[128];
    snprintf(logLine, sizeof(logLine), "SWEEP START mode=%s start=%lu",
             modeName, (unsigned long)((cfg.mode == Sweeper::ADDR_SWEEP)
                 ? cfg.addrStart : cfg.freqStartKHz));
    log(logLine);
  } else {
    resp->code   = RespCode::ERR_PARAM;
    resp->detail = result;
  }
}

// ============================================================
// Handler: SWEEP_STOP
// ============================================================

void CommandDispatcher::handleSweepStop(const CmdRequest* req, CmdResponse* resp) {
  resp->cmd = CmdID::CMD_SWEEP_STOP;

  if (m_sweeper && m_sweeper->isRunning()) {
    Sweeper::Status st = m_sweeper->getStatus();
    char logLine[64];
    snprintf(logLine, sizeof(logLine), "SWEEP STOP done=%lu/%lu",
             (unsigned long)st.done, (unsigned long)st.total);
    log(logLine);
    m_sweeper->stop();
  }
  resp->code = RespCode::OK;
}

// ============================================================
// Handler: SWEEP_STATUS
// ============================================================

void CommandDispatcher::handleSweepStatus(const CmdRequest* req, CmdResponse* resp) {
  resp->cmd  = CmdID::CMD_SWEEP_STATUS;
  resp->code = RespCode::OK;

  if (m_sweeper) {
    Sweeper::Status st = m_sweeper->getStatus();
    resp->sweepStatus.running     = st.running;
    resp->sweepStatus.mode        = st.mode;
    resp->sweepStatus.current     = st.current;
    resp->sweepStatus.total       = st.total;
    resp->sweepStatus.done        = st.done;
    resp->sweepStatus.batchStart  = st.batchStart;
    resp->sweepStatus.batchEnd    = st.batchEnd;
  } else {
    memset(&resp->sweepStatus, 0, sizeof(resp->sweepStatus));
  }
}

// ============================================================
// 公共调度入口
// ============================================================

void CommandDispatcher::dispatch(const CmdRequest* req, CmdResponse* resp) {
  if (!req || !resp) return;

  resp->cmd  = req->cmd;
  resp->code = RespCode::ERR_UNKNOWN;
  resp->detail = 0;

  // union 判别校验: 传输层必须声明实际负载, 防跨成员误读 (UB)
  if (req->payloadType != expectedPayload(req->cmd)) {
    resp->code = RespCode::ERR_PARAM;
    resp->detail = -99;
    char logLine[128];
    snprintf(logLine, sizeof(logLine), "%s ERR payloadType=%d",
             cmdName(req->cmd), (int)req->payloadType);
    log(logLine);
    return;
  }

  switch (req->cmd) {
  case CmdID::CMD_PING:          handlePing(req, resp);          break;
  case CmdID::CMD_GET_STATUS:    handleGetStatus(req, resp);     break;
  case CmdID::CMD_GET_CONFIG:    handleGetConfig(req, resp);     break;
  case CmdID::CMD_SET_CONFIG:    handleSetConfig(req, resp);     break;
  case CmdID::CMD_SAVE_CONFIG:   handleSaveConfig(req, resp);    break;
  case CmdID::CMD_TRANSMIT:      handleTransmit(req, resp);      break;
  case CmdID::CMD_ABORT:         handleAbort(req, resp);         break;
  case CmdID::CMD_RESET:         handleReset(req, resp);         break;
  case CmdID::CMD_SET_WIFI:      handleSetWiFi(req, resp);       break;
  case CmdID::CMD_SCAN_WIFI:     handleScanWiFi(req, resp);      break;
  case CmdID::CMD_GET_WIFI_STATUS: handleGetWifiStatus(req, resp); break;
  case CmdID::CMD_GET_LOG:       handleGetLog(req, resp);        break;
  case CmdID::CMD_SWEEP_START:   handleSweepStart(req, resp);    break;
  case CmdID::CMD_SWEEP_STOP:    handleSweepStop(req, resp);     break;
  case CmdID::CMD_SWEEP_STATUS:  handleSweepStatus(req, resp);   break;
  default: break;
  }

  // 集中错误日志 (非 OK 响应)
  if (resp->code != RespCode::OK) {
    char logLine[128];
    snprintf(logLine, sizeof(logLine), "%s ERR code=%d detail=%d",
             cmdName(resp->cmd), (int)resp->code, resp->detail);
    log(logLine);
  }
}

// ============================================================
// api_sweepStartSimple — Web/串口快捷入口 (规格 §8.2)
// ============================================================

RespCode CommandDispatcher::sweepStartSimple(uint8_t mode, uint32_t* startOut,
                                             int* detailOut) {
  CmdRequest req; CmdResponse resp;
  memset(&req, 0, sizeof(req));
  req.cmd = CmdID::CMD_SWEEP_START;
  req.payloadType = PayloadType::SWEEP_START;
  req.sweepStart.mode = mode;
  req.sweepStart.encoding   = -1;
  req.sweepStart.baudRate   = -1;
  req.sweepStart.phase      = -1;
  req.sweepStart.tone       = -1;
  req.sweepStart.msgType    = -1;
  req.sweepStart.intervalMs = -1;
  dispatch(&req, &resp);

  if (detailOut) *detailOut = resp.detail;
  if (startOut && resp.code == RespCode::OK) {
    TxConfig cfg = getConfig();
    *startOut = (mode == 0) ? cfg.address : cfg.freqKHz;
  }
  return resp.code;
}

// ============================================================
// Loop 调度 (由 main.cpp loop 调用)
// ============================================================

void CommandDispatcher::loop() {
  // WiFi 后台扫描 (非阻塞: 由主循环执行)
  if (m_queue.consumeScan() && m_wifi) {
    m_queue.setScanCache(m_wifi->scanNetworks().c_str());
    log("wifi scan complete");
  }

  // WiFi 凭据设置 (非阻塞: 由主循环执行)
  char ssid[33], pass[65];
  if (m_queue.consumeWifi(ssid, sizeof(ssid), pass, sizeof(pass)) && m_wifi) {
    m_wifi->setCredentials(ssid, pass);
    log("wifi credentials saved");
  }

  // 待发射的单次 TX (非阻塞: 由主循环执行)
  char pendingMsg[256];
  if (m_queue.consumePendingTx(pendingMsg, sizeof(pendingMsg)) && m_tx) {
    char savedMsg[256] = {0};
    bool msgOverride = (pendingMsg[0] != '\0');
    if (msgOverride) {
      strncpy(savedMsg, m_config.message, sizeof(savedMsg) - 1);
      strncpy(m_config.message, pendingMsg, sizeof(m_config.message) - 1);
    }

    // 日志: 完整发送参数
    {
      const char* encName = (m_config.encoding == EncodingType::POCSAG) ? "POCSAG" : "FLEX";
      const char* phName  = (m_config.phase == PhaseType::NORMAL) ? "NRM" : "INV";
      const char* tpName  = (m_config.msgType == MsgType::NUMERIC) ? "NUM" : "ALPHA";
      char buf[256];
      char toneL = "ABCD"[(uint8_t)m_config.tone & 3];
      snprintf(buf, sizeof(buf),
               "TX> %s %s %s addr=%lu freq=%ukHz baud=%u pw=%u tone=%c \"%s\"",
               encName, phName, tpName,
               (unsigned long)m_config.address,
               m_config.freqKHz,
               m_config.baudRate,
               m_config.txPower,
               toneL,
               m_config.message);
      log(buf);
    }

    if (m_tx->send(makeTxConfig())) {
      m_txCount++;
    } else {
      // 队列满 (可能 Sweeper 占用): 恢复消息并重新挂起待发射, 下帧重试
      if (msgOverride) {
        strncpy(m_config.message, savedMsg, sizeof(m_config.message) - 1);
      }
      m_queue.setPendingTx(pendingMsg[0] ? pendingMsg : nullptr);
      log("TX deferred (tx queue busy)");
      return;
    }

    if (msgOverride) {
      strncpy(m_config.message, savedMsg, sizeof(m_config.message) - 1);
    }
  }
  // 注: Sweeper 调度由 main.cpp loop() 调用, 不在此处 (规格 §8.4)
}

// ============================================================
// 初始化
// ============================================================

void CommandDispatcher::init() {
  // 从 NVS 恢复配置; 无存档则使用出厂默认
  m_store.loadOrDefault(m_config);

  // 将配置应用到 Si4463 硬件 (频率 + 功率)
  Si446x_setFrequencyKHz(m_config.freqKHz);
  Si446x_setTxPower(m_config.txPower);

  m_txCount = 0;
  m_queue.clear();
}

// ============================================================
// 全局调度器实例 (唯一状态持有者)
// ============================================================

CommandDispatcher g_dispatcher;

} // namespace

// ============================================================
// C 接口薄封装 (保持 ABI, 调用方零改动)
// ============================================================

extern "C" {

void api_init() {
  g_dispatcher.init();
}

void api_dispatch(const CmdRequest* req, CmdResponse* resp) {
  g_dispatcher.dispatch(req, resp);
}

void api_setTransmitter(Transmitter* tx) {
  g_dispatcher.setTransmitter(tx);
}

void api_setWiFiManager(WiFiManager* wifi) {
  g_dispatcher.setWiFiManager(wifi);
}

void api_setSweeper(Sweeper* sweeper) {
  g_dispatcher.setSweeper(sweeper);
}

void api_setRadioReady(bool ready) {
  g_dispatcher.setRadioReady(ready);
}

RespCode api_sweepStartSimple(uint8_t mode, uint32_t* startOut, int* detailOut) {
  return g_dispatcher.sweepStartSimple(mode, startOut, detailOut);
}

TxConfig api_getConfig() {
  return g_dispatcher.getConfig();
}

void api_getLog(char* buf, size_t size) {
  g_dispatcher.getLog(buf, size);
}

void api_getLogAfter(char* buf, size_t size, uint32_t after,
                     uint32_t* seqOut, bool* wrappedOut) {
  g_dispatcher.getLogAfter(buf, size, after, seqOut, wrappedOut);
}

void api_log(const char* line) {
  g_dispatcher.log(line);
}

void api_loop() {
  g_dispatcher.loop();
}

void api_triggerScan() {
  g_dispatcher.triggerScan();
}

void api_getScanCache(char* buf, size_t size) {
  g_dispatcher.getScanCache(buf, size);
}

} // extern "C"
