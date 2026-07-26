#include "FlexEncoder.h"
#include "flex.h"

FlexEncoder::FlexEncoder() : m_length(0), m_error(0) {
  // m_buffer 无需初始化，encode() 会写入
}

size_t FlexEncoder::encode(uint64_t capcode, const char *msg, MsgType type) {
  m_error = 0;
  m_length = 0;

  switch (type) {
  case ALPHA:
    m_length = tf_encode_flex_message(msg, capcode, m_buffer, sizeof(m_buffer),
                                      &m_error);
    break;

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

const char *FlexEncoder::getErrorString() const {
  switch (m_error) {
  case 0:
    return "No error";
  case -TF_INVALID_MESSAGE:
    return "Invalid message (empty or too long)";
  case -TF_INVALID_CAPCODE:
    return "Invalid capcode (out of range)";
  case -TF_INVALID_FLEXBUFFER:
    return "Buffer too small for FLEX frame";
  default:
    return "Unknown error";
  }
}
