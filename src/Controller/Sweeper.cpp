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
  , m_total(0)
  , m_done(0)
  , m_origFreqKHz(0)
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
    if (cfg.addrStart > cfg.addrEnd)           return -2;  // 起始 > 上限
    if (cfg.addrEnd > 0x1FFFFF)                return -2;  // 超出 POCSAG 21-bit
  }
  if (cfg.mode == FREQ_SWEEP) {
    if (cfg.freqStartKHz < 119000)             return -2;  // 低于硬件下限
    if (cfg.freqStartKHz > cfg.freqMaxKHz)     return -2;
  }

  m_cfg       = cfg;
  m_mode      = cfg.mode;
  m_current   = (cfg.mode == ADDR_SWEEP) ? cfg.addrStart : cfg.freqStartKHz;
  m_done      = 0;
  m_origFreqKHz = (cfg.mode == FREQ_SWEEP) ? cfg.freqStartKHz : 0;
  m_nextTxMs  = millis();  // 立即开始第一轮

  if (!calcTotalSteps()) {
    m_mode = IDLE;
    return -2;
  }

  return 0;
}

void Sweeper::stop() {
  if (m_mode == IDLE) return;
  restoreFreqIfNeeded();
  m_mode = IDLE;
}

// ============================================================
// 状态查询
// ============================================================

Sweeper::Status Sweeper::getStatus() const {
  Status s;
  s.running = (m_mode != IDLE);
  s.mode    = (uint8_t)m_mode;
  s.current = m_current;
  s.total   = m_total;
  s.done    = m_done;
  return s;
}

bool Sweeper::calcTotalSteps() {
  if (m_mode == ADDR_SWEEP) {
    if (m_cfg.addrStart > m_cfg.addrEnd) return false;
    m_total = m_cfg.addrEnd - m_cfg.addrStart + 1;
    return true;
  }
  if (m_mode == FREQ_SWEEP) {
    if (m_cfg.freqStepKHz == 0) return false;
    m_total = (m_cfg.freqMaxKHz - m_cfg.freqStartKHz) / m_cfg.freqStepKHz + 1;
    return true;
  }
  return false;
}

bool Sweeper::isAddrMax() const {
  return m_current > m_cfg.addrEnd;
}

bool Sweeper::isFreqMax() const {
  return m_current > m_cfg.freqMaxKHz;
}

void Sweeper::restoreFreqIfNeeded() {
  // 追频结束/停止时, 把射频频率恢复为起始值 (避免下次手动发射落错频率)
  if (m_mode == FREQ_SWEEP && m_origFreqKHz != 0 &&
      m_current != m_origFreqKHz) {
    Si446x_setFrequencyKHz(m_origFreqKHz);
    char logLine[64];
    snprintf(logLine, sizeof(logLine), "SWEEP freq restored to %lukHz",
             (unsigned long)m_origFreqKHz);
    api_log(logLine);
  }
}

// ============================================================
// 主循环 (由 main.cpp loop 驱动)
// ============================================================
void Sweeper::loop() {
  if (m_mode == IDLE) return;
  if (!m_tx) { m_mode = IDLE; return; }

  // 范围完成自动停止 (规格 §7.2)
  if (m_done >= m_total) {
    stop();
    api_log("SWEEP complete");
    return;
  }

  // 按节拍调度: 批次间 intervalMs 间隔 (规格 §7.3)
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
  // 下一批时间: intervalMs (至少 BATCH_GAP_MS 保证 WiFi/串口处理机会)
  uint32_t gap = m_cfg.intervalMs;
  if (gap < BATCH_GAP_MS) gap = BATCH_GAP_MS;
  m_nextTxMs = millis() + gap;
}

