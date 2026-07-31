#!/usr/bin/env python3
"""
主机端验证: PocsagEncoder::encodeBatch 帧结构

项目无单元测试 (CLAUDE.md), 此脚本在宿主机编译 Pocsag.cpp 并验证:
  1. 前 18 CW 为前导码 POCSAG_PREAMBLE (0xAAAAAAAA)
  2. 每组 = [SYNC] + 8 帧 × 2 CW (17 CW), 组以 SYNC (0x7CD215D8) 开头
  3. 地址码字放在 (addr & 0x07) 对应帧槽, 消息码字紧跟其后
  4. 地址码字可解回地址 (bit31=0, 地址 <<3 后异或/取位)
  5. 消息码字为合法 POCSAG 消息 (bit31=1, BCH 校验通过)
  6. 组尾 / 整批尾部用 IDLE (0x7A89C197) 填充
  7. 6 位及以上地址 / 帧冲突 / ALPHANUMERIC → 返回 0 (降级)
  8. invert 时所有码字取反

用法: python3 tools/verify_pocsag_batch.py
退出码: 0=全部通过, 1=存在失败
"""

import subprocess
import sys
import os
import tempfile

PREAMBLE = 0xAAAAAAAA
SYNC = 0x7CD215D8
IDLE = 0x7A89C197
GEN_POLY = 0x769  # BCH(31,21) 生成多项式

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def gen_msg_code(msg_bin: int) -> int:
    """复刻 Pocsag.cpp 的 genMsgCode + calcCRC + evenCheck."""
    data = msg_bin
    data <<= 10
    mod = 0x76900000
    cmp_val = 0x40000000
    while cmp_val >= 0b10000000000:
        if data & cmp_val:
            data ^= mod
        mod >>= 1
        cmp_val >>= 1
    # data 现在是 BCH 码字位 (bit31..bit10 = 原始数据+CRC 处理结果)
    # genMsgCode 具体实现需与源码核对, 这里按 POCSAG 标准生成
    # 消息码字: bit31=1, bit30..11 = 20bit 消息, bit10..1 = BCH, bit0 = 偶校验
    bch = data & 0x3FF
    code = (1 << 31) | ((msg_bin & 0xFFFFF) << 11) | (bch << 1)
    parity = bin(code & ~1).count("1") & 1
    return code | parity


def verify_bch_parity(word: int) -> bool:
    """校验 BCH 码字 (POCSAG 标准校验: 对 31bit 数据做 BCH, 校验位在 bit10..1)."""
    # 提取 21-bit 数据 (bit31..bit11) 与 10-bit BCH (bit10..bit1)
    # POCSAG: 校验 = CRC 前 21 bit (含 bit31)
    data21 = word >> 11
    bch_recv = (word >> 1) & 0x3FF
    # 计算校验: 用生成多项式 0x769 对 data21 取模
    reg = data21 << 10
    for bit in range(30, 9, -1):
        if reg & (1 << bit):
            reg ^= GEN_POLY << (bit - 10)
    bch_calc = reg & 0x3FF
    # 偶校验
    parity = bin(word >> 1).count("1") & 1
    return (bch_calc == bch_recv) and (parity == (word & 1))


def decode_addr(word: int) -> int:
    """从地址码字解回地址: bit31=0, bit30..13 = addr>>3, bit12..11 = func."""
    return ((word >> 13) & 0x3FFFF) << 3


def decode_msg(word: int) -> int:
    """消息码字: bit31=1, bit30..11 = 20-bit 消息."""
    return (word >> 11) & 0xFFFFF


def compile_harness(src: str, out: str) -> None:
    # Pocsag.h include <Arduino.h> (项目用 Arduino 框架), 宿主机测试用 stub 替代
    stub_dir = os.path.join(tempfile.gettempdir(), "pocsag_stub")
    os.makedirs(stub_dir, exist_ok=True)
    stub_h = os.path.join(stub_dir, "Arduino.h")
    with open(stub_h, "w") as f:
        f.write("// 宿主机测试 stub\n"
                "#pragma once\n"
                "#include <stdint.h>\n"
                "#include <stddef.h>\n"
                "#include <string.h>\n"
                "#include <stdio.h>\n"
                "#include <stdlib.h>\n"
                "typedef uint8_t byte;\n"
                "typedef uint16_t word;\n"
                "#define F(x) (x)\n"
                "#define PROGMEM\n"
                "#define PSTR(x) (x)\n"
                "static inline unsigned long millis() { return 0; }\n"
                "static inline void delay(unsigned long) {}\n")

    cmd = ["g++", "-std=c++11", "-O0",
           "-I", os.path.join(REPO, "lib/Pocsag"),
           "-I", stub_dir,
           os.path.join(REPO, "lib/Pocsag/Pocsag.cpp"),
           "-x", "c++", "-", "-o", out]
    p = subprocess.run(cmd, input=src, capture_output=True, text=True)
    if p.returncode != 0:
        print("编译 harness 失败:\n", p.stderr)
        sys.exit(1)


