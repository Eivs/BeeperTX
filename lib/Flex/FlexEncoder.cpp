#include "FlexEncoder.h"
#include "flex.h"

FlexEncoder::FlexEncoder() : m_length(0), m_error(0) {
  // m_buffer 无需初始化，encode() 会写入
}

size_t FlexEncoder::encode(uint64_t capcode, const char *msg, MsgType type,
                           bool mailDrop) {
  m_error = 0;
  m_length = 0;

  switch (type) {
  case ALPHA: {
    // Mail Drop 标志经扩展 API 透传 (仅 ALPHA 生效: 置消息首字 bit20)
    struct tf_message_config cfg;
    cfg.mail_drop = mailDrop ? 1 : 0;
    m_length = tf_encode_flex_message_ex(msg, capcode, m_buffer,
                                         sizeof(m_buffer), &m_error, &cfg);
    break;
  }

  case NUMERIC:
    m_length = tf_encode_flex_numeric_message(msg, capcode, m_buffer,
                                              sizeof(m_buffer), &m_error);
    break;

  case TONE_ONLY:
    // TONE_ONLY 不需要 msg 参数 (可以为 NULL)
    m_length = tf_encode_flex_tone_only_msg(capcode, m_buffer, sizeof(m_buffer),
                                            &m_error);
    break;
  }

  return m_length;
}

size_t FlexEncoder::encodeBatch(const BatchMsg *msgs, size_t count,
                                size_t *encoded) {
  m_error = 0;
  m_length = 0;
  if (encoded) *encoded = 0;
  if (!msgs || count == 0) { m_error = -TF_INVALID_MESSAGE; return 0; }
  if (count > MAX_BATCH_MSGS) { m_error = -TF_INVALID_MESSAGE; return 0; }

  // 映射到 C 层批量结构 (字段一一对应; config 透传 mail_drop)
  struct tf_batch_msg tmp[MAX_BATCH_MSGS];
  struct tf_message_config cfg[MAX_BATCH_MSGS];
  for (size_t i = 0; i < count; i++) {
    tmp[i].capcode = msgs[i].capcode;
    tmp[i].msg     = (msgs[i].type == TONE_ONLY) ? (const char *)NULL
                                                 : msgs[i].msg;
    tmp[i].type    = (msgs[i].type == ALPHA) ? TF_MESSAGE_TYPE_ALPHA
                                             : TF_MESSAGE_TYPE_NUMERIC;
    cfg[i].mail_drop = msgs[i].mailDrop ? 1 : 0;
    tmp[i].config  = &cfg[i];  // 编码器仅在 ALPHA 路径读取 (Numeric/Tone 忽略)
  }

  size_t enc = 0;
  m_length = tf_encode_flex_batch(tmp, count, m_buffer, sizeof(m_buffer),
                                  &m_error, &enc);
  if (encoded) *encoded = enc;

  if (m_error != 0) {
    // 失败: 丢弃部分输出 (调用方降级/重试), 与 encode() 失败语义一致
    m_length = 0;
    if (encoded) *encoded = 0;
    return 0;
  }
  return m_length;
}

const char *FlexEncoder::getErrorString() const {
  switch (m_error) {
  case 0:
    return "No error";
  case -TF_INVALID_MESSAGE:
    return "Invalid message (empty or too long)";
  case -TF_INVALID_CAPCODE:
    return "Invalid capcode (out of range)";
  case -TF_LONG_ADDR_IN_BATCH:
    return "Long address not supported in batch (v1 short-address only)";
  case -TF_INVALID_FLEXBUFFER:
    return "Buffer too small for FLEX frame";
  default:
    return "Unknown error";
  }
}
