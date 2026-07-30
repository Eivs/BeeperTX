#include "Sweeper.h"
#include "Transmitter.h"
#include "Pocsag.h"
#include "FlexEncoder.h"
#include "Si446x.h"
#include "API.h"
#include <Arduino.h>
#include <cstring>
#include <cstdio>

// ============================================================
// 构造
// ============================================================
Sweeper::Sweeper()
  : m_tx(nullptr)
  , m_mode(IDLE)
  , m_current(0)
  , m_totalSent(0)
  , m_nextTxMs(0)
{
}

// ============================================================
// 公开接口
// ============================================================
void Sweeper::setTransmitter(Transmitter* tx) {
  m_tx = tx;
}

int Sweeper::start(const Config& cfg) {
  if (!m_tx)           return -2;
  if (m_mode != IDLE)  return -1;

  if (cfg.mode == ADDR_SWEEP) {
    if (cfg.addrStart > 0x1FFFFF) return -2;  // 超出 POCSAG 21-bit 范围
  }
  if (cfg.mode == FREQ_SWEEP) {
    if (cfg.freqStartKHz < 119000 || cfg.freqStartKHz > cfg.freqMaxKHz)
      return -2;
  }

  m_cfg       = cfg;
  m_mode      = cfg.mode;
  m_current   = (cfg.mode == ADDR_SWEEP) ? cfg.addrStart : cfg.freqStartKHz;
  m_totalSent = 0;
  m_nextTxMs  = millis();  // 立即开始第一轮

  return 0;
}

void Sweeper::stop() {
  m_mode = IDLE;
}

// ============================================================
// 状态查询
// ============================================================

bool Sweeper::isAddrMax() const {
  return m_current > 0x1FFFFF;
}

bool Sweeper::isFreqMax() const {
  return m_current > m_cfg.freqMaxKHz;
}

// ============================================================
// 主循环 (由 api_loop 驱动)
// ============================================================
void Sweeper::loop() {
  if (m_mode == IDLE) return;
  if (!m_tx) { m_mode = IDLE; return; }

  // 按节拍调度: 上一批发射完成后才启动下一批
  if (millis() >= m_nextTxMs) {
    sweepStep();
  }
}

// ============================================================
// 发射步进
// ============================================================
void Sweeper::sweepStep() {
  switch (m_mode) {
    case ADDR_SWEEP:
      txAddrBatch();
      break;
    case FREQ_SWEEP:
      txFreqOnce();
      break;
    default:
      break;
  }
  // 下轮时间: 给 WiFi/串口让出 10ms 处理机会
  m_nextTxMs = millis() + BATCH_GAP_MS;
}