HARNESS = r"""
#include "Pocsag.h"
#include <cstdio>
#include <cstdint>

static void dump(const uint32_t* addrs, int n, int mode, int invert, int func) {
  PocsagEncoder enc;
  int addrEncoded = -1;
  int len = enc.encodeBatch(addrs, n, func,
                            (DisplayMode)mode, (invert != 0), &addrEncoded);
  if (len <= 0) { printf("RET0 addrEncoded=%d\n", addrEncoded); return; }
  const unsigned long* bb = enc.getBaseband();
  printf("LEN %d ADDRENC %d\n", len, addrEncoded);
  for (int i = 0; i < len; i++) printf("%08lX\n", bb[i]);
}

int main(int argc, char** argv) {
  if (argc < 4) { fprintf(stderr, "usage: harness mode invert func addr...\n"); return 1; }
  int mode = atoi(argv[1]);
  int invert = atoi(argv[2]);
  int func = atoi(argv[3]);
  int n = argc - 4;
  uint32_t addrs[512];
  for (int i = 0; i < n && i < 512; i++) addrs[i] = strtoul(argv[4 + i], nullptr, 10);
  dump(addrs, n, mode, invert, func);
  return 0;
}
"""


def run_harness(harness_bin: str, mode: int, invert: int, func: int, addrs):
    cmd = [harness_bin, str(mode), str(invert), str(func)] + [str(a) for a in addrs]
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        raise RuntimeError(f"harness 运行失败: {p.stderr}")
    lines = p.stdout.strip().splitlines()
    if not lines:
        return (0, [])
    if lines[0].startswith("RET0"):
        return (0, [])
    header = lines[0].split()
    if header[0] != "LEN":
        raise RuntimeError(f"harness 输出异常: {lines[0]}")
    length = int(header[1])
    addr_enc = int(header[3])
    words = [int(l, 16) for l in lines[1:]]
    assert len(words) == length, f"长度不符 {len(words)} vs {length}"
    return (addr_enc, words)


def check_struct(addrs, words, invert):
    """校验帧结构 (不反相视角)."""
    w = [~x & 0xFFFFFFFF if invert else x for x in words]
    ok = True

    # 1. 前 18 前导
    if w[:18] != [PREAMBLE] * 18:
        print("  FAIL: 前导码缺失/错误"); ok = False

    body = w[18:]
    n = len(addrs)
    groups = (n + 7) // 8

    # 2. 每组 17 CW 且以 SYNC 开头
    expected_len = 18 + groups * 17 + 16
    if len(w) != expected_len:
        print(f"  FAIL: 总长度 {len(w)} 期望 {expected_len}"); ok = False

    for g in range(groups):
        base = 18 + g * 17
        if body[g * 17] != SYNC:
            print(f"  FAIL: 组 {g} 不以 SYNC 开头 (got {body[g*17]:08X})"); ok = False

        group_addrs = addrs[g * 8:(g + 1) * 8]
        frame_slots = body[g * 17 + 1:(g + 1) * 17]

        # 3. 地址帧槽对应 addr&7, 消息紧跟
        for a in group_addrs:
            frame = a & 0x07
            slot = frame * 2
            addr_word = frame_slots[slot]
            msg_word = frame_slots[slot + 1]
            if addr_word & 0x80000000:
                print(f"  FAIL: addr {a} 槽 {slot} 是消息码字 (bit31=1)"); ok = False
            if decode_addr(addr_word) != (a & ~0x7):
                print(f"  FAIL: addr {a} 码字解回 {decode_addr(addr_word)}"); ok = False
            if not (msg_word & 0x80000000):
                print(f"  FAIL: addr {a} 消息槽 {slot+1} 不是消息码字"); ok = False
            if not verify_bch_parity(addr_word) or not verify_bch_parity(msg_word):
                print(f"  FAIL: addr {a} 码字 BCH/奇偶校验失败"); ok = False

    # 4. 组内未用槽 + 尾部 16 用 IDLE
    for g in range(groups):
        used = [a & 0x07 for a in addrs[g * 8:(g + 1) * 8]]
        for f in range(8):
            if f not in used:
                slot = f * 2
                if frame_slots_ok(w, 18 + g * 17 + 1 + slot, IDLE, invert) is False:
                    print(f"  FAIL: 组 {g} 未用帧 {f} 槽应为 IDLE"); ok = False

    tail = w[len(w) - 16:]
    if tail != [IDLE] * 16:
        print(f"  FAIL: 尾部 16 不全是 IDLE"); ok = False

    return ok


