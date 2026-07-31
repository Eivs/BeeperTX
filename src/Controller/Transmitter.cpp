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
  // 深度 3: 允许少量积压 (Sweeper 连续批次), 同时防止无界排队。
  m_queue = xQueueCreate(3, sizeof(Job));
  if (!m_queue) return false;
  // core 1: 位敲击独占该核不被抢占 (与 loopTask 原同核, 但业务
  // loop 已迁至 core 0); core 0 (WiFi/AsyncTCP/Web) 完全不受影响。
  if (xTaskCreatePinnedToCore(txTaskEntry, "txTask", 4096, this, 3, &m_task, 1) != pdPASS) {
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
  // 注册到 Task WDT: 发射期间 txTask 独占 core 1 (抢占 loopTask),
  // loopTask 的 WDT 订阅已在 main.cpp setup() 中移除; 本任务接管
  // "长发射不触发复位" 的喂狗职责, 同时保留系统级安全网
  // (txTask 卡死时 WDT 仍会复位系统)。
  esp_task_wdt_add(NULL);

  Job job;
  while (xQueueReceive(m_queue, &job, portMAX_DELAY)) {
    m_txActive = true;  // isBusy 门控: 发射期间让 Sweeper 暂停发起下一批
    switch (job.type) {
      case JobType::POCSAG:     sendPocsag(job.cfg); break;
      case JobType::RAW_POCSAG: sendPocsagRawImpl(job.pocsag.words, job.pocsag.wordCount, job.baudRate); break;
      case JobType::RAW_FLEX:   sendFlexRawImpl(job.flex.data, job.flex.len, job.baudRate, job.invert); break;
    }
    m_txActive = false;
  }
}

bool Transmitter::enqueue(Job& job) {
  // 非阻塞入队 (深拷贝)。队列满返回 false, 调用方下帧重试。
  if (!ensureTask()) return false;
  return xQueueSend(m_queue, &job, 0) == pdPASS;
}

bool Transmitter::isBusy() const {
  // 队列有积压 或 txTask 正在发射 → 调用方 (Sweeper/API) 应等待。
  // 保证追频路径的 Si446x_setFrequencyKHz 不会与 txTask 的 Direct TX 冲突。
  if (!m_queue) return false;
  return uxQueueMessagesWaiting(m_queue) > 0 || m_txActive;
}

void Transmitter::feedWatchdog() {
  // 喂当前任务 (txTask) 的 WDT — 发射独占 core 1, 由本任务维护安全网。
  esp_task_wdt_reset();
}

// ============================================================
// POCSAG 协议常量
// ============================================================
static const int PREAMBLE_BITS  = 576;   // POCSAG 标准前导码
static const int TX_SETTLE_MS   = 2;  // Direct TX PLL 锁定等待 (Si4463 典型 200µs, 2ms 含 10 倍余量)

// ============================================================
// 构造 / 析构
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
  // bitTime = 1,000,000 / baudRate (µs)
  // 查表避免浮点并处理不可整除的情况
  switch (baudRate) {
    case 512:   return 1953;  // 1,000,000 / 512 ≈ 1953
    case 1200:  return 833;   // 1,000,000 / 1200 ≈ 833
    case 1600:  return 625;   // 1,000,000 / 1600 = 625
    case 2400:  return 417;   // 1,000,000 / 2400 ≈ 417
    case 3200:  return 312;   // 1,000,000 / 3200 = 312.5 → 312
    case 6400:  return 156;   // 1,000,000 / 6400 = 156.25 → 156
    default:    return 833;   // fallback: 1200 bps
  }
}

int Transmitter::preambleBits(int /*baudRate*/) {
  // POCSAG 标准: 至少 576 位前导码
  return PREAMBLE_BITS;
}

// ============================================================
// Direct TX 模式控制
// ============================================================
void Transmitter::enterTx() {
  Si446x_startDirectTX(0, 1);
  delay(TX_SETTLE_MS);  // 等待 PLL 锁定
}

void Transmitter::exitTx() {
  Si446x_stopTX();
}

// ============================================================
// 低层位流发送 (忙等待, 精确时序)
// ============================================================

/**
 * 发送 POCSAG 前导码: 交替 1/0 模式.
 *   正相(invert=false): 1, 0, 1, 0, ...
 *   反相(invert=true):  0, 1, 0, 1, ...
 */
void Transmitter::sendPreambleBits(int bits, int bitTimeUs, bool invert) {
  if (bits <= 0) return;
  uint64_t next = esp_timer_get_time();
  int wdtCnt = 0;
  for (int i = 0; i < bits; i++) {
    int bit = (i & 1) ? 0 : 1;   // 正相
    if (invert) bit = 1 - bit;    // 反相
    digitalWrite(m_txPin, bit ? HIGH : LOW);
    next += bitTimeUs;
    while (esp_timer_get_time() < next) {}
    // 每 500 位喂一次看门狗, 防止长帧触发 WDT (喂 loopTask, 因发射任务抢占它)
    if (++wdtCnt >= 500) { wdtCnt = 0; feedWatchdog(); }
  }
}

