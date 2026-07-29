/**
 * @file    API.cpp
 * @brief   命令调度器实现 — 连接 API 定义与 Transmitter/HW 层
 *
 * 本文件实现 api_dispatch(), 负责:
 *   1. 参数校验
 *   2. 调用 Transmitter / Si446x 执行实际操作
 *   3. 填充 CmdResponse
 *
 * 串口控制、Web 控制等传输层各自将原始输入解析为 CmdRequest,
 * 调用 api_dispatch() 后序列化 CmdResponse 返回。
 */

#include "API.h"
#include "Transmitter.h"
#include "WiFiManager.h"
#include "Si446x.h"
#include "Sweeper.h"
#include <Arduino.h>
#include <cstring>

// ============================================================
// 全局状态
// ============================================================

static TxConfig       s_config;       // 当前发射配置
static DeviceStatus   s_status;       // 设备状态缓存 (每次 GET_STATUS 刷新)
static Transmitter*   s_tx  = nullptr;// Transmitter 实例
static WiFiManager*   s_wifi = nullptr;// WiFiManager 实例
static Sweeper*       s_sweeper = nullptr;// Sweeper 实例
static uint32_t       s_txCount = 0;
static bool           s_pendingTx = false;      // 待发射标记 (在主循环执行)
static char           s_pendingTxMsg[256];      // 待发射消息
static bool           s_pendingScan = false;    // 待扫描 WiFi
static bool           s_pendingWifiSet = false; // 待设置 WiFi
static char           s_pendingWifiSsid[33];
static char           s_pendingWifiPass[65];
static String         s_scanCache = "[]";       // 扫描结果缓存

// ============================================================
// 日志环形缓冲区
// ============================================================

#define LOG_MAX_LINES 50
#define LOG_LINE_LEN  128
static char s_logBuf[LOG_MAX_LINES][LOG_LINE_LEN];
static int  s_logHead = 0;   // 下一条写入位置
static int  s_logCount = 0;  // 已写入条数
static portMUX_TYPE s_logMux = portMUX_INITIALIZER_UNLOCKED;

// ============================================================
// 参数校验
// ============================================================

static bool isFreqValid(uint32_t kHz) {
  return kHz >= 119000 && kHz <= 1050000;
}

static bool isBaudRateValid(uint16_t br) {
  return br == 512 || br == 1200 || br == 1600 ||
         br == 2400 || br == 3200 || br == 6400;
}

static bool isPocsagAddrValid(uint32_t addr) {
  return addr <= 0x1FFFFF;  // 21-bit
}

static bool isTxPowerValid(uint8_t pwr) {
  return pwr <= 127;
}

// ============================================================
// Handler: PING
// ============================================================

static void handlePing(const CmdRequest* req, CmdResponse* resp) {
  resp->cmd  = CmdID::CMD_PING;
  resp->code = RespCode::OK;
  // detail 返回系统运行毫秒数, 用于延迟估算
  resp->detail = (int32_t)millis();
}

// ============================================================
// Handler: GET_STATUS
// ============================================================

static void handleGetStatus(const CmdRequest* req, CmdResponse* resp) {
  resp->cmd  = CmdID::CMD_GET_STATUS;
  resp->code = RespCode::OK;

  si446x_info_t info;
  Si446x_getInfo(&info);

  s_status.partId       = info.part;
  s_status.romId        = info.romId;
  s_status.chipRev      = info.chipRev;
  s_status.state        = (uint8_t)Si446x_getState();
  s_status.freqKHz      = Si446x_getFrequencyKHz();
  s_status.txPower      = Si446x_getTxPower();
  s_status.rssiDbm      = Si446x_getRSSI();
  s_status.temperatureC = 0;          // Si4463 无温度传感器, 预留
  s_status.uptimeS      = millis() / 1000;
  s_status.txCount      = s_txCount;

  resp->status = s_status;
}

// ============================================================
// Handler: GET_CONFIG
// ============================================================

static void handleGetConfig(const CmdRequest* req, CmdResponse* resp) {
  resp->cmd    = CmdID::CMD_GET_CONFIG;
  resp->code   = RespCode::OK;
  resp->config = s_config;
}

// ============================================================
// Handler: SET_CONFIG
// ============================================================