def frame_slots_ok(w, idx, val, invert):
    v = ~w[idx] & 0xFFFFFFFF if invert else w[idx]
    return v == val


def check_reject(harness_bin, mode, addrs):
    """应返回 0 (降级)."""
    (addr_enc, _) = run_harness(harness_bin, mode, 0, 0, addrs)
    return addr_enc == 0


def main():
    harness_bin = os.path.join(tempfile.gettempdir(), "pocsag_batch_harness")
    compile_harness(HARNESS, harness_bin)

    failures = 0

    # 用例 1: 连续 8 地址, NUMERIC, 正相
    addrs = list(range(1000, 1008))
    _, words = run_harness(harness_bin, 0, 0, 0, addrs)
    if not check_struct(addrs, words, invert=False):
        print("  [用例1] 连续8地址正相: FAIL"); failures += 1
    else:
        print("  [用例1] 连续8地址正相: PASS")

    # 用例 2: 连续 8 地址, 反相
    _, words = run_harness(harness_bin, 0, 1, 0, addrs)
    if not check_struct(addrs, words, invert=True):
        print("  [用例2] 连续8地址反相: FAIL"); failures += 1
    else:
        print("  [用例2] 连续8地址反相: PASS")

    # 用例 3: 2 组 (16 地址), 连续
    addrs16 = list(range(2000, 2016))
    _, words = run_harness(harness_bin, 0, 0, 0, addrs16)
    if not check_struct(addrs16, words, invert=False):
        print("  [用例3] 16地址2组: FAIL"); failures += 1
    else:
        print("  [用例3] 16地址2组: PASS")

    # 用例 4: 非对齐起始地址 (跨帧边界)
    addrs_unal = list(range(1234, 1242))
    _, words = run_harness(harness_bin, 0, 0, 1, addrs_unal)
    if not check_struct(addrs_unal, words, invert=False):
        print("  [用例4] 非对齐起始: FAIL"); failures += 1
    else:
        print("  [用例4] 非对齐起始: PASS")

    # 用例 5: 降级 - 6 位地址
    if check_reject(harness_bin, 0, [123456]):
        print("  [用例5] 6位地址降级: PASS")
    else:
        print("  [用例5] 6位地址降级: FAIL"); failures += 1

    # 用例 6: 降级 - 帧冲突 (非连续)
    if check_reject(harness_bin, 0, [1000, 1008]):
        print("  [用例6] 帧冲突降级: PASS")
    else:
        print("  [用例6] 帧冲突降级: FAIL"); failures += 1

    # 用例 7: 降级 - ALPHANUMERIC
    if check_reject(harness_bin, 1, [1000]):
        print("  [用例7] ALPHA模式降级: PASS")
    else:
        print("  [用例7] ALPHA模式降级: FAIL"); failures += 1

    # 用例 8: 组内非连续但帧号唯一的地址 (如 1000, 1008 帧号均为0 → 应降级)
    if check_reject(harness_bin, 0, [1000, 1008]):
        print("  [用例8] 组内同帧降级: PASS")
    else:
        print("  [用例8] 组内同帧降级: FAIL"); failures += 1

    # 用例 9: MAX_BASEBAND 截断防护 — 大量地址 (400 个) 不应溢出缓冲区
    # 每组 17 CW (SYNC+16), MAX_BASEBAND=768:
    #   bandPtr 从 18 起, 条件 bandPtr < 768-18=750 → 最多 44 组 = 352 地址
    addrs400 = list(range(0, 400))  # 400 连续地址, 帧号 0..7 每 8 个循环, 组内不冲突
    (addr_enc, words) = run_harness(harness_bin, 0, 0, 0, addrs400)
    if addr_enc == 352 and len(words) <= 768:
        print("  [用例9] MAX_BASEBAND 截断防护: PASS")
    else:
        print(f"  [用例9] MAX_BASEBAND 截断防护: FAIL (addr_enc={addr_enc}, len={len(words)})")
        failures += 1

    print()
    if failures:
        print(f"FAILED: {failures} 个用例失败")
        sys.exit(1)
    print("ALL PASS")


if __name__ == "__main__":
    main()