/**
 * 发送 POCSAG 基带码字 (32-bit, MSB first).
 * 码字已由 PocsagEncoder 处理过相位 (invert), 此处直接送 GPIO.
 */
void Transmitter::sendCodewords(const unsigned long* cw, int count, int bitTimeUs) {
  uint64_t next = esp_timer_get_time();
  int wdtCnt = 0;
  for (int i = 0; i < count; i++) {
    unsigned long word = cw[i];
    if (word == 0) break;
    for (int bit = 31; bit >= 0; bit--) {
      digitalWrite(m_txPin, ((word >> bit) & 1) ? HIGH : LOW);
      next += bitTimeUs;
      while (esp_timer_get_time() < next) {}
    }
    if (++wdtCnt >= 500) { wdtCnt = 0; feedWatchdog(); }
  }
}

/**
 * 发送字节数组 (MSB first).
 * 每字节从 bit 7 → 0 依次输出.
 */
void Transmitter::sendBytes(const uint8_t* data, size_t len, int bitTimeUs) {
  uint64_t next = esp_timer_get_time();
  int wdtCnt = 0;
  for (size_t i = 0; i < len; i++) {
    uint8_t byte = data[i];
    for (int bit = 7; bit >= 0; bit--) {
      digitalWrite(m_txPin, ((byte >> bit) & 1) ? HIGH : LOW);
      next += bitTimeUs;
      while (esp_timer_get_time() < next) {}
    }
    if (++wdtCnt >= 500) { wdtCnt = 0; feedWatchdog(); }
  }
}

// ============================================================
// POCSAG 发射
// ============================================================
void Transmitter::sendPocsag(const Config& cfg) {
  // 编码器为实例成员 (.bss), 不占任务栈; 非重入问题已消除。
  PocsagEncoder& encoder = m_pocsagEncoder;
  // Si4463 的 FSK MARK/SPACE 极性与寻呼机解调器预期相反,
  // 故 NORMAL = 基带反转以补偿硬件极性。
  bool invert = (cfg.phase == PhaseType::NORMAL);

  // UTF-8 → GB2312 转换 (直接引用 String 内部 buffer, 避免 getBytes 拷贝)
  String gbStr;  // 保持生命周期覆盖 data 使用期
  const uint8_t* data = (const uint8_t*)cfg.message;
  size_t dataLen = strlen(cfg.message);

  if (cfg.msgType == MsgType::ALPHANUMERIC) {
    gbStr = GB.get(String(cfg.message));
    dataLen = gbStr.length();
    data = (const uint8_t*)gbStr.c_str();   // 指向 String 内部 buffer, 无拷贝
  }

  encoder.encode(cfg.address, static_cast<int>(cfg.tone),  // 库边界: Pocsag 用 int 功能码
                 data, dataLen,
                 static_cast<DisplayMode>(cfg.msgType), invert);

  const unsigned long* bb = encoder.getBaseband();
  int bandLen = encoder.getBasebandLength();

  int bt   = bitTimeUs(cfg.baudRate);
  int pre  = preambleBits(cfg.baudRate);

  const char toneLetter = "ABCD"[static_cast<int>(cfg.tone) & 3];
  const char* modeName = (cfg.msgType == MsgType::NUMERIC) ? "NUM" : "ALPHA";
  const char* phaseName = (cfg.phase == PhaseType::INVERTED) ? "INV" : "NRM";
  Serial.printf("TX[%d][POCSAG] addr=%llu tone=%c baud=%d %s %s \"%s\"\n",
                m_txCount, cfg.address, toneLetter, cfg.baudRate,
                modeName, phaseName, cfg.message);

  Serial.print('.');
  enterTx();
  sendPreambleBits(pre, bt, invert);
  sendCodewords(bb, bandLen, bt);
  exitTx();
  Serial.println(" OK");
  m_txCount++;
}

