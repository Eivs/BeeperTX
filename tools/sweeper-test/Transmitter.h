// 宿主机测试 stub — Transmitter 最小替代 (方法实现见 harness.cpp)
// ⚠ 仅测试用精简接口: 勿当作生产代码修改; 若生产 Transmitter.h 变更导致编译失败,
//   应在此同步精简 (本 stub 与 src/Controller/Transmitter.h 故意不同 — 只暴露测试所需子集)
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "ProtocolTypes.h"  // 真实枚举源 (与生产同源, 验证脚本复制)

class Transmitter {
public:
  // 协议枚举统一来自 ProtocolTypes.h (与生产 Transmitter.h 一致)

  struct Config {
    EncodingType encoding = EncodingType::POCSAG;
    PhaseType phase = PhaseType::NORMAL;
    int baudRate = 1200;
    ToneType tone = ToneType::TONE_A;
    uint64_t address = 1000;
    char message[256] = "";
    MsgType msgType = MsgType::ALPHANUMERIC;
  };

  bool send(const Config& cfg);
  bool sendPocsagRaw(const unsigned long* baseband, int wordCount, int baudRate);
  bool sendFlexRaw(const uint8_t* data, size_t len, int baudRate, bool invert);

  /** 门控查询: 模拟 txTask 忙 (发射中/队列积压), 由 harness 全局控制 */
  bool isBusy() const;
};