// ============================================================
// 追码: 批量发射
// ============================================================
void Sweeper::txAddrBatch() {
  if (m_cfg.encoding == Transmitter::POCSAG) {
    // ---- POCSAG 批量 (50 地址/批, 共用一个前导码; 8 地址子批次检查 STOP) ----
    uint32_t addrs[POCSAG_BATCH_SIZE];
    int totalCount = 0;
    for (int i = 0; i < POCSAG_BATCH_SIZE; i++) {
      uint32_t addr = m_current + i;
      if (addr > 0x1FFFFF) break;
      addrs[totalCount++] = addr;
    }

    if (totalCount == 0) {
      stop();
      return;
    }

    // 拆分子批次: 每个子批次独立编码+发射, 中间检查 STOP
    int sent = 0;
    while (sent < totalCount) {
      if (m_mode == IDLE) return;  // STOP 响应

      int sub = SUB_BATCH_SIZE;
      if (sent + sub > totalCount) sub = totalCount - sent;

      // 日志 — 显示本次子批次范围
      char logLine[128];
      snprintf(logLine, sizeof(logLine),
               "SWEEP addr batch: %lu-%lu (%d/%d addrs) POCSAG",
               (unsigned long)addrs[sent],
               (unsigned long)addrs[sent + sub - 1],
               sub, totalCount);
      api_log(logLine);

      static PocsagEncoder encoder;
      bool invert = (m_cfg.phase == Transmitter::NORMAL);
      DisplayMode dm = (m_cfg.msgType == Transmitter::NUMERIC)
                           ? MODE_NUMERIC : MODE_ALPHANUMERIC;
      int words = encoder.encodeBatch(addrs + sent, sub, m_cfg.tone, dm, invert);

      if (words <= 0) {
        stop();
        return;
      }

      Serial.print('.');
      m_tx->sendPocsagRaw(encoder.getBaseband(), words, m_cfg.baudRate);
      Serial.println(F(" OK"));

      sent += sub;
      m_totalSent += sub;
    }

    m_current = addrs[totalCount - 1] + 1;

    if (isAddrMax()) {
      api_log("SWEEP addr complete");
      stop();
    }

  } else {
    // ---- FLEX 批量 (15 地址/批, 逐地址编码发射, 中间检查 STOP) ----
    int attempted = 0;
    int count = 0;

    for (int i = 0; i < FLEX_BATCH_SIZE; i++) {
      if (m_mode == IDLE) return;  // STOP 响应

      uint32_t addr = m_current + i;
      if (addr > 0x1FFFFF) break;
      attempted++;

      char msg[16];
      snprintf(msg, sizeof(msg), "%lu", (unsigned long)addr);

      FlexEncoder encoder;
      FlexEncoder::MsgType mt = (m_cfg.msgType == Transmitter::NUMERIC)
                                    ? FlexEncoder::NUMERIC : FlexEncoder::ALPHA;
      size_t len = encoder.encode(addr, msg, mt);

      if (len == 0 || encoder.hasError()) {
        continue;
      }

      char logLine[128];
      snprintf(logLine, sizeof(logLine),
               "SWEEP addr=%lu FLEX", (unsigned long)addr);
      api_log(logLine);

      Serial.print('.');
      bool invert = (m_cfg.phase == Transmitter::NORMAL);
      m_tx->sendFlexRaw(encoder.getData(), len, m_cfg.baudRate, invert);
      Serial.println(F(" OK"));

      count++;
      m_totalSent++;
    }

    m_current += attempted;

    if (isAddrMax()) {
      api_log("SWEEP addr complete");
      stop();
    }
  }
}

// ============================================================
// 追频: 单次发射 (不批量, 每次完整发送)
// ============================================================
void Sweeper::txFreqOnce() {
  // 检查频率上限
  if (isFreqMax()) {
    api_log("SWEEP freq complete");
    stop();
    return;
  }

  // 设置新频率
  Si446x_setFrequencyKHz(m_current);
  // 短暂等待 PLL 锁定 (Si4463 锁定时序 ~200µs, 1ms 有足够余量)
  delay(1);

  // 构建 Config — 消息为频率值 (kHz 数值, 不含小数点)
  Transmitter::Config cfg;
  cfg.encoding = static_cast<Transmitter::Encoding>(m_cfg.encoding);
  cfg.phase    = static_cast<Transmitter::Phase>(m_cfg.phase);
  cfg.tone     = static_cast<Transmitter::Tone>(m_cfg.tone);
  cfg.baudRate = m_cfg.baudRate;
  cfg.address  = m_cfg.address;
  cfg.msgType  = static_cast<Transmitter::MsgType>(m_cfg.msgType);

  // 消息 = 频率值 (如 "152675")
  char freqStr[16];
  snprintf(freqStr, sizeof(freqStr), "%lu", (unsigned long)m_current);
  strncpy(cfg.message, freqStr, sizeof(cfg.message) - 1);
  cfg.message[sizeof(cfg.message) - 1] = '\0';

  char logLine[128];
  snprintf(logLine, sizeof(logLine),
           "SWEEP freq=%lukHz addr=%lu",
           (unsigned long)m_current, (unsigned long)cfg.address);
  api_log(logLine);

  m_tx->send(cfg);

  m_totalSent++;
  m_current += m_cfg.freqStepKHz;

  if (isFreqMax()) {
    api_log("SWEEP freq complete");
    stop();
  }
}
