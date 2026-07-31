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

  /**
   * @brief 批量编码 — 多个地址共用一个前导码, 按规格帧结构拼接。
   *
   * 帧结构 (规格 §4.2): 每组 = [SYNC] + 8 帧 × [Addr][Msg], 最多 8 地址共享一组。
   * 地址放入其 (addr & 0x07) 对应的帧槽, 消息码字紧跟其后 (每组每地址 1 个消息 CW)。
   *
   * @param addresses  地址数组 (需连续递增, 且每条消息必须 ≤1 个消息码字)
   * @param addrCount  地址数量
   * @param function   2-bit 功能码 (0-3)
   * @param mode       消息编码方式 (NUMERIC=4-bit BCD, ALPHANUMERIC=7-bit ASCII)
   * @param invert     反相发射
   * @param addrEncoded [out] 可选: 实际编码的地址数 (任一地址消息 >1 CW 时返回 0)
   * @return 基带总码字数; 0 = 参数非法或消息超过 1 个码字 (调用方需降级逐地址发送)
   */
  int encodeBatch(const uint32_t *addresses, int addrCount,
                  int function, DisplayMode mode, bool invert,
                  int *addrEncoded = nullptr);

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
   * @brief 将消息字节编码为 POCSAG 消息码字 (支持 GB2312 中文)。
   *         Tone (function) 由用户指定, 此处不自动检测。
   */
  void deCodeAlpha(const uint8_t *charBytes, size_t length);

  /**
   * @brief 将数字消息转换为 4-bit BCD POCSAG 消息码字。
   */
  void deCodeNumeric(const uint8_t *charBytes, size_t length);

  /**
   * @brief 为一个寻呼机生成完整基带信号。
   *         function 即是 WebUI 设置的 Tone (A=0, B=1, C=2, D=3),
   *         直接写入地址码字的功能码位, 不自动覆盖。
   */
  void genSignal(unsigned long addr, int function, const uint8_t *msg,
                 size_t length, DisplayMode mode, bool invert);

  // ---- 内部缓冲区 ----
  unsigned long m_msgCode[MAX_CODEMORDS]; //!< 编码后的消息码字
  unsigned long m_baseband[MAX_BASEBAND]; //!< 完整基带信号
  int m_bandLength;                       //!< 基带有效长度
};
