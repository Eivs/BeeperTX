#pragma once

#include <stddef.h>
#include <stdint.h>

/**
 * @brief FLEX 协议编码器 C++ 封装。
 *
 * 基于 flex.h 单头文件库，将对 FLEX 寻呼协议的支持
 * 添加到现有 POCSAG 项目中。
 *
 * 支持的 FLEX 特性:
 *   - 速率: 1600 bps / 2-FSK
 *   - 帧: 单帧
 *   - 消息类型: Alphanumeric (ASCII)、Numeric、Tone-only
 *   - 地址: Short (7-digit) 和 Long (9-10 digit)
 */
class FlexEncoder {
public:
  /** 消息类型 */
  enum MsgType {
    ALPHA = 0,    ///< ASCII 字母消息 (最长 248 字符)
    NUMERIC = 1,  ///< 数字消息 (0-9, -, _, [, ], 空格, U)
    TONE_ONLY = 2 ///< 仅提示音消息 (无文本)
  };

  FlexEncoder();

  /**
   * @brief 编码一条 FLEX 消息。
   * @param capcode 寻呼机地址 (Short: 1-1933312, Long: 2101249-4297068542)
   * @param msg     ASCII 消息字符串 (ALPHA/NUMERIC 模式)
   * @param type    消息类型 (ALPHA / NUMERIC / TONE_ONLY)
   * @return 编码后的字节数，失败返回 0
   */
  size_t encode(uint64_t capcode, const char *msg, MsgType type = ALPHA);

  /** @return 编码数据缓冲区指针 (字节数组) */
  const uint8_t *getData() const { return m_buffer; }

  /** @return 编码数据有效长度 (字节) */
  size_t getLength() const { return m_length; }

  /** @return 错误码 (0=无错误) */
  int getError() const { return m_error; }

  /** @return 人类可读的错误描述 */
  const char *getErrorString() const;

  /** @return 是否有错误 */
  bool hasError() const { return m_error != 0; }

  // ---- FLEX 协议常数 ----

  /** FLEX 1600 bps 的位时间 (µs) */
  static constexpr int BIT_TIME_US = 625;

  /** FLEX 波特率 */
  static constexpr int BAUDRATE = 1600;

  /** FLEX 缓冲区大小 (flex.h 所需) */
  static constexpr size_t BUFFER_SIZE = 800;

private:
  uint8_t m_buffer[BUFFER_SIZE]; //!< 编码缓冲区
  size_t m_length;               //!< 有效数据长度
  int m_error;                   //!< 错误码
};
