#include "Transmitter.h"
#include "Pocsag.h"
#include "FlexEncoder.h"
#include "Si446x.h"
#include "Charset.h"
#include <Arduino.h>
#include "esp_task_wdt.h"

// ============================================================
// 后台发射任务 (core 1) — 位敲击移出 loopTask
// ============================================================

bool Transmitter::ensureTask() {
  if (m_queue) return true;
  m_queue = xQueueCreate(3, sizeof(Job));
  if (!m_queue) return false;
  if (xTaskCreatePinnedToCore(txTaskEntry, "txTask", 8192, this, 3, &m_task, 1) != pdPASS) {
    vQueueDelete(m_queue);
    m_queue = nullptr;
    return false;
  }
  return true;
}

void Transmitter::txTaskEntry(void* arg) {
  static_cast<Transmitter*>(arg)->txTaskMain();
}

void Transmitter::txTaskMain() {
  Job job;
  for (;;) {
    if (!xQueueReceive(m_queue, &job, portMAX_DELAY)) continue;

    esp_task_wdt_add(NULL);
    m_txActive = true;
    switch (job.type) {
      case JobType::RAW_POCSAG: sendPocsagRawImpl(job.pocsag.words, job.pocsag.wordCount, job.baudRate); break;
      case JobType::RAW_FLEX:   sendFlexRawImpl(job.flex.data, job.flex.len, job.baudRate, job.invert); break;
    }
    m_txActive = false;
    esp_task_wdt_delete(NULL);
  }
}

void Transmitter::abortTx() {
  m_abort = true;
  if (m_queue) xQueueReset(m_queue);
}

bool Transmitter::enqueue(Job& job) {
  if (!ensureTask()) return false;
  return xQueueSend(m_queue, &job, 0) == pdPASS;
}

bool Transmitter::isBusy() const {
  if (!m_queue) return false;
  return uxQueueMessagesWaiting(m_queue) > 0 || m_txActive;
}

void Transmitter::feedWatchdog() {
  esp_task_wdt_reset();
}

// ============================================================
// POCSAG 协议常量
// ============================================================
static const int TX_SETTLE_MS   = 2;
static const int SI446X_TX_RECOVER_MS = 20;

// ============================================================
// 构造
// ============================================================
Transmitter::Transmitter(int txPin)
  : m_txPin(txPin) {
  pinMode(m_txPin, OUTPUT);
  digitalWrite(m_txPin, LOW);
}

// ============================================================
// 静态工具
// ============================================================
int Transmitter::bitTimeUs(int baudRate) {
  switch (baudRate) {
    case 512:   return 1953;
    case 1200:  return 833;
    case 1600:  return 625;
    case 2400:  return 417;
    case 3200:  return 312;
    case 6400:  return 156;
    default:    return 833;
  }
}

int Transmitter::preambleBits(int /*baudRate*/) {
  return 576;  // POCSAG 标准
}

// ============================================================
// Direct TX 模式控制
// ============================================================
void Transmitter::enterTx() {
  Si446x_startDirectTX(0, 1);
  delay(TX_SETTLE_MS);
}

void Transmitter::exitTx() {
  Si446x_stopTX();
  delay(SI446X_TX_RECOVER_MS);
}

// ============================================================
// 低层位流发送 (忙等待, 精确时序)
// ============================================================

// 忙等到指定时刻。检查 abort 标志实现秒级停发;
// 若中断被意外禁用, CPU cycle counter 自救恢复。
static inline bool busyWaitUntil(uint64_t next, volatile bool& abort) {
  uint32_t cycMark = esp_cpu_get_ccount();
  while (esp_timer_get_time() < next) {
    if (abort) return false;
    if ((uint32_t)(esp_cpu_get_ccount() - cycMark) > 2400000UL) {
      cycMark = esp_cpu_get_ccount();
      portENABLE_INTERRUPTS();
    }
  }
  return true;
}

void Transmitter::sendCodewords(const unsigned long* cw, int count, int bitTimeUs) {
  uint64_t next = esp_timer_get_time();
  int wdtCnt = 0;
  for (int i = 0; i < count; i++) {
    if (m_abort) return;
    unsigned long word = cw[i];
    if (word == 0) break;
    for (int bit = 31; bit >= 0; bit--) {
      digitalWrite(m_txPin, ((word >> bit) & 1) ? HIGH : LOW);
      next += bitTimeUs;
      if (!busyWaitUntil(next, m_abort)) return;
      if (++wdtCnt >= 500) { wdtCnt = 0; feedWatchdog(); }
    }
  }
}

void Transmitter::sendBytes(const uint8_t* data, size_t len, int bitTimeUs, bool invert) {
  uint64_t next = esp_timer_get_time();
  int wdtCnt = 0;
  for (size_t i = 0; i < len; i++) {
    if (m_abort) return;
    uint8_t byte = invert ? ~data[i] : data[i];
    for (int bit = 7; bit >= 0; bit--) {
      digitalWrite(m_txPin, ((byte >> bit) & 1) ? HIGH : LOW);
      next += bitTimeUs;
      if (!busyWaitUntil(next, m_abort)) return;
      if (++wdtCnt >= 500) { wdtCnt = 0; feedWatchdog(); }
    }
  }
}

