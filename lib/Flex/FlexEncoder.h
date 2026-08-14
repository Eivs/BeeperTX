#pragma once

#include <stddef.h>
#include <stdint.h>

/**
 * @brief FLEX 协议编码器 C++ 封装。
 *
 * 支持的 FLEX 特性:
 *   - 速率: 1600 bps / 2-FSK
 *   - 帧: 单帧 (逐条) / 批量 (一次 ERS + 连续多帧, 每帧 frame=0)
 *   - 消息类型: Alphanumeric (ASCII)、Numeric、Tone-only
 *   - 地址: Short (7-digit) 和 Long (9-10 digit)
 *
 * 批量 (encodeBatch) v1 约束: 仅短地址 (capcode 1..1933312);
 * 所有帧 FIW 固定 cycle=0, frame=0 (追码寻呼机配置在 frame 0)。
 */
class FlexEncoder {
public:
  /** 消息类型 */
  enum MsgType {
    ALPHA = 0,    ///< ASCII 字母消息 (最长 248 字符)
    NUMERIC = 1,  ///< 数字消息 (0-9, -, _, [, ], 空格, U)
    TONE_ONLY = 2 ///< 仅提示音消息 (无文本)
  };

  /** 批量消息条目 (encodeBatch 输入; v1 仅短地址) */
  struct BatchMsg {
    uint64_t capcode;   //!< 短地址 (1..1933312)
    const char *msg;    //!< NULL = tone-only
    MsgType type;       //!< ALPHA / NUMERIC / TONE_ONLY
    bool mailDrop = false; //!< Mail Drop 标志 (仅 ALPHA 生效, 寻呼机特定行为)
  };

  FlexEncoder();

  /**
   * @brief 编码一条 FLEX 消息。
   * @param capcode 寻呼机地址 (Short: 1-1933312, Long: 2101249-4297068542)
   * @param msg     ASCII 消息字符串 (ALPHA/NUMERIC 模式)
   * @param type    消息类型 (ALPHA / NUMERIC / TONE_ONLY)
   * @param mailDrop Mail Drop 标志 (仅 ALPHA 生效: 置消息首字 bit20;
   *                  NUMERIC/TONE_ONLY 由协议规范忽略, 传值无效果)
   * @return 编码后的字节数，失败返回 0
   */
  size_t encode(uint64_t capcode, const char *msg, MsgType type = ALPHA,
                bool mailDrop = false);

  /**
   * @brief 批量编码: 一次 ERS + 连续多帧 (每帧 frame=0), 帧内多地址多消息。
   *
   * 追码优化: 55 地址 ALPHA 批 ≈ ERS 420 B + 4 帧×375 B = 1920 B,
   * 对比逐地址 (每地址 ERS+1 帧 ≈ 4.0 s) 提速 ~23×。
   *
   * @param msgs    批量消息数组 (仅短地址; 长地址返回错误, 由调用方降级)
   * @param count   消息数 (上限 55, 超出整批拒绝)
   * @param encoded 输出: 实际放入的消息数 (缓冲区满时 < count; 可传 nullptr)
   * @return 编码字节数; 失败返回 0 (hasError() 区分原因)
   */
  size_t encodeBatch(const BatchMsg *msgs, size_t count,
                     size_t *encoded = nullptr);

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

  /** FLEX 缓冲区大小: 单帧 800 B; 批量 55 地址 ALPHA 批 1920 B, 留余量 */
  static constexpr size_t BUFFER_SIZE = 2048;

  /** FLEX 字母消息最大字符数 (7-bit, 1600 bps 单帧)。
   *  与 flex.h MAX_CHARS_ALPHA 一致: (88 字/帧 - 4) * 3 - 4 = 248。 */
  static constexpr size_t MAX_ALPHA_CHARS = 248;

  /** 批量消息上限 (与 Sweeper FLEX_BATCH_SIZE 一致, 55 地址 ALPHA 批 4 帧) */
  static constexpr size_t MAX_BATCH_MSGS = 55;

  /** @return 是否 FLEX 短地址 (1..1933312) */
  static bool isShortCapcode(uint64_t c) { return c >= 1 && c <= 1933312; }

private:
  uint8_t m_buffer[BUFFER_SIZE]; //!< 编码缓冲区
  size_t m_length;               //!< 有效数据长度
  int m_error;                   //!< 错误码
};
