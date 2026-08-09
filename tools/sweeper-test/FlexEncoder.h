// 宿主机测试 stub — FlexEncoder 最小替代
// ⚠ 仅测试用精简接口: 勿当作生产代码修改; 若 lib/Flex/FlexEncoder.h 变更导致编译失败,
//   应在此同步精简 (本 stub 与生产 FlexEncoder 故意不同 — 只暴露测试所需子集)
#pragma once
#include <stdint.h>
#include <stddef.h>

class FlexEncoder {
public:
  enum MsgType { ALPHA = 0, NUMERIC = 1, TONE_ONLY = 2 };

  struct BatchMsg {
    uint64_t capcode;
    const char* msg;
    MsgType type;
  };

  size_t encode(uint64_t capcode, const char* msg, MsgType type = ALPHA) {
    (void)capcode; (void)msg; (void)type;
    m_buf[0] = 0xAA;
    return 1;
  }
  size_t encodeBatch(const BatchMsg*, size_t count, size_t* encoded) {
    if (encoded) *encoded = count;
    m_buf[0] = 0xAA;
    return 1;
  }
  bool hasError() const { return false; }
  const uint8_t* getData() const { return m_buf; }

  static bool isShortCapcode(uint64_t c) { return c >= 1 && c <= 1933312; }

private:
  uint8_t m_buf[8];
};