static void handleSetConfig(const CmdRequest* req, CmdResponse* resp) {
  // 扫描期间禁止修改配置
  if (s_sweeper && s_sweeper->isRunning()) {
    resp->cmd = CmdID::CMD_SET_CONFIG; resp->code = RespCode::ERR_BUSY;
    resp->detail = -10;
    return;
  }

  const TxConfig& c = req->setConfig;

  if (!isFreqValid(c.freqKHz)) {
    resp->cmd = CmdID::CMD_SET_CONFIG; resp->code = RespCode::ERR_PARAM;
    resp->detail = -1;
    api_log("SET_CONFIG: invalid freq");
    return;
  }
  if (!isBaudRateValid(c.baudRate)) {
    resp->cmd = CmdID::CMD_SET_CONFIG; resp->code = RespCode::ERR_PARAM;
    resp->detail = -2;
    api_log("SET_CONFIG: invalid baudRate");
    return;
  }
  if (c.encoding == EncodingType::POCSAG && !isPocsagAddrValid(c.address)) {
    resp->cmd = CmdID::CMD_SET_CONFIG; resp->code = RespCode::ERR_PARAM;
    resp->detail = -3;
    api_log("SET_CONFIG: invalid POCSAG address");
    return;
  }
  if (!isTxPowerValid(c.txPower)) {
    resp->cmd = CmdID::CMD_SET_CONFIG; resp->code = RespCode::ERR_PARAM;
    resp->detail = -4;
    api_log("SET_CONFIG: invalid txPower");
    return;
  }

  s_config = c;

  // 立即生效: 更新频率和功率
  Si446x_setFrequencyKHz(c.freqKHz);
  Si446x_setTxPower(c.txPower);

  resp->cmd  = CmdID::CMD_SET_CONFIG;
  resp->code = RespCode::OK;
}

// ============================================================
// Handler: TRANSMIT
// ============================================================

static Transmitter::Config makeTxConfig() {
  Transmitter::Config c;
  c.encoding = (s_config.encoding == EncodingType::POCSAG)
                   ? Transmitter::POCSAG : Transmitter::FLEX;
  c.phase    = (s_config.phase == PhaseType::NORMAL)
                   ? Transmitter::NORMAL : Transmitter::INVERTED;
  c.baudRate = s_config.baudRate;
  c.tone     = static_cast<Transmitter::Tone>(s_config.tone);
  c.address  = s_config.address;
  memcpy(c.message, s_config.message, sizeof(c.message));
  c.msgType  = (s_config.msgType == MsgType::NUMERIC)
                   ? Transmitter::NUMERIC : Transmitter::ALPHANUMERIC;
  return c;
}

static void handleTransmit(const CmdRequest* req, CmdResponse* resp) {
  // 扫描期间禁止单次发射
  if (s_sweeper && s_sweeper->isRunning()) {
    resp->cmd = CmdID::CMD_TRANSMIT; resp->code = RespCode::ERR_BUSY;
    resp->detail = -10;
    return;
  }

  if (s_pendingTx) {
    resp->cmd = CmdID::CMD_TRANSMIT; resp->code = RespCode::ERR_BUSY;
    resp->detail = -1;
    return;
  }
  if (!s_tx) {
    resp->cmd = CmdID::CMD_TRANSMIT; resp->code = RespCode::ERR_HW;
    return;
  }

  // 设置待发射标记, 由 api_loop() 在主循环中执行 (避免阻塞 async_tcp 等任务)
  s_pendingTx = true;
  if (req->transmitMsg[0] != '\0') {
    strncpy(s_pendingTxMsg, req->transmitMsg, sizeof(s_pendingTxMsg) - 1);
    s_pendingTxMsg[sizeof(s_pendingTxMsg) - 1] = '\0';
  } else {
    s_pendingTxMsg[0] = '\0';
  }

  resp->cmd    = CmdID::CMD_TRANSMIT;
  resp->code   = RespCode::OK;
  resp->detail = 0;  // 返回 0 表示已入队, 实际 count 在 TX 完成后增加
}

// ============================================================
// Handler: ABORT
// ============================================================

static void handleAbort(const CmdRequest* req, CmdResponse* resp) {
  resp->cmd  = CmdID::CMD_ABORT;
  resp->code = RespCode::OK;
}

// ============================================================
// Handler: GET_WIFI_STATUS (仅查询状态, 不触发扫描)
// ============================================================

static void handleGetWifiStatus(const CmdRequest* req, CmdResponse* resp) {
  resp->cmd = CmdID::CMD_GET_WIFI_STATUS;

  if (!s_wifi) {
    resp->code = RespCode::ERR_HW;
    resp->detail = -1;
    return;
  }

  memset(&resp->wifi, 0, sizeof(resp->wifi));
  resp->wifi.state = (uint8_t)s_wifi->state();
  resp->wifi.rssi  = -1;
  resp->code = RespCode::OK;
}

