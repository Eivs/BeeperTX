#pragma once

#include <stdint.h>
#include <stddef.h>
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
 * 码率、相位等由 Config 结构体统一配置。
 *
 * 使用方式:
 * @code
 *   Transmitter tx(PIN_TX_DATA);
 *   Transmitter::Config cfg;
 *   cfg.encoding = Transmitter::POCSAG;
 *   cfg.baudRate = 1200;
 *   cfg.address  = 1000;
 *   cfg.message  = "你好 FLEX";
 *   tx.send(cfg);
 * @endcode
 */
class Transmitter {
public:
  /** 编码方式 */
  enum Encoding {
    POCSAG = 0,  //!< POCSAG 协议 (支持中文 GB2312)
    FLEX   = 1   //!< FLEX 协议 (ASCII only)
  };

  /** 相位 (FSK 极性) */
  enum Phase {
    NORMAL   = 0, //!< 正相
    INVERTED = 1  //!< 反相
  };

  /** 铃音 (POCSAG function bits) */
  enum Tone {
    TONE_A = 0,
    TONE_B = 1,
    TONE_C = 2,
    TONE_D = 3
  };

  /** 消息类型 */
  enum MsgType {
    NUMERIC      = 0, //!< 数字 (POCSAG: 4-bit BCD)
    ALPHANUMERIC = 1  //!< 汉字/字母 (POCSAG: GB2312, FLEX: ASCII)
  };

  /**
   * @brief 发射配置 (仅数据容器, 不含逻辑)
   *
   * main.cpp 只填充此结构体, 然后调用 send().
   */
  struct Config {
    Encoding   encoding   = POCSAG;          //!< POCSAG / FLEX
    Phase      phase      = NORMAL;           //!< 正相 / 反相 (根据射频配置和寻呼机适配情况选择)
    int        baudRate   = 1200;             //!< 码率: 512/1200/1600/2400/3200/6400
    Tone       tone       = TONE_A;           //!< 铃音
    uint64_t   address    = 1000;             //!< 寻呼机地址
    char       message[256] = "";             //!< 消息内容 (UTF-8, 最大 255 字节)
    MsgType    msgType    = ALPHANUMERIC;//!< 数字 / 汉字
  };

  /**
   * @param txPin 连接到 Si4463 GPIO0 的 ESP32 GPIO 编号
   */
  explicit Transmitter(int txPin);

  /**
   * @brief 发射一条消息 (自动选择编码器并调度 Direct TX)
   */
  void send(const Config& cfg);

  /**
   * @brief 直接发送已编码的 POCSAG 基带码字数组 (已含前导码)。
   *        一次 enterTx→send→exitTx, 不做额外编码/反相。
   *        供 Sweeper 批量追码使用 (PocsagEncoder::encodeBatch 输出),
   *        反相已由编码器处理。
   */
  void sendPocsagRaw(const unsigned long* baseband, int wordCount, int baudRate);

  /**
   * @brief 直接发送已编码的 FLEX 字节数组。
   *        一次 enterTx→send→exitTx, 可选字节级反相。
   *        供 Sweeper 逐地址追码使用 (FlexEncoder 输出)。
   */
  void sendFlexRaw(const uint8_t* data, size_t len, int baudRate, bool invert);

  // ---- 静态工具 ----

  /** 码率 → 位时间 (µs) */
  static int bitTimeUs(int baudRate);

  /** POCSAG 前导码长度 (bit) */
  static int preambleBits(int baudRate);

private:
  int  m_txPin;
  int  m_txCount = 0;   //!< 发射序号 (仅用于日志)

  // 编码器/混频缓冲区作为实例成员 (.bss), 而非函数内 static:
  // 消除模块级共享状态, 多个 Transmitter 实例可安全并发。
  PocsagEncoder m_pocsagEncoder;   //!< POCSAG 编码器 (基带 ~5KB)
  uint8_t       m_flexMixed[512];  //!< FLEX 中英混排控制字符缓冲

  // ---- 后台发射任务 (核心架构: 位敲击移出 loopTask) ----
  //
  // ESP32-S3 双核布局:
  //   core 0: WiFi 协议栈 + AsyncTCP (Web 请求处理) → 发射期间保持可用
  //   core 1: loopTask (priority 1) + txTask (priority 3) → 发射独占
  //
  // send()/sendPocsagRaw()/sendFlexRaw() 通过单槽队列提交 Job,
  // 阻塞等待 txTask 完成 (同步语义保持不变), 实际 μs 级位敲击
  // 全部发生在 txTask (core 1, 高优先级), 不再卡死 Web 处理。
  // 发射期间 loopTask 被高优先级 txTask 抢占 — 与重构前行为等价
  // (原本就是同步阻塞), 但 Web/WiFi (core 0) 不再受影响。
  enum class JobType : uint8_t {
    POCSAG     = 0,  //!< 标准发射 (Config)
    RAW_POCSAG = 1,  //!< Sweeper 批量追码
    RAW_FLEX   = 2,  //!< Sweeper 逐地址追码
  };
  struct Job {
    JobType type;
    Config  cfg;                     // POCSAG
    const unsigned long* baseband;   // RAW_POCSAG
    int     wordCount;
    const uint8_t* data;             // RAW_FLEX
    size_t  len;
    int     baudRate;
    bool    invert;
  };
  QueueHandle_t     m_queue = nullptr;  //!< 单槽任务队列
  TaskHandle_t      m_task  = nullptr;  //!< txTask 句柄
  SemaphoreHandle_t m_done  = nullptr;  //!< 完成信号量 (计数, 初值 0)

  void ensureTask();                        //!< 懒创建队列/任务/信号量
  static void txTaskEntry(void* arg);
  void txTaskMain();                        //!< 任务主体: 收 Job → 发送 → give
  void submit(Job& job);                    //!< 入队 + 等待完成 (同步语义)
  void feedWatchdog();                      //!< 喂 txTask 自身的 WDT (发射期间安全网)

  // ---- 任务侧实际发送 (在 txTask 上下文执行) ----
  void sendPocsagRawImpl(const unsigned long* baseband, int wordCount, int baudRate);
  void sendFlexRawImpl(const uint8_t* data, size_t len, int baudRate, bool invert);

  // ---- Direct TX 流程 ----
  void enterTx();
  void exitTx();

  // ---- 低层位流发送 ----
  void sendPreambleBits(int bits, int bitTimeUs, bool invert);
  void sendCodewords(const unsigned long* cw, int count, int bitTimeUs);
  void sendBytes(const uint8_t* data, size_t len, int bitTimeUs);

  // ---- 协议调度 ----
  void sendPocsag(const Config& cfg);
  void sendFlex(const Config& cfg);
};
