#pragma once

#include <Arduino.h>

// ============================================================================
// POCSAG 协议常量
// ============================================================================
#define POCSAG_IDLE 0x7A89C197UL
#define POCSAG_SYNC 0x7CD215D8UL
#define POCSAG_PREAMBLE 0xAAAAAAAAUL

// ============================================================================
// 缓冲区大小限制
// ============================================================================
#define MAX_CODEMORDS 512    // 最多消息码字数
#define MAX_BASEBAND 768     // 基带码字总数
#define MAX_MESSAGE_LEN 1024 // 输入消息最大字节数

// ============================================================================
// 显示模式 (由地址码字的功能位指示)
// ============================================================================
enum DisplayMode {
  MODE_NUMERIC = 0,     // 纯数字 (4-bit BCD)
  MODE_ALPHANUMERIC = 1 // 英文/字母 (7-bit ASCII)
};

/**
 * @brief POCSAG (Post Office Code Standardisation Advisory Group)
 *        寻呼协议编码器。
 *
 * 将文本消息编码为 POCSAG 基带码字，适合 FSK 调制发送。
 */
class PocsagEncoder {
public:
  PocsagEncoder();

  /**
   * @brief 编码一条消息，发送到一个或多个寻呼机。
   * @param addr       21-bit POCSAG 地址
   * @param function   2-bit 功能码 (0-3, 对应铃音 TONE_A~TONE_D)
   * @param msg        消息字节
   * @param length     消息长度（字节）
   * @param mode       显示模式 (MODE_NUMERIC / MODE_ALPHANUMERIC)
   * @param invert     反相发射 (true=反相 全码字取反, false=正相)
   */
  void encode(unsigned long addr, int function, const uint8_t *msg,
              size_t length, DisplayMode mode = MODE_ALPHANUMERIC,
              bool invert = false);

  /** @return 基带码字缓冲区指针 */
  const unsigned long *getBaseband() const { return m_baseband; }

  /** @return 基带有效码字数 */
  int getBasebandLength() const { return m_bandLength; }

private:
  // ---- POCSAG 辅助函数 ----

  /** BCH(31,21) 校验码计算 */
  static unsigned long calcCRC(unsigned long data);

  /** 32-bit 码字偶校验 */
  static int evenCheck(unsigned long data);

  /** 生成 576-bit 前导码 (18 次重复 0xAAAAAAAA) */
  void genPreamble();

  /**
   * @brief 生成地址码字。
   * @param addr     21-bit 寻呼机地址
   * @param function 2-bit 功能码 (0-3)
   * @return 32-bit 地址码字
   */
  unsigned long genAddrCode(unsigned long addr, int function);

  /**
   * @brief 从 20 bit 数据生成消息码字。
   * @param msgBin 20-bit 消息片段
   * @return 32-bit 消息码字
   */
  unsigned long genMsgCode(unsigned long msgBin);

  /**
   * @brief 将消息字节编码为 POCSAG 码字 (支持 GB2312 中文)。
   * @param charBytes  输入消息字节 (ASCII < 0x80, GB2312 >= 0x80)
   * @param length     输入长度
   * @return POCSAG 功能码 (1=纯ASCII, 3=含GB2312中文)
   */
  int deCodeAlpha(const uint8_t *charBytes, size_t length);

  /**
   * @brief 将数字消息转换为 4-bit BCD POCSAG 码字。
   * @param charBytes  输入消息字节
   * @param length     输入长度
   */
  int deCodeNumeric(const uint8_t *charBytes, size_t length);

  /**
   * @brief 为一个寻呼机生成完整基带信号。
   */
  void genSignal(unsigned long addr, int function, const uint8_t *msg,
                 size_t length, DisplayMode mode, bool invert);

  // ---- 内部缓冲区 ----
  unsigned long m_msgCode[MAX_CODEMORDS]; //!< 编码后的消息码字
  unsigned long m_baseband[MAX_BASEBAND]; //!< 完整基带信号
  int m_bandLength;                       //!< 基带有效长度
};