// ============================================================
// Handler: RESET
// ============================================================

static void handleReset(const CmdRequest* req, CmdResponse* resp) {
  resp->cmd    = CmdID::CMD_RESET;
  resp->code   = RespCode::OK;
  resp->detail = 0;
  // 由传输层在返回响应后调用 ESP.restart()
}

// ============================================================
// Handler: SET_WIFI (非阻塞: 设置标记, 由主循环执行)
// ============================================================

static void handleSetWiFi(const CmdRequest* req, CmdResponse* resp) {
  resp->cmd = CmdID::CMD_SET_WIFI;

  if (!s_wifi) {
    resp->code = RespCode::ERR_HW;
    resp->detail = -1;
    return;
  }
  if (req->wifiCred.ssid[0] == '\0') {
    resp->code = RespCode::ERR_PARAM;
    resp->detail = -2;
    return;
  }

  strncpy(s_pendingWifiSsid, req->wifiCred.ssid, sizeof(s_pendingWifiSsid) - 1);
  strncpy(s_pendingWifiPass, req->wifiCred.password, sizeof(s_pendingWifiPass) - 1);
  s_pendingWifiSet = true;

  resp->code = RespCode::OK;
}

// ============================================================
// Handler: SCAN_WIFI (非阻塞: 触发后台扫描, 立即返回缓存结果)
// ============================================================

static void handleScanWiFi(const CmdRequest* req, CmdResponse* resp) {
  resp->cmd = CmdID::CMD_SCAN_WIFI;

  if (!s_wifi) {
    resp->code = RespCode::ERR_HW;
    resp->detail = -1;
    return;
  }

  // 触发后台扫描 (由 api_loop 在主循环中执行)
  s_pendingScan = true;

  // 立即返回缓存结果
  memset(&resp->wifi, 0, sizeof(resp->wifi));
  resp->wifi.state = (uint8_t)s_wifi->state();
  strncpy(resp->wifi.scanResult, s_scanCache.c_str(), sizeof(resp->wifi.scanResult) - 1);
  resp->code = RespCode::OK;
}

// ============================================================
// Handler: SWEEP_START
// ============================================================

