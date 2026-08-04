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
  m_batchBase = 0;
  m_batchSize = 0;
  m_batchLeft = 0;
  m_origFreqKHz = (cfg.mode == FREQ_SWEEP) ? cfg.freqStartKHz : 0;
  m_nextTxMs  = millis();  // 立即开始第一轮
  m_tx->clearAbort();      // 清除上次 STOP 遗留的中断标志

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
  if (m_tx) m_tx->abortTx();  // 中断当前发射 (半字节精度的秒级停发)
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
  // 当前批次区间 (UI 显示用): 批量进行中显示实际批; 否则单地址 = current
  if (m_mode == ADDR_SWEEP && m_batchSize > 0) {
    s.batchStart = m_batchBase;
    s.batchEnd   = m_batchBase + m_batchSize - 1;
  } else {
    s.batchStart = m_current;
    s.batchEnd   = m_current;
  }
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

  // 异步门控: txTask 正在发射/有积压时暂停发起下一批。
  // 保证 Si4463 SPI 独占 (追频 setFrequency 不会与 Direct TX 冲突),
  // 且追码追频队列最多积压有限批次, STOP 后残余最多 1-2 批。
  if (m_tx->isBusy()) return;

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
  if (m_cfg.encoding == EncodingType::POCSAG) {
    // ---- POCSAG 批量 ----
    // 消息格式必须与功能码匹配 (POCSAG 标准): 寻呼机按功能码解码消息,
    // 字母功能码 (A/B/C) 收到 BCD 数字消息时按字母解 → 控制字符 → 空消息
    // (用户实测"追码收到空消息"根因)。encodeBatchMulti 按用户 msgType 编码
    // (NUMERIC=BCD / ALPHANUMERIC=ASCII), 消息 = 地址十进制串, 单前导码
    // 连续多 Batch 调度, 消息码字跨 SYNC 延续 (ITU-R M.584), 全地址进批量。
    if (m_mode == IDLE) return;  // STOP 响应

    if (m_batchLeft == 0) {
      m_batchBase = m_current;
      m_batchSize = 0;
      for (int i = 0; i < POCSAG_BATCH_SIZE; i++) {
        uint32_t a = m_batchBase + i;
        if (a > m_cfg.addrEnd) break;
        m_batchSize++;
      }
      if (m_batchSize == 0) {
        stop();
        return;
      }
      m_batchLeft = m_batchSize;
    }

    PocsagEncoder& encoder = m_batchEncoder;
    bool invert = (m_cfg.phase == PhaseType::NORMAL);
    DisplayMode dm = (m_cfg.msgType == MsgType::NUMERIC)
                         ? MODE_NUMERIC : MODE_ALPHANUMERIC;

    // 整批一次入队 (55 地址 ≤ encodeBatchMulti 的 MAX_BATCH, 恒可编码)
    int sub = (int)m_batchLeft;
    for (int i = 0; i < sub; i++) {
      m_batchAddrs[i] = m_batchBase + i;
    }

    int addrEncoded = 0;
    int words = encoder.encodeBatchMulti(m_batchAddrs, sub,
                                         static_cast<int>(m_cfg.tone), dm,
                                         invert, &addrEncoded);
    if (words > 0) {
      if (!m_tx->sendPocsagRaw(encoder.getBaseband(), words, m_cfg.baudRate)) {
        return;  // 队列满: 下帧重试 (m_batchLeft 未推进)
      }
      char logLine[128];
      snprintf(logLine, sizeof(logLine),
               "SWEEP addr batch: %lu-%lu (%d/%d addrs) POCSAG",
               (unsigned long)m_batchAddrs[0],
               (unsigned long)m_batchAddrs[sub - 1],
               addrEncoded, sub);
      api_log(logLine);

      Serial.print('.');
      Serial.println(F(" OK"));
    } else {
      // 理论不可达 (55 地址恒可编码): 防御推进, 避免卡死
      api_log("SWEEP addr batch encode failed");
    }
    m_batchLeft -= sub;
    m_done += (uint32_t)addrEncoded;

    m_current = m_batchBase + m_batchSize;
    m_batchBase = 0;
    m_batchSize = 0;
    if (isAddrMax()) {
      api_log("SWEEP addr complete");
      stop();
    }

  } else {
    // ---- FLEX 批量 (追码优化): 一次 ERS + 连续多帧 (frame=0), 帧内多地址多消息 ----
    // v1 仅短地址 (capcode 1..1933312, 7 位内), 消息 = 地址十进制串。
    // 55 地址 ALPHA 批 ≈ ERS 420 B + 4 帧×375 B = 1920 B, 一次入队,
    // 对比旧逐地址 (每地址 ERS+1 帧 ≈ 4.0 s) 提速 ~23×。
    // 非短地址 (或编码失败) 退化为逐地址发射, 避免卡死。
    if (m_mode == IDLE) return;  // STOP 响应

    uint32_t addr = m_current;
    if (addr > m_cfg.addrEnd) {
      api_log("SWEEP addr complete");
      stop();
      return;
    }

    // 组装连续短地址子批 (遇非短地址 / 超上限即停)
    size_t n = 0;
    uint32_t a = addr;
    while (n < (size_t)FLEX_BATCH_SIZE && a <= m_cfg.addrEnd &&
           FlexEncoder::isShortCapcode(a)) {
      // FLEX ALPHA: prepend SI (0x0F) to switch Chinese pagers from
      // default GB2312 mode to ASCII mode, so address digits display
      // correctly instead of garbage like "撤背".
      if (m_cfg.msgType != MsgType::NUMERIC) {
        m_flexMsgText[n][0] = '\x0F';
        snprintf(m_flexMsgText[n] + 1, 15, "%lu", (unsigned long)a);
      } else {
        snprintf(m_flexMsgText[n], sizeof(m_flexMsgText[n]), "%lu",
                 (unsigned long)a);
      }
      m_flexBatch[n].capcode = a;
      m_flexBatch[n].msg     = m_flexMsgText[n];
      m_flexBatch[n].type    = (m_cfg.msgType == MsgType::NUMERIC)
                                   ? FlexEncoder::NUMERIC : FlexEncoder::ALPHA;
      n++;
      a++;
    }

    if (n == 0) {
      // 非短地址 (超出 FLEX 短地址域): 逐地址发射, 编码失败也推进
      char msg[16];
      if (m_cfg.msgType != MsgType::NUMERIC) {
        msg[0] = '\x0F';
        snprintf(msg + 1, 15, "%lu", (unsigned long)addr);
      } else {
        snprintf(msg, sizeof(msg), "%lu", (unsigned long)addr);
      }
      FlexEncoder::MsgType mt = (m_cfg.msgType == MsgType::NUMERIC)
                                    ? FlexEncoder::NUMERIC : FlexEncoder::ALPHA;
      size_t len = m_flexEncoder.encode(addr, msg, mt);
      if (len > 0 && !m_flexEncoder.hasError()) {
        bool invert = (m_cfg.phase == PhaseType::NORMAL);
        if (!m_tx->sendFlexRaw(m_flexEncoder.getData(), len, m_cfg.baudRate,
                               invert)) {
          return;  // 队列满: 下帧重试 (m_current 未推进)
        }
        m_done++;
        m_current = addr + 1;
      } else {
        m_current = addr + 1;
      }
      if (isAddrMax()) {
        api_log("SWEEP addr complete");
        stop();
      }
      return;
    }

    size_t encoded = 0;
    size_t len = m_flexEncoder.encodeBatch(m_flexBatch, n, &encoded);
    if (len > 0 && !m_flexEncoder.hasError() && encoded > 0) {
      char logLine[128];
      snprintf(logLine, sizeof(logLine), "SWEEP addr=%lu..%lu FLEX batch",
               (unsigned long)addr, (unsigned long)(addr + encoded - 1));
      api_log(logLine);

      Serial.print('.');
      bool invert = (m_cfg.phase == PhaseType::NORMAL);
      if (!m_tx->sendFlexRaw(m_flexEncoder.getData(), len, m_cfg.baudRate,
                             invert)) {
        return;  // 队列满: 下帧重试 (m_current 未推进)
      }
      Serial.println(F(" OK"));
      m_done += (uint32_t)encoded;
      m_current = addr + (uint32_t)encoded;
    } else {
      // 编码失败 (理论不可达: 短消息恒可编码): 防御推进, 避免卡死
      m_current = addr + 1;
    }

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

  // 设置新频率 (此时 txTask 空闲 — loop() 的 isBusy 门控保证, 规格 §13.3)
  Si446x_setFrequencyKHz(m_current);
  // 短暂等待 PLL 锁定 (Si4463 锁定时序 ~200µs, 1ms 有足够余量)
  delay(1);

  // 构建 Config — 消息为频率值 (kHz 数值, 不含小数点)
  Transmitter::Config cfg;
  cfg.encoding = m_cfg.encoding;
  cfg.phase    = m_cfg.phase;
  cfg.tone     = m_cfg.tone;
  cfg.baudRate = m_cfg.baudRate;
  cfg.address  = m_cfg.address;
  cfg.msgType  = m_cfg.msgType;

  // 消息 = 频率值 (如 "152675")
  char freqStr[16];
  snprintf(freqStr, sizeof(freqStr), "%lu", (unsigned long)m_current);
  strncpy(cfg.message, freqStr, sizeof(cfg.message) - 1);
  cfg.message[sizeof(cfg.message) - 1] = '\0';

  if (!m_tx->send(cfg)) return;  // 队列满: 下帧重试 (m_current 未推进)

  char logLine[128];
  snprintf(logLine, sizeof(logLine),
           "SWEEP freq=%lukHz addr=%lu",
           (unsigned long)m_current, (unsigned long)cfg.address);
  api_log(logLine);

  m_done++;
  m_current += m_cfg.freqStepKHz;

  if (isFreqMax()) {
    api_log("SWEEP freq complete");
    stop();
  }
}