// ============================================================
// FLEX 发射
// ============================================================
void Transmitter::sendFlex(const Config& cfg) {
  FlexEncoder encoder;
  // Si4463 FSK 极性补偿: NORMAL = 反转基带
  bool invert = (cfg.phase == PhaseType::NORMAL);
  int bt = bitTimeUs(cfg.baudRate);
  size_t len;

  bool isNum = (cfg.msgType == MsgType::NUMERIC);
  if (isNum) {
    len = encoder.encode(cfg.address, cfg.message, FlexEncoder::NUMERIC);
  } else {
    // 中文寻呼机: UTF-8 → GB2312 → FLEX Alphanumeric (V=101)
    // GB2312 字节经 & 0x7F 剥离 bit 7 后放入 7-bit 字段,
    // 寻呼机固件默认中文模式，每两字节重组为 GB2312 字符。
    //
    // 对于中英混排消息，用 \x0E(SI)/\x0F(SO) 控制字符切换:
    //   \x0E — 进入中文模式 (Shift Out, 两字节配对)
    //   \x0F — 进入英文模式 (Shift In,  单字节 ASCII)
    // 寻呼机默认中文模式，与 POCSAG 的 deCodeAlpha() 一致。
    uint8_t* mixed = m_flexMixed;   // 实例成员, 非 static (重入安全)
    String gbStr = GB.get(String(cfg.message));
    size_t gbLen = gbStr.length();

    size_t m = 0;
    bool inChinese = true;  // 默认中文模式
    for (size_t i = 0; i < gbLen && m < sizeof(m_flexMixed) - 8; i++) {
      uint8_t byte = (uint8_t)gbStr[i];  // 直接索引 String, 无拷贝
      if (byte >= 0x80) {
        // GB2312 中文字节
        if (!inChinese) {
          mixed[m++] = 0x0E;  // 切到中文
          inChinese = true;
        }
        mixed[m++] = byte;
      } else {
        // ASCII 字节
        if (inChinese) {
          mixed[m++] = 0x0F;  // 切到英文
          inChinese = false;
        }
        mixed[m++] = byte;
      }
    }
    mixed[m] = '\0';
    len = encoder.encode(cfg.address, (const char *)mixed, FlexEncoder::ALPHA);
  }

  const char* modeName = isNum ? "NUM" : "ALPHA(CN)";
  const char* phaseName = (cfg.phase == PhaseType::INVERTED) ? "INV" : "NRM";
  Serial.printf("TX[%d][FLEX] addr=%llu baud=%d %s %s \"%s\"\n",
                m_txCount, cfg.address, cfg.baudRate,
                modeName, phaseName, cfg.message);

  if (encoder.hasError()) {
    Serial.printf("  encode error: %s\n", encoder.getErrorString());
    m_txCount++;
    return;
  }

  Serial.print('.');
  enterTx();
  if (invert) {
    // 反相: 复制缓冲区并逐字节取反, 然后走 sendBytes 精确时序
    uint8_t* invBuf = (uint8_t*)malloc(len);
    if (invBuf) {
      const uint8_t* src = encoder.getData();
      for (size_t i = 0; i < len; i++) invBuf[i] = ~src[i];
      sendBytes(invBuf, len, bt);
      free(invBuf);
    } else {
      // malloc 失败, 直接发正相
      sendBytes(encoder.getData(), len, bt);
    }
  } else {
    sendBytes(encoder.getData(), len, bt);
  }
  exitTx();
  Serial.println(" OK");
  m_txCount++;
}

// ============================================================
// 公开 API — 统一入口 (非阻塞入队, 由 txTask 异步执行)
// ============================================================
bool Transmitter::send(const Config& cfg) {
  Job job;
  job.type = JobType::POCSAG;
  job.cfg  = cfg;  // 深拷贝 (含 message[256])
  return enqueue(job);
}

// ============================================================
// 原始发送 — Sweeper 等模块直接使用已编码数据 (深拷贝入队)
// ============================================================
bool Transmitter::sendPocsagRaw(const unsigned long* baseband, int wordCount,
                                 int baudRate) {
  if (!baseband || wordCount <= 0 || wordCount > JOB_MAX_POCSAG_WORDS) return false;
  Job job;
  job.type = JobType::RAW_POCSAG;
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

// ---- 任务侧实际发送实现 (原逻辑, 在 txTask 上下文执行) ----
void Transmitter::sendPocsagRawImpl(const unsigned long* baseband, int wordCount,
                                     int baudRate) {
  int bt = bitTimeUs(baudRate);
  enterTx();
  sendCodewords(baseband, wordCount, bt);
  exitTx();
}

void Transmitter::sendFlexRawImpl(const uint8_t* data, size_t len,
                                   int baudRate, bool invert) {
  int bt = bitTimeUs(baudRate);
  enterTx();
  if (invert) {
    uint8_t* invBuf = (uint8_t*)malloc(len);
    if (invBuf) {
      for (size_t i = 0; i < len; i++) invBuf[i] = ~data[i];
      sendBytes(invBuf, len, bt);
      free(invBuf);
    } else {
      // malloc 失败, 直接发正相
      sendBytes(data, len, bt);
    }
  } else {
    sendBytes(data, len, bt);
  }
  exitTx();
}
