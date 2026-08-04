#pragma once

#include <stdint.h>
#include <stddef.h>
#include "ProtocolTypes.h"
#include "Pocsag.h"
#include "FlexEncoder.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

/**
 * @brief 统一发射控制器 — 支持 POCSAG / FLEX 两种编码。
 *
 * 封装 Direct TX Mode (GPIO0 bit-banging) 的公共流程:
 *   启动 Direct TX → 发送前导码/数据 → 停止 TX
 *
 * 架构: send() 在 core 0 预编码, 通过队列提交到 core 1 的 txTask 异步发射。
 * POCSAG 和 FLEX 均先编码再提交 RAW Job, 编码位置对称。
 *
 * 使用方式:
 * @code
 *   Transmitter tx(PIN_TX_DATA);
 *   Transmitter::Config cfg;
 *   cfg.encoding = EncodingType::FLEX;
 *   cfg.baudRate = 1600;
 *   cfg.address  = 37137;
 *   cfg.message  = "你好 FLEX";
 *   tx.send(cfg);
 * @endcode
 */
class Transmitter {
public:
  struct Config {
    EncodingType encoding   = EncodingType::POCSAG;
    PhaseType    phase      = PhaseType::NORMAL;
    int          baudRate   = 1200;
    ToneType     tone       = ToneType::TONE_A;
    uint64_t     address    = 1000;
    char         message[256] = "";
    MsgType      msgType    = MsgType::ALPHANUMERIC;
  };

  explicit Transmitter(int txPin);

  /**
   * @brief 发射一条消息 (预编码后异步入队, txTask 执行位敲击)。
   * @return true=已入队, false=队列满/编码失败
   */
  bool send(const Config& cfg);

  /**
   * @brief 直接发送已编码的 POCSAG 基带码字数组 (已含前导码)。
   *        供 Sweeper 批量追码使用 (encodeBatchMulti 输出)。
   */
  bool sendPocsagRaw(const unsigned long* baseband, int wordCount, int baudRate);

  /**
   * @brief 直接发送已编码的 FLEX 字节数组。
   *        供 Sweeper 追码 / send() 预编码路径使用。
   */
  bool sendFlexRaw(const uint8_t* data, size_t len, int baudRate, bool invert);

  /** 发射通道是否忙 */
  bool isBusy() const;

  /** 中断当前发射: 清空队列 + 设 abort 标志 */
  void abortTx();

  /** 清除中断标志 */
  void clearAbort() { m_abort = false; }

  // ---- 静态工具 ----
  static int bitTimeUs(int baudRate);
  static int preambleBits(int baudRate);

private:
  int  m_txPin;
  int  m_txCount = 0;

  // FLEX 中英混排控制字符缓冲 (实例成员, 非 static)
  uint8_t m_flexMixed[512];

  // POCSAG 单次发送预编码器 (实例成员, 避免 ~6KB 栈分配)
  PocsagEncoder m_pocsagEncoder;

  // ---- 后台发射任务 (core 1) ----
  enum class JobType : uint8_t {
    RAW_POCSAG = 0,  //!< 预编码 POCSAG (单次 + Sweeper 批量)
    RAW_FLEX   = 1,  //!< 预编码 FLEX  (单次 + Sweeper 批量)
  };

  static const int JOB_MAX_POCSAG_WORDS = 1024;
  static const int JOB_MAX_FLEX_BYTES   = 2048;

  struct Job {
    JobType type;
    int     baudRate;
    bool    invert;
    Job() : type(JobType::RAW_POCSAG), baudRate(0), invert(false) {}
    union {
      struct { unsigned long words[JOB_MAX_POCSAG_WORDS]; int wordCount; } pocsag;
      struct { uint8_t  data[JOB_MAX_FLEX_BYTES]; size_t len; } flex;
    };
  };
  QueueHandle_t m_queue = nullptr;
  TaskHandle_t  m_task  = nullptr;
  volatile bool m_txActive = false;
  volatile bool m_abort    = false;

  bool ensureTask();
  static void txTaskEntry(void* arg);
  void txTaskMain();
  bool enqueue(Job& job);
  void feedWatchdog();

  void sendPocsagRawImpl(const unsigned long* baseband, int wordCount, int baudRate);
  void sendFlexRawImpl(const uint8_t* data, size_t len, int baudRate, bool invert);

  void enterTx();
  void exitTx();

  void sendCodewords(const unsigned long* cw, int count, int bitTimeUs);
  void sendBytes(const uint8_t* data, size_t len, int bitTimeUs, bool invert = false);
};
