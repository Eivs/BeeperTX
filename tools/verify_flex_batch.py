#!/usr/bin/env python3
"""
主机端验证: FLEX 批量编码 (tf_encode_flex_batch / FlexEncoder::encodeBatch)

设计: 一次 ERS + 连续多帧 (frame=0), 帧内 多地址 + 多向量 + 多消息。
此脚本在宿主机编译 lib/Flex/flex.h + FlexEncoder.cpp, 验证:

  1. 单条消息批量输出 == 单帧路径输出 (逐字节相等, 布局与字段完全一致)
  2. ERS 只出现一次 (35 × 12 B), 每帧自带 S1+FIW+S2
  3. 所有帧 FIW 固定 cycle=0, frame=0
  4. BIW s_vfield = 1 + 地址数; 地址域/向量域/消息域边界正确
  5. 每个向量 msg_start 指向消息域, 消息字与单帧路径逐字相等
  6. 消息域之后的 idle 交替填充 (偶 0xFFFFFFFF / 奇 0)
  7. 55 地址 ALPHA 批 = 4 帧 (55×6=330 ≤ 4×87), NUMERIC = 2 帧
  8. 长地址 → TF_LONG_ADDR_IN_BATCH; 缓冲区过小 → TF_INVALID_FLEXBUFFER;
     count 超上限 → 拒绝; tone-only 批量可用
  9. FlexEncoder::encodeBatch 封装层: encoded 计数与错误码传播

用法: python3 tools/verify_flex_batch.py
退出码: 0=全部通过, 1=存在失败
"""

import subprocess
import sys
import os
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

