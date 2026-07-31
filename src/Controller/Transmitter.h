#pragma once

#include <stdint.h>
#include <stddef.h>

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