static void handleSweepStart(const CmdRequest* req, CmdResponse* resp) {
  resp->cmd = CmdID::CMD_SWEEP_START;

  if (!s_sweeper) {
    resp->code = RespCode::ERR_HW;
    resp->detail = -1;
    return;
  }
  if (s_sweeper->isRunning()) {
    resp->code = RespCode::ERR_BUSY;
    resp->detail = -2;
    return;
  }
  if (s_pendingTx) {
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

  // 构建 Sweeper 配置 — 发射参数从当前 s_config 继承
  Sweeper::Config cfg;
  cfg.mode      = (m == 0) ? Sweeper::ADDR_SWEEP : Sweeper::FREQ_SWEEP;
  cfg.addrStart = s_config.address;
  cfg.freqStartKHz = s_config.freqKHz;
  // 以下使用 Sweeper 默认常量, Config 结构体已设默认值
  // cfg.freqStepKHz / cfg.freqMaxKHz 从 Config 默认继承
  cfg.encoding  = (s_config.encoding == EncodingType::POCSAG)
                      ? 0 : 1;  // Transmitter::POCSAG/FLEX
  cfg.phase     = (s_config.phase == PhaseType::NORMAL) ? 0 : 1;
  cfg.tone      = (int)s_config.tone;
  cfg.msgType   = (s_config.msgType == MsgType::NUMERIC) ? 0 : 1;
  cfg.baudRate  = s_config.baudRate;
  cfg.txPower   = s_config.txPower;
  cfg.freqKHz   = s_config.freqKHz;
  cfg.address   = s_config.address;

  int result = s_sweeper->start(cfg);
  if (result == 0) {
    resp->code = RespCode::OK;
    const char* modeName = (cfg.mode == Sweeper::ADDR_SWEEP) ? "addr" : "freq";
    char logLine[128];
    snprintf(logLine, sizeof(logLine), "SWEEP START mode=%s start=%lu",
             modeName, (unsigned long)((cfg.mode == Sweeper::ADDR_SWEEP)
                 ? cfg.addrStart : cfg.freqStartKHz));
    api_log(logLine);
  } else {
    resp->code   = RespCode::ERR_PARAM;
    resp->detail = result;
  }
}

// ============================================================
// Handler: SWEEP_STOP
// ============================================================

static void handleSweepStop(const CmdRequest* req, CmdResponse* resp) {
  resp->cmd = CmdID::CMD_SWEEP_STOP;

  if (s_sweeper && s_sweeper->isRunning()) {
    char logLine[64];
    snprintf(logLine, sizeof(logLine), "SWEEP STOP sent=%lu",
             (unsigned long)s_sweeper->totalSent());
    api_log(logLine);
    s_sweeper->stop();
  }
  resp->code = RespCode::OK;
}

// ============================================================
// Handler: SWEEP_STATUS
// ============================================================

static void handleSweepStatus(const CmdRequest* req, CmdResponse* resp) {
  resp->cmd  = CmdID::CMD_SWEEP_STATUS;
  resp->code = RespCode::OK;

  if (s_sweeper) {
    resp->sweepStatus.running   = s_sweeper->isRunning();
    resp->sweepStatus.mode      = (uint8_t)s_sweeper->mode();
    resp->sweepStatus.current   = s_sweeper->current();
    resp->sweepStatus.totalSent = s_sweeper->totalSent();
  } else {
    memset(&resp->sweepStatus, 0, sizeof(resp->sweepStatus));
  }
}

// ============================================================
// 日志操作
// ============================================================

void api_log(const char* line) {
  if (!line) return;
  // 同步输出到串口 (方便调试)
  Serial.print(F("[LOG] "));
  Serial.println(line);

  taskENTER_CRITICAL(&s_logMux);
  strncpy(s_logBuf[s_logHead], line, LOG_LINE_LEN - 1);
  s_logBuf[s_logHead][LOG_LINE_LEN - 1] = '\0';
  s_logHead = (s_logHead + 1) % LOG_MAX_LINES;
  if (s_logCount < LOG_MAX_LINES) s_logCount++;
  taskEXIT_CRITICAL(&s_logMux);
}

void api_getLog(char* buf, size_t size) {
  if (!buf || size == 0) return;
  buf[0] = '\0';

  taskENTER_CRITICAL(&s_logMux);
  int start = (s_logCount < LOG_MAX_LINES) ? 0
              : s_logHead;
  int remaining = size - 1;
  int pos = 0;

  for (int i = 0; i < s_logCount && remaining > 2; i++) {
    int idx = (start + i) % LOG_MAX_LINES;
    size_t len = strlen(s_logBuf[idx]);
    if (len == 0) continue;
    if (pos + (int)len + 2 > (int)size - 1) break;
    if (pos > 0) { buf[pos++] = '\n'; remaining--; }
    strncpy(buf + pos, s_logBuf[idx], remaining);
    pos += len;
    remaining -= len;
  }
  buf[pos] = '\0';
  taskEXIT_CRITICAL(&s_logMux);
}

// ============================================================
// 注入函数
// ============================================================

void api_setTransmitter(Transmitter* tx) {
  s_tx = tx;
}

void api_setWiFiManager(WiFiManager* wifi) {
  s_wifi = wifi;
}

void api_setSweeper(Sweeper* sweeper) {
  s_sweeper = sweeper;
}

TxConfig api_getConfig() {
  return s_config;
}

void api_triggerScan() {
  s_pendingScan = true;
}

const char* api_getScanCache() {
  return s_scanCache.c_str();
}

// ============================================================
// Loop 调度 (由 main.cpp loop 调用)
// ============================================================

void api_loop() {
  // WiFi 后台扫描 (非阻塞: 由主循环执行)
  if (s_pendingScan && s_wifi) {
    s_pendingScan = false;
    s_scanCache = s_wifi->scanNetworks();
    api_log("wifi scan complete");
  }

  // WiFi 凭据设置 (非阻塞: 由主循环执行)
  if (s_pendingWifiSet && s_wifi) {
    s_pendingWifiSet = false;
    s_wifi->setCredentials(s_pendingWifiSsid, s_pendingWifiPass);
    api_log("wifi credentials saved");
  }

  // 待发射的单次 TX (非阻塞: 由主循环执行)
  if (s_pendingTx && s_tx) {
    char savedMsg[256] = {0};
    bool msgOverride = (s_pendingTxMsg[0] != '\0');
    if (msgOverride) {
      strncpy(savedMsg, s_config.message, sizeof(savedMsg) - 1);
      strncpy(s_config.message, s_pendingTxMsg, sizeof(s_config.message) - 1);
    }

    // 日志: 完整发送参数
    {
      const char* encName = (s_config.encoding == EncodingType::POCSAG) ? "POCSAG" : "FLEX";
      const char* phName  = (s_config.phase == PhaseType::NORMAL) ? "NRM" : "INV";
      const char* tpName  = (s_config.msgType == MsgType::NUMERIC) ? "NUM" : "ALPHA";
      char buf[256];
      char toneL = "ABCD"[(uint8_t)s_config.tone & 3];
      snprintf(buf, sizeof(buf),
               "TX> %s %s %s addr=%lu freq=%ukHz baud=%u pw=%u tone=%c \"%s\"",
               encName, phName, tpName,
               (unsigned long)s_config.address,
               s_config.freqKHz,
               s_config.baudRate,
               s_config.txPower,
               toneL,
               s_config.message);
      api_log(buf);
    }

    s_tx->send(makeTxConfig());
    s_txCount++;
    s_pendingTx = false;

    if (msgOverride) {
      strncpy(s_config.message, savedMsg, sizeof(s_config.message) - 1);
    }
  }

  // Sweeper 调度 (非阻塞)
  if (s_sweeper) s_sweeper->loop();
}

// ============================================================
// 公共调度入口
// ============================================================

static const char* cmdName(CmdID cmd) {
  switch (cmd) {
  case CmdID::CMD_PING:        return "PING";
  case CmdID::CMD_GET_STATUS:  return "STATUS";
  case CmdID::CMD_GET_CONFIG:  return "GET_CONFIG";
  case CmdID::CMD_RESET:       return "RESET";
  case CmdID::CMD_SET_CONFIG:  return "SET_CONFIG";
  case CmdID::CMD_TRANSMIT:    return "TX";
  case CmdID::CMD_ABORT:       return "ABORT";
  case CmdID::CMD_SET_WIFI:        return "SET_WIFI";
  case CmdID::CMD_SCAN_WIFI:       return "SCAN_WIFI";
  case CmdID::CMD_GET_WIFI_STATUS: return "GET_WIFI_STATUS";
  case CmdID::CMD_SWEEP_START:   return "SWEEP_START";
  case CmdID::CMD_SWEEP_STOP:    return "SWEEP_STOP";
  case CmdID::CMD_SWEEP_STATUS:  return "SWEEP_STATUS";
  default: return "UNKNOWN";
  }
}

void api_dispatch(const CmdRequest* req, CmdResponse* resp) {
  if (!req || !resp) return;

  resp->cmd  = req->cmd;
  resp->code = RespCode::ERR_UNKNOWN;
  resp->detail = 0;

  switch (req->cmd) {
  case CmdID::CMD_PING:         handlePing(req, resp);        break;
  case CmdID::CMD_GET_STATUS:   handleGetStatus(req, resp);   break;
  case CmdID::CMD_GET_CONFIG:   handleGetConfig(req, resp);   break;
  case CmdID::CMD_SET_CONFIG:   handleSetConfig(req, resp);   break;
  case CmdID::CMD_TRANSMIT:     handleTransmit(req, resp);    break;
  case CmdID::CMD_ABORT:        handleAbort(req, resp);       break;
  case CmdID::CMD_RESET:        handleReset(req, resp);       break;
  case CmdID::CMD_SET_WIFI:       handleSetWiFi(req, resp);       break;
  case CmdID::CMD_SCAN_WIFI:      handleScanWiFi(req, resp);      break;
  case CmdID::CMD_GET_WIFI_STATUS:handleGetWifiStatus(req, resp); break;
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
    api_log(logLine);
  }
}

// ============================================================
// 初始化
// ============================================================

void api_init() {
  // 加载默认配置 (后续可从 NVS 恢复)
  s_config.encoding  = EncodingType::POCSAG;
  s_config.phase     = PhaseType::NORMAL;
  s_config.tone      = ToneType::TONE_D;
  s_config.msgType   = MsgType::ALPHANUMERIC;
  s_config.freqKHz   = 152675;
  s_config.baudRate  = 1200;
  s_config.address   = 1000;
  s_config.txPower   = 127;
  strncpy(s_config.message, "BeeperTX OK", sizeof(s_config.message) - 1);

  // 将默认配置应用到 Si4463 硬件 (频率 + 功率)
  Si446x_setFrequencyKHz(s_config.freqKHz);
  Si446x_setTxPower(s_config.txPower);

  s_pendingTx = false;
  s_pendingScan = false;
  s_pendingWifiSet = false;
  s_pendingTxMsg[0] = '\0';
  s_txCount = 0;
  s_scanCache = "[]";
}