HARNESS = r"""
/* flex.h 是"单头文件"库: 公共函数为非 static 定义, 只能在一个 TU 内包含。
 * 这里直接包含 FlexEncoder.cpp, 让它把 flex.h 定义带进来 (单一 TU)。 */
#include "FlexEncoder.cpp"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>

static int g_fail = 0;

#define CHECK(cond, name, detail)                                              \
  do {                                                                         \
    if (cond) {                                                                \
      printf("  [PASS] %s\n", name);                                           \
    } else {                                                                   \
      printf("  [FAIL] %s: %s\n", name, detail);                               \
      g_fail++;                                                                \
    }                                                                          \
  } while (0)

/* ---- 反交织: interleave_block 的逆 ----
 * interleave_block: dst_block[i] bit(7-j) = src word j bit(31-i)
 * 逆: word j bit b = byte (31-b) bit (7-j) */
static void deinterleave(uint32_t *words, const uint8_t *bytes) {
  memset(words, 0, WORDS_PER_FRAME * sizeof(uint32_t));
  for (uint32_t blk = 0; blk < BLOCKS_PER_FRAME; blk++) {
    for (uint32_t i = 0; i < 32; i++) {
      uint8_t byte = bytes[blk * 32 + i];
      for (uint32_t j = 0; j < 8; j++) {
        if (byte & (1u << (7 - j)))
          words[blk * 8 + j] |= (1u << (31 - i));
      }
    }
  }
}

/* 读 4 字节大端为 uint32 (SAVE_WORD 的逆) */
static uint32_t rd32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static const uint8_t *frame_data(const uint8_t *buf, size_t frames) {
  return buf + 420 + frames * 375 + 23;  /* ERS + f×375 + S1+FIW+S2 */
}

/* 单帧路径编码, 返回消息字 (去掉 vector) */
static std::vector<uint32_t> single_msg_words(uint64_t cap, const char *msg,
                                              int numeric) {
  uint8_t out[800];
  int err = 0;
  size_t len = numeric ? tf_encode_flex_numeric_message(msg, cap, out, sizeof(out), &err)
                       : tf_encode_flex_message(msg, cap, out, sizeof(out), &err);
  std::vector<uint32_t> words;
  if (err || len == 0) return words;
  uint32_t fw[WORDS_PER_FRAME];
  deinterleave(fw, frame_data(out, 0));
  uint32_t biw = tf_decode_word_data(fw[0]);
  uint32_t s_v = (biw >> 10) & 0x3F;
  uint32_t vec = tf_decode_word_data(fw[s_v]);
  uint32_t ms = (vec >> 7) & 0x7F;
  uint32_t mw = (vec >> 14) & 0x7F;
  for (uint32_t i = 0; i < mw; i++) words.push_back(fw[ms + i]);
  return words;
}

/* 校验一个 batch 帧: 期望地址列表, 每地址期望消息字 (与单帧比对) */
static int check_frame(const uint8_t *buf, size_t frame_idx,
                       const std::vector<uint32_t> &caps,
                       const std::vector<std::string> &msgs, int numeric,
                       std::string *err) {
  uint32_t fw[WORDS_PER_FRAME];
  deinterleave(fw, frame_data(buf, frame_idx));
  uint32_t biw = tf_decode_word_data(fw[0]);
  uint32_t s_v = (biw >> 10) & 0x3F;
  uint32_t naddr = s_v - 1;
  if (s_v == 0 || s_v > 63) { *err = "BIW s_vfield 越界"; return -1; }
  if (naddr != caps.size()) {
    char b[64]; snprintf(b, sizeof(b), "s_vfield=%u 地址数=%u 期望 %u",
                         s_v, naddr, (unsigned)caps.size());
    *err = b; return -1;
  }
  /* 地址域 */
  for (size_t k = 0; k < caps.size(); k++) {
    uint32_t dw = tf_decode_word_data(fw[1 + k]);
    if (dw != caps[k] + 32768) {
      char b[64]; snprintf(b, sizeof(b), "地址[%u] 期望 %u 实得 %u",
                           (unsigned)k, (unsigned)(caps[k] + 32768), dw);
      *err = b; return -1;
    }
  }
  /* 向量域 + 消息域 */
  uint32_t m_end = 0;
  for (size_t k = 0; k < caps.size(); k++) {
    uint32_t vec = tf_decode_word_data(fw[s_v + k]);
    uint32_t ms = (vec >> 7) & 0x7F;
    uint32_t mw = (vec >> 14) & 0x7F;
    if (ms < s_v + (uint32_t)caps.size()) {
      char b[64]; snprintf(b, sizeof(b), "消息 %u msg_start=%u 在向量域内",
                           (unsigned)k, ms);
      *err = b; return -1;
    }
    if (ms + mw > WORDS_PER_FRAME) {
      char b[64]; snprintf(b, sizeof(b), "消息 %u 越界 ms=%u mw=%u",
                           (unsigned)k, ms, mw);
      *err = b; return -1;
    }
    /* 消息字与单帧路径逐字比对 */
    std::vector<uint32_t> expect = single_msg_words(caps[k], msgs[k].c_str(), numeric);
    if (expect.size() != mw) {
      char b[96]; snprintf(b, sizeof(b), "消息 %u 字数 %u 期望 %u",
                           (unsigned)k, mw, (unsigned)expect.size());
      *err = b; return -1;
    }
    for (uint32_t i = 0; i < mw; i++) {
      if (fw[ms + i] != expect[i]) {
        char b[96]; snprintf(b, sizeof(b), "消息 %u 字 %u 不匹配", (unsigned)k, i);
        *err = b; return -1;
      }
    }
    if (ms + mw > m_end) m_end = ms + mw;
  }
  /* idle 填充 (偶 0xFFFFFFFF / 奇 0) */
  for (uint32_t i = m_end; i < WORDS_PER_FRAME; i++) {
    uint32_t want = (i & 1) ? 0 : 0xFFFFFFFF;
    if (fw[i] != want) {
      char b[64]; snprintf(b, sizeof(b), "idle[%u] 期望 %08X 实得 %08X", i, want, fw[i]);
      *err = b; return -1;
    }
  }
  return 0;
}

/* ERS 字节是否出现第二次 (帧边界之外) — 只查 ERS 起始模式 */
static bool ers_elsewhere(const uint8_t *buf, size_t len) {
  static const uint8_t pat[4] = {0xAA, 0xAA, 0xCB, 0x20};
  for (size_t i = 420; i + 4 <= len; i++) {
    if (memcmp(buf + i, pat, 4) == 0) return true;
  }
  return false;
}

int main() {
  /* ---- 用例 1: 单条批量 == 单帧路径 (逐字节) ---- */
  {
    struct tf_batch_msg m1;
    m1.capcode = 1000; m1.msg = "1000";
    m1.type = TF_MESSAGE_TYPE_ALPHA; m1.config = NULL;
    uint8_t b1[2048], b2[2048];
    int err = 0;
    size_t n1 = tf_encode_flex_message("1000", 1000, b1, sizeof(b1), &err);
    size_t n2 = tf_encode_flex_batch(&m1, 1, b2, sizeof(b2), &err, NULL);
    CHECK(err == 0 && n1 == n2 && memcmp(b1, b2, n1) == 0,
          "单条 ALPHA 批量 == 单帧路径",
          (n1 == n2 ? "内容不一致" : "长度不一致"));
  }
  {
    struct tf_batch_msg m1;
    m1.capcode = 1000; m1.msg = "1000";
    m1.type = TF_MESSAGE_TYPE_NUMERIC; m1.config = NULL;
    uint8_t b1[2048], b2[2048];
    int err = 0;
    size_t n1 = tf_encode_flex_numeric_message("1000", 1000, b1, sizeof(b1), &err);
    size_t n2 = tf_encode_flex_batch(&m1, 1, b2, sizeof(b2), &err, NULL);
    CHECK(err == 0 && n1 == n2 && memcmp(b1, b2, n1) == 0,
          "单条 NUMERIC 批量 == 单帧路径",
          (n1 == n2 ? "内容不一致" : "长度不一致"));
  }

  /* ---- 用例 2: 11 地址 ALPHA 批量 (1000..1010) = 1 帧 ---- */
  {
    struct tf_batch_msg ms[11];
    uint8_t out[2048];
    std::vector<uint32_t> caps;
    std::vector<std::string> texts;
    int err = 0;
    for (int i = 0; i < 11; i++) {
      ms[i].capcode = 1000 + i;
      ms[i].msg = "1000";  /* 内容不要求 = 地址, 这里统一 "1000" 便于比对 */
      ms[i].type = TF_MESSAGE_TYPE_ALPHA;
      ms[i].config = NULL;
      caps.push_back(1000 + i);
      texts.push_back("1000");
    }
    size_t n = tf_encode_flex_batch(ms, 11, out, sizeof(out), &err, NULL);
    if (err != 0) { printf("  [FAIL] 11 地址批量: err=%d\n", err); g_fail++; }
    else {
      size_t frames = (n - 420) / 375;
      std::string derr;
      bool ok = (frames == 1 && (n - 420) % 375 == 0) &&
                (check_frame(out, 0, caps, texts, 0, &derr) == 0);
      CHECK(ok, "11 地址 ALPHA 批量 = 1 帧且布局正确", derr.c_str());
      CHECK(!ers_elsewhere(out, n), "ERS 只出现一次 (帧外无重复)", "");
    }
  }

  /* ---- 用例 3: 55 地址 ALPHA 批量 = 4 帧, 全部 frame=0 ---- */
  {
    struct tf_batch_msg ms[55];
    char buf[55][8];
    uint8_t out[2048];
    int err = 0;
    for (int i = 0; i < 55; i++) {
      snprintf(buf[i], sizeof(buf[i]), "%lu", (unsigned long)(1000 + i));
      ms[i].capcode = 1000 + i; ms[i].msg = buf[i];
      ms[i].type = TF_MESSAGE_TYPE_ALPHA; ms[i].config = NULL;
    }
    size_t n = tf_encode_flex_batch(ms, 55, out, sizeof(out), &err, NULL);
    size_t frames = (n - 420) / 375;
    if (err != 0 || (n - 420) % 375 != 0) {
      printf("  [FAIL] 55 地址批量: err=%d len=%u\n", err, (unsigned)n);
      g_fail++;
    } else {
      bool fw_ok = true;
      for (size_t f = 0; f < frames; f++) {
        uint32_t fiw = rd32(out + 420 + f * 375 + 14);  /* S1=14, FIW 紧随 */
        uint32_t dw = tf_decode_word_data(fiw);
        if (((dw >> 8) & 0x7F) != 0 || ((dw >> 4) & 0xF) != 0) fw_ok = false;
      }
      char dbg[64];
      snprintf(dbg, sizeof(dbg), "frames=%u", (unsigned)frames);
      CHECK(frames == 4 && fw_ok, "55 地址 ALPHA = 4 帧且 FIW 全 frame=0", dbg);
      /* 逐帧结构与单帧比对 (帧边界以各帧实际 s_vfield 为准) */
      bool all = true;
      std::string derr;
      size_t idx = 0;
      for (size_t f = 0; f < frames && all; f++) {
        uint32_t fw[WORDS_PER_FRAME];
        deinterleave(fw, frame_data(out, f));
        uint32_t biw = tf_decode_word_data(fw[0]);
        uint32_t naddr = ((biw >> 10) & 0x3F) - 1;
        std::vector<uint32_t> caps;
        std::vector<std::string> texts;
        for (size_t k = idx; k < idx + naddr && k < 55; k++) {
          char b[8]; snprintf(b, sizeof(b), "%lu", (unsigned long)(1000 + k));
          caps.push_back(1000 + k); texts.push_back(b);
        }
        if (check_frame(out, f, caps, texts, 0, &derr) != 0) { all = false; break; }
        idx += naddr;
      }
      CHECK(all && idx == 55, "55 地址 ALPHA 逐帧布局/消息字正确", derr.c_str());
    }
  }

  /* ---- 用例 4: 55 地址 NUMERIC 批量 = 2 帧 ---- */
  {
    struct tf_batch_msg ms[55];
    char buf[55][8];
    uint8_t out[2048];
    int err = 0;
    for (int i = 0; i < 55; i++) {
      snprintf(buf[i], sizeof(buf[i]), "%lu", (unsigned long)(2000 + i));
      ms[i].capcode = 2000 + i; ms[i].msg = buf[i];
      ms[i].type = TF_MESSAGE_TYPE_NUMERIC; ms[i].config = NULL;
    }
    size_t n = tf_encode_flex_batch(ms, 55, out, sizeof(out), &err, NULL);
    size_t frames = (n - 420) / 375;
    char dbg[64];
    snprintf(dbg, sizeof(dbg), "frames=%u", (unsigned)frames);
    CHECK(err == 0 && (n - 420) % 375 == 0 && frames <= 2,
          "55 地址 NUMERIC 批量 ≤ 2 帧", dbg);
  }

  /* ---- 用例 5: 错误路径 ---- */
  {
    struct tf_batch_msg m1;
    m1.capcode = 2101249ULL; m1.msg = "x";  /* 长地址 (非法域起始) */
    m1.type = TF_MESSAGE_TYPE_ALPHA; m1.config = NULL;
    uint8_t out[2048]; int err = 0;
    size_t n = tf_encode_flex_batch(&m1, 1, out, sizeof(out), &err, NULL);
    CHECK(err == -TF_LONG_ADDR_IN_BATCH && n == 0,
          "长地址批量 → TF_LONG_ADDR_IN_BATCH", "");
  }
  {
    struct tf_batch_msg m1;
    m1.capcode = 1000; m1.msg = "1000";
    m1.type = TF_MESSAGE_TYPE_ALPHA; m1.config = NULL;
    uint8_t out[700]; int err = 0;
    size_t n = tf_encode_flex_batch(&m1, 1, out, sizeof(out), &err, NULL);
    CHECK(err == -TF_INVALID_FLEXBUFFER && n == 0,
          "缓冲区过小 → TF_INVALID_FLEXBUFFER", "");
  }
  {
    struct tf_batch_msg m1;
    m1.capcode = 1000; m1.msg = NULL;  /* tone-only */
    m1.type = TF_MESSAGE_TYPE_ALPHA; m1.config = NULL;
    uint8_t out[2048]; int err = 0;
    size_t n = tf_encode_flex_batch(&m1, 1, out, sizeof(out), &err, NULL);
    uint32_t fw[WORDS_PER_FRAME];
    deinterleave(fw, frame_data(out, 0));
    uint32_t biw = tf_decode_word_data(fw[0]);
    uint32_t s_v = (biw >> 10) & 0x3F;
    uint32_t vec = tf_decode_word_data(fw[s_v]);
    bool tone_ok = err == 0 && n == 795 && s_v == 2 && ((vec >> 4) & 0x7) == 2;
    CHECK(tone_ok, "tone-only 批量可用 (vector 类型 010)", "");
  }

  /* ---- 用例 6: FlexEncoder::encodeBatch 封装层 ---- */
  {
    FlexEncoder enc;
    FlexEncoder::BatchMsg ms[11];
    for (int i = 0; i < 11; i++) {
      ms[i].capcode = 1000 + i; ms[i].msg = "1000";
      ms[i].type = FlexEncoder::ALPHA;
    }
    size_t encd = 0;
    size_t len = enc.encodeBatch(ms, 11, &encd);
    CHECK(!enc.hasError() && len == 795 && encd == 11,
          "FlexEncoder::encodeBatch 11 条 ALPHA 全部编码", "");
  }
  {
    FlexEncoder enc;
    FlexEncoder::BatchMsg ms[56];
    for (int i = 0; i < 56; i++) {
      ms[i].capcode = 1000 + i; ms[i].msg = "1000";
      ms[i].type = FlexEncoder::ALPHA;
    }
    size_t encd = 99;
    size_t len = enc.encodeBatch(ms, 56, &encd);
    CHECK(len == 0 && enc.hasError() && encd == 0,
          "FlexEncoder::encodeBatch 超上限整批拒绝", "");
  }
  {
    FlexEncoder enc;
    FlexEncoder::BatchMsg ms[1];
    ms[0].capcode = 2101249ULL; ms[0].msg = "x";
    ms[0].type = FlexEncoder::ALPHA;
    size_t encd = 99;
    size_t len = enc.encodeBatch(ms, 1, &encd);
    CHECK(len == 0 && enc.getError() == -TF_LONG_ADDR_IN_BATCH && encd == 0,
          "FlexEncoder::encodeBatch 长地址错误码传播", "");
  }

  if (g_fail) { printf("FAILED: %d\n", g_fail); return 1; }
  printf("ALL PASS\n");
  return 0;
}
"""


def compile_harness(out: str) -> None:
    cmd = ["g++", "-std=c++11", "-O0",
           "-I", os.path.join(REPO, "lib/Flex"),
           "-x", "c++", "-", "-o", out]
    p = subprocess.run(cmd, input=HARNESS, capture_output=True, text=True)
    if p.returncode != 0:
        print("编译 harness 失败:\n", p.stderr)
        sys.exit(1)


def main():
    harness = os.path.join(tempfile.gettempdir(), "flex_batch_harness")
    compile_harness(harness)
    p = subprocess.run([harness], capture_output=True, text=True)
    print(p.stdout, end="")
    if p.returncode != 0:
        print("FAILED: harness 退出码", p.returncode)
        if p.stderr:
            print(p.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