// ============================================================
// 追码: 批量发射
// ============================================================
void Sweeper::txAddrBatch() {
  if (m_cfg.encoding == Transmitter::POCSAG) {
    // ---- POCSAG 批量 (POCSAG_BATCH_SIZE 地址/批, 共享一个前导码; 8 地址/组) ----
    // 消息 = 地址数字串, 固定 NUMERIC 编码 (规格 §1.2 纯数字消息)。
    // 批量仅支持 ≤5 位数字 (1 CW) 且批内帧号不冲突; 否则降级逐地址标准发射。
    uint32_t addrs[POCSAG_BATCH_SIZE];
    int totalCount = 0;
    for (int i = 0; i < POCSAG_BATCH_SIZE; i++) {
      uint32_t addr = m_current + i;
      if (addr > m_cfg.addrEnd) break;
      addrs[totalCount++] = addr;
    }

    if (totalCount == 0) {
      stop();
      return;
    }

    // 拆分子批次: 每个子批次独立编码+发射, 中间检查 STOP
    PocsagEncoder& encoder = m_batchEncoder;
    bool invert = (m_cfg.phase == Transmitter::NORMAL);

    int sent = 0;
    while (sent < totalCount) {
      if (m_mode == IDLE) return;  // STOP 响应

      int sub = SUB_BATCH_SIZE;
      if (sent + sub > totalCount) sub = totalCount - sent;

      int addrEncoded = 0;
      int words = encoder.encodeBatch(addrs + sent, sub, m_cfg.tone,
                                      MODE_NUMERIC, invert, &addrEncoded);

      if (words > 0 && addrEncoded == sub) {
        // 批量发射成功
        char logLine[128];
        snprintf(logLine, sizeof(logLine),
                 "SWEEP addr batch: %lu-%lu (%d addrs) POCSAG",
                 (unsigned long)addrs[sent],
                 (unsigned long)addrs[sent + sub - 1], sub);
        api_log(logLine);

        Serial.print('.');
        m_tx->sendPocsagRaw(encoder.getBaseband(), words, m_cfg.baudRate);
        Serial.println(F(" OK"));

        sent += sub;
        m_done += sub;
      } else {
        // 降级: 消息 >5 位或帧冲突, 逐地址标准发射 (支持任意位数/字母模式)
        for (int i = 0; i < sub; i++) {
          if (m_mode == IDLE) return;  // STOP 响应

          Transmitter::Config cfg;
          cfg.encoding = Transmitter::POCSAG;
          cfg.phase    = (Transmitter::Phase)m_cfg.phase;
          cfg.tone     = (Transmitter::Tone)m_cfg.tone;
          cfg.baudRate = m_cfg.baudRate;
          cfg.address  = addrs[sent + i];
          cfg.msgType  = (m_cfg.msgType == Transmitter::NUMERIC)
                             ? Transmitter::NUMERIC : Transmitter::ALPHANUMERIC;
          snprintf(cfg.message, sizeof(cfg.message), "%lu",
                   (unsigned long)addrs[sent + i]);

          char logLine[128];
          snprintf(logLine, sizeof(logLine),
                   "SWEEP addr=%lu POCSAG deg", (unsigned long)addrs[sent + i]);
          api_log(logLine);

          m_tx->send(cfg);
          m_done++;
        }
        sent += sub;
      }
    }

    m_current = addrs[totalCount - 1] + 1;

    if (isAddrMax()) {
      api_log("SWEEP addr complete");
      stop();
    }

  } else {
    // ---- FLEX 逐地址发射 (规格 §5.2: FLEX 每批 1 地址, 帧长 ~1.9s, 不批量) ----
    if (m_mode == IDLE) return;  // STOP 响应

    uint32_t addr = m_current;
    if (addr > m_cfg.addrEnd) {
      api_log("SWEEP addr complete");
      stop();
      return;
    }

    char msg[16];
    snprintf(msg, sizeof(msg), "%lu", (unsigned long)addr);

    FlexEncoder encoder;
    FlexEncoder::MsgType mt = (m_cfg.msgType == Transmitter::NUMERIC)
                                  ? FlexEncoder::NUMERIC : FlexEncoder::ALPHA;
    size_t len = encoder.encode(addr, msg, mt);

    if (len > 0 && !encoder.hasError()) {
      char logLine[128];
      snprintf(logLine, sizeof(logLine),
               "SWEEP addr=%lu FLEX", (unsigned long)addr);
      api_log(logLine);

      Serial.print('.');
      bool invert = (m_cfg.phase == Transmitter::NORMAL);
      m_tx->sendFlexRaw(encoder.getData(), len, m_cfg.baudRate, invert);
      Serial.println(F(" OK"));
      m_done++;
    }

    m_current = addr + 1;

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

  // 设置新频率 (此时 Si4463 处于 IDLE, 规格 §13.3)
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

  m_done++;
  m_current += m_cfg.freqStepKHz;

  if (isFreqMax()) {
    api_log("SWEEP freq complete");
    stop();
  }
}