// ============================================================
// 公开 API — 统一入口 (预编码 + 异步入队)
// ============================================================
bool Transmitter::send(const Config& cfg) {
  clearAbort();

  // ---- FLEX: 预编码 → sendFlexRaw ----
  if (cfg.encoding == EncodingType::FLEX) {
    FlexEncoder encoder;
    size_t len = 0;
    bool isNum = (cfg.msgType == MsgType::NUMERIC);

    if (isNum) {
      len = encoder.encode(cfg.address, cfg.message, FlexEncoder::NUMERIC);
    } else {
      uint8_t* mixed = m_flexMixed;
      String gbStr = GB.get(String(cfg.message));
      size_t gbLen = gbStr.length();
      size_t m = 0;
      bool inChinese = true;
      for (size_t i = 0; i < gbLen && m < sizeof(m_flexMixed) - 8; i++) {
        uint8_t byte = (uint8_t)gbStr[i];
        if (byte >= 0x80) {
          if (!inChinese) { mixed[m++] = 0x0E; inChinese = true; }
          mixed[m++] = byte;
        } else {
          if (inChinese) { mixed[m++] = 0x0F; inChinese = false; }
          mixed[m++] = byte;
        }
      }
      mixed[m] = '\0';
      len = encoder.encode(cfg.address, (const char*)mixed, FlexEncoder::ALPHA);
    }

    if (encoder.hasError()) {
      Serial.printf("FLEX encode error: %s\n", encoder.getErrorString());
      m_txCount++;
      return false;
    }
    bool invert = (cfg.phase == PhaseType::NORMAL);
    return sendFlexRaw(encoder.getData(), len, cfg.baudRate, invert);
  }

  // ---- POCSAG: 预编码 → sendPocsagRaw (与 FLEX 对称) ----
  PocsagEncoder& encoder = m_pocsagEncoder;
  bool invert = (cfg.phase == PhaseType::NORMAL);

  String gbStr;
  const uint8_t* data = (const uint8_t*)cfg.message;
  size_t dataLen = strlen(cfg.message);

  if (cfg.msgType == MsgType::ALPHANUMERIC) {
    gbStr = GB.get(String(cfg.message));
    dataLen = gbStr.length();
    data = (const uint8_t*)gbStr.c_str();
  }

  DisplayMode dm = (cfg.msgType == MsgType::NUMERIC) ? MODE_NUMERIC : MODE_ALPHANUMERIC;
  encoder.encode(cfg.address, static_cast<int>(cfg.tone), data, dataLen, dm, invert);

  const char toneLetter = "ABCD"[static_cast<int>(cfg.tone) & 3];
  const char* modeName = (cfg.msgType == MsgType::NUMERIC) ? "NUM" : "ALPHA";
  const char* phaseName = invert ? "INV" : "NRM";
  Serial.printf("TX[%d][POCSAG] addr=%llu tone=%c baud=%d %s %s \"%s\"\n",
                m_txCount, cfg.address, toneLetter, cfg.baudRate,
                modeName, phaseName, cfg.message);

  return sendPocsagRaw(encoder.getBaseband(), encoder.getBasebandLength(), cfg.baudRate);
}

// ============================================================
// 原始发送 — Sweeper / send() 预编码路径
// ============================================================
bool Transmitter::sendPocsagRaw(const unsigned long* baseband, int wordCount, int baudRate) {
  if (!baseband || wordCount <= 0 || wordCount > JOB_MAX_POCSAG_WORDS) return false;
  Job job;
  job.type     = JobType::RAW_POCSAG;
  job.baudRate = baudRate;
  for (int i = 0; i < wordCount; i++) job.pocsag.words[i] = baseband[i];
  job.pocsag.wordCount = wordCount;
  return enqueue(job);
}

bool Transmitter::sendFlexRaw(const uint8_t* data, size_t len,
                               int baudRate, bool invert) {
  if (!data || len == 0 || len > JOB_MAX_FLEX_BYTES) return false;
  Job job;
  job.type     = JobType::RAW_FLEX;
  job.baudRate = baudRate;
  job.invert   = invert;
  memcpy(job.flex.data, data, len);
  job.flex.len = len;
  return enqueue(job);
}

// ---- 任务侧实际发送 (txTask 上下文) ----
void Transmitter::sendPocsagRawImpl(const unsigned long* baseband, int wordCount,
                                     int baudRate) {
  if (m_abort) return;
  int bt = bitTimeUs(baudRate);
  Serial.print('.');
  enterTx();
  sendCodewords(baseband, wordCount, bt);
  exitTx();
  Serial.println(" OK");
  m_txCount++;
}

void Transmitter::sendFlexRawImpl(const uint8_t* data, size_t len,
                                   int baudRate, bool invert) {
  if (m_abort) return;
  int bt = bitTimeUs(baudRate);
  Serial.printf("TX[%d][FLEX] len=%u baud=%d %s\n",
                m_txCount, (unsigned)len, baudRate, invert ? "INV" : "NRM");
  Serial.print('.');
  enterTx();
  sendBytes(data, len, bt, invert);
  exitTx();
  if (m_abort) {
    Serial.println(" ABORTED");
  } else {
    Serial.println(" OK");
    m_txCount++;
  }
}
