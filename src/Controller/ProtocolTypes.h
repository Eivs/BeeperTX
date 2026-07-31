#pragma once

#include <stdint.h>

// ============================================================
// 协议配置枚举 — 全工程唯一来源
// ============================================================
// API (TxConfig) / Transmitter (Config) / Sweeper (Config) 共用同一套
// 枚举, 消除历史"API 枚举 / Transmitter 枚举 / 裸 int"三套定义互相
// 强转 (值漂移即静默出错) 的问题。
//
// ⚠ 值必须保持稳定 — 它们直接落盘到 NVS (ConfigStore) 并被
//    Sweeper/协议库边界依赖。POCSAG=0, FLEX=1, NORMAL=0, INVERTED=1,
//    TONE_A=0..TONE_D=3, NUMERIC=0, ALPHANUMERIC=1。
//
// 库层 (lib/Pocsag, lib/Flex) 保留各自的内部枚举 — 协议库不依赖上层
// 类型; 边界转换集中在调用处 (见 Sweeper.cpp 的 FlexEncoder 转换)。
// ============================================================

enum class EncodingType : uint8_t {
  POCSAG = 0,
  FLEX   = 1
};

enum class PhaseType : uint8_t {
  NORMAL   = 0,  //!< 正相
  INVERTED = 1   //!< 反相
};

enum class ToneType : uint8_t {
  TONE_A = 0,
  TONE_B = 1,
  TONE_C = 2,
  TONE_D = 3
};

enum class MsgType : uint8_t {
  NUMERIC      = 0, //!< 数字 (POCSAG: 4-bit BCD)
  ALPHANUMERIC = 1  //!< 汉字/字母 (POCSAG: GB2312, FLEX: ASCII)
};
