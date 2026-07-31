// 宿主机测试 stub — Transmitter 最小替代 (方法实现见 harness.cpp)
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

class Transmitter {
public:
  enum Encoding { POCSAG = 0, FLEX = 1 };
  enum Phase { NORMAL = 0, INVERTED = 1 };
  enum Tone { TONE_A = 0, TONE_B = 1, TONE_C = 2, TONE_D = 3 };
  enum MsgType { NUMERIC = 0, ALPHANUMERIC = 1 };

  struct Config {
    Encoding encoding = POCSAG;
    Phase phase = NORMAL;
    int baudRate = 1200;
    Tone tone = TONE_A;
    uint64_t address = 1000;
    char message[256] = "";
    MsgType msgType = ALPHANUMERIC;
  };

  void send(const Config& cfg);
  void sendPocsagRaw(const unsigned long* baseband, int wordCount, int baudRate);
  void sendFlexRaw(const uint8_t* data, size_t len, int baudRate, bool invert);
};
