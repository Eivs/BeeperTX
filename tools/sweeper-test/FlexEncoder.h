// 宿主机测试 stub — FlexEncoder 最小替代
#pragma once
#include <stdint.h>
#include <stddef.h>

class FlexEncoder {
public:
  enum MsgType { ALPHA = 0, NUMERIC = 1, TONE_ONLY = 2 };

  size_t encode(uint64_t capcode, const char* msg, MsgType type = ALPHA) {
    (void)capcode; (void)msg; (void)type;
    m_buf[0] = 0xAA;  // 返回 1 字节, 使 Sweeper 判定 len>0 且无错误
    return 1;
  }
  bool hasError() const { return false; }
  const uint8_t* getData() const { return m_buf; }

private:
  uint8_t m_buf[8];
};
