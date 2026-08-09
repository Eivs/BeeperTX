#!/usr/bin/env python3
"""
主机端验证: PocsagEncoder::encodeBatchMulti 单前导码连续多 Batch 调度

项目无单元测试 (CLAUDE.md), 此脚本在宿主机编译 Pocsag.cpp 并验证:
  1. 前 18 CW 为前导码 POCSAG_PREAMBLE (0xAAAAAAAA)
  2. 每 Batch = [SYNC] + 16 槽 (17 CW), 以 SYNC (0x7CD215D8) 开头
  3. 地址码字放 (addr & 0x07)*2 槽, 消息码字紧随其后可跨 SYNC 延续
  4. 地址码字可解回地址 (bit31=0), BCH/偶校验通过
  5. 消息码字为合法 POCSAG 消息 (bit31=1, BCH 校验通过)
  6. 尾部 16 个 IDLE (0x7A89C197) 收尾
  7. 消息内容 = 地址十进制串 (ALPHA 模式逐字解码比对)
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


# ---- encodeBatchMulti 验证 ----
HARNESS_MULTI = r"""
#include "Pocsag.h"
#include <cstdio>
#include <cstdint>
#include <cstring>

int main(int argc, char** argv) {
  if (argc < 4) { fprintf(stderr, "usage: harness_multi mode invert func addr...\n"); return 1; }
  int mode = atoi(argv[1]);
  int invert = atoi(argv[2]);
  int func = atoi(argv[3]);
  int n = argc - 4;
  uint32_t addrs[512];
  bool skipped[512];
  for (int i = 0; i < n && i < 512; i++) addrs[i] = strtoul(argv[4 + i], nullptr, 10);
  PocsagEncoder enc;
  int addrEncoded = -1;
  int len = enc.encodeBatchMulti(addrs, n, func, (DisplayMode)mode,
                                 (invert != 0), &addrEncoded, skipped);
  const unsigned long* bb = enc.getBaseband();
  printf("LEN %d ADDRENC %d\n", len, addrEncoded);
  for (int i = 0; i < n; i++) printf("SKIP%d %d\n", i, skipped[i] ? 1 : 0);
  for (int i = 0; i < len; i++) printf("%08lX\n", bb[i]);
  return 0;
}
"""


def flip7(v: int) -> int:
    """7-bit 位反转 (与 Pocsag.cpp flip7bitOrder 一致)."""
    out = 0
    for _ in range(7):
        out <<= 1
        out |= (v & 1)
        v >>= 1
    return out


def decode_alpha_msg(msg_words):
    """解码 ALPHA 消息码字序列 → 字符串 (7-bit 反转, EOT 0x04 结束)."""
    bits = []
    for w in msg_words:
        m20 = (w >> 11) & 0xFFFFF  # 20-bit 消息 (MSB-first)
        for b in range(19, -1, -1):
            bits.append((m20 >> b) & 1)
    chars = []
    i = 0
    while i + 7 <= len(bits):
        val = 0
        for k in range(7):
            val = (val << 1) | bits[i + k]
        c = flip7(val)
        if c == 0x04:  # EOT
            break
        if c not in (0x0E, 0x0F):  # Shift Out/In 字符集切换控制符
            chars.append(chr(c))
        i += 7
    return "".join(chars)


def check_multi(addrs, words, skip, invert, mode, expect_encoded):
    """encodeBatchMulti 单前导码连续多 Batch 流校验.

    结构: 18 前导 + N batch × (SYNC + 16 槽) + 尾部 16 IDLE.
    消息码字可跨 SYNC 延续 (ITU-R M.584). 模拟寻呼机逐码字扫描:
    地址 (bit31=0 且非 IDLE/SYNC) 后收集连续 bit31=1 码字为消息,
    遇 bit31=0 结束.
    """
    w = [~x & 0xFFFFFFFF if invert else x for x in words]
    ok = True
    # 1. 前导
    if w[:18] != [PREAMBLE] * 18:
        print("  FAIL: 前导码缺失/错误"); ok = False
    body = w[18:]
    # 2. 尾部 16 IDLE
    if body[-16:] != [IDLE] * 16:
        print("  FAIL: 尾部 16 不全是 IDLE"); ok = False
    body = body[:-16]
    # 3. 结构: 每 17 CW = SYNC + 16 槽
    if len(body) % 17 != 0:
        print(f"  FAIL: 正文长度 {len(body)} 不是 17 的倍数"); ok = False
    nbatch = len(body) // 17
    for b in range(nbatch):
        if body[b * 17] != SYNC:
            print(f"  FAIL: batch {b} 不以 SYNC 开头"); ok = False

    # 4. 模拟寻呼机: 扫描连续码字流 (跨 batch)
    # decode_addr 丢失地址低 3 位 (1000..1007 → 1000), 事件按 (key, 槽位) 定位
    events = []   # (addr_key, pos_in_body, [msg_cw...])
    last_key = None
    last_pos = None
    for idx, cw in enumerate(body):
        if cw == SYNC:
            continue
        if cw & 0x80000000:          # 消息码字
            if last_key is not None:
                events[last_idx][2].append(cw)
        else:                        # 地址 / IDLE
            last_key = None
            if cw != IDLE:           # 地址码字 (bit31=0)
                a = decode_addr(cw)
                last_key = a
                last_idx = len(events)
                events.append([a, idx, []])

    # 5. 校验: 全部地址在场 + 槽位正确 + 消息内容 = 地址串
    n_enc = 0
    for i, a in enumerate(addrs):
        if skip[i]:
            continue
        n_enc += 1
        key = a & ~0x7
        slot = (a & 0x07) * 2
        ev = None
        for e in events:             # 每个 (key, 槽) 事件唯一匹配一个地址
            if e[0] == key and (e[1] % 17 - 1) == slot:
                ev = e; break
        if ev is None:
            print(f"  FAIL: 缺地址 {a}"); ok = False; continue
        pos, mw = ev[1], ev[2]
        if mode == 1:  # ALPHANUMERIC
            text = decode_alpha_msg(mw)
            if text != str(a):
                print(f"  FAIL: 地址 {a} 消息 '{text}' 期望 '{str(a)}' (mw={len(mw)})"); ok = False
        else:
            if not mw:
                print(f"  FAIL: 地址 {a} NUMERIC 无消息"); ok = False
        if not verify_bch_parity(body[pos]) or any(not verify_bch_parity(m) for m in mw):
            print(f"  FAIL: 地址 {a} 码字 BCH/奇偶校验失败"); ok = False
    if n_enc != expect_encoded:
        print(f"  FAIL: 编码地址数 {n_enc} 期望 {expect_encoded}"); ok = False
    return ok


def run_multi(harness_bin, mode, invert, func, addrs):
    cmd = [harness_bin, str(mode), str(invert), str(func)] + [str(a) for a in addrs]
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        raise RuntimeError(f"harness_multi 运行失败: {p.stderr}")
    lines = p.stdout.strip().splitlines()
    if not lines:
        return (0, [], {})
    header = lines[0].split()
    if header[0] != "LEN":
        raise RuntimeError(f"harness_multi 输出异常: {lines[0]}")
    length = int(header[1])
    addr_enc = int(header[3])
    skip = {}
    idx = 1
    while idx < len(lines) and lines[idx].startswith("SKIP"):
        parts = lines[idx].split()
        skip[int(parts[0][4:])] = (parts[1] == "1")
        idx += 1
    words = [int(l, 16) for l in lines[idx:]]
    assert len(words) == length, f"长度不符 {len(words)} vs {length}"
    return (addr_enc, words, skip)


def main():
    multi_bin = os.path.join(tempfile.gettempdir(), "pocsag_batch_multi_harness")
    compile_harness(HARNESS_MULTI, multi_bin)

    failures = 0

    # ---- encodeBatchMulti: 单前导码连续多 Batch, 全地址进批量, 消息跨 SYNC ----
    # 用例 M1: ALPHA 4 地址 (帧 0-3), 消息 3 CW 跨 batch 避让, 消息 = 地址串
    addrs4 = list(range(1000, 1004))
    ae, words, skip = run_multi(multi_bin, 1, 0, 0, addrs4)
    if ae == 4 and not any(skip.values()) and check_multi(addrs4, words, skip, False, 1, 4):
        print("  [用例M1] ALPHA 4 地址全批量 (跨 batch 避让): PASS")
    else:
        print(f"  [用例M1] ALPHA 4 地址全批量: FAIL (ae={ae}, skip={skip})"); failures += 1

    # 用例 M2: ALPHA 8 地址 (帧 0-7), frame-7 消息跨 batch, 不再跳过
    addrs8 = list(range(1000, 1008))
    ae, words, skip = run_multi(multi_bin, 1, 0, 0, addrs8)
    if ae == 8 and not any(skip.values()) and check_multi(addrs8, words, skip, False, 1, 8):
        print("  [用例M2] ALPHA 8 地址全批量 (frame7 跨 batch): PASS")
    else:
        print(f"  [用例M2] ALPHA 8 地址全批量: FAIL (ae={ae}, skip={skip})"); failures += 1

    # 用例 M3: ALPHA 8 地址反相
    ae, words, skip = run_multi(multi_bin, 1, 1, 1, addrs8)
    if ae == 8 and not any(skip.values()) and check_multi(addrs8, words, skip, True, 1, 8):
        print("  [用例M3] ALPHA 8 地址全批量反相: PASS")
    else:
        print(f"  [用例M3] ALPHA 8 地址全批量反相: FAIL (ae={ae})"); failures += 1

    # 用例 M4: NUMERIC 1 CW 全部批量 (帧 0-7 均可放 1 CW)
    addrs16 = list(range(2000, 2016))
    ae, words, skip = run_multi(multi_bin, 0, 0, 0, addrs16)
    if ae == 16 and not any(skip.values()) and check_multi(addrs16, words, skip, False, 0, 16):
        print("  [用例M4] NUMERIC 16 地址全批量: PASS")
    else:
        print(f"  [用例M4] NUMERIC 16 地址全批量: FAIL (ae={ae})"); failures += 1

    # 用例 M5: 6 位地址 ALPHA (消息 3 CW) 全部批量, frame 7 跨 2 槽
    addrs6 = list(range(100000, 100008))
    ae, words, skip = run_multi(multi_bin, 1, 0, 0, addrs6)
    if ae == 8 and not any(skip.values()) and check_multi(addrs6, words, skip, False, 1, 8):
        print("  [用例M5] 6位地址 ALPHA 8 地址全批量: PASS")
    else:
        print(f"  [用例M5] 6位地址 ALPHA 8 地址全批量: FAIL (ae={ae}, skip={skip})"); failures += 1

    # 用例 M6: 空地址数组 → 0
    ae, words, skip = run_multi(multi_bin, 1, 0, 0, [])
    if ae == 0:
        print("  [用例M6] 空数组返回 0: PASS")
    else:
        print(f"  [用例M6] 空数组返回 0: FAIL (ae={ae})"); failures += 1

    # 用例 M7: 55 地址单前导码连续流, 全部编码, 无 frame-7 补发
    # 贪心实测 16 batch: len = 18 + 16×17 + 16 = 306 CW (对比旧方案 850+ 补发)
    addrs55 = list(range(1, 56))
    ae, words, skip = run_multi(multi_bin, 1, 0, 0, addrs55)
    if (ae == 55 and not any(skip.values())
            and len(words) <= 18 + 17 * 18 + 16   # ≤ 18 batch (340 CW), 证明密度提升
            and check_multi(addrs55, words, skip, False, 1, 55)):
        print(f"  [用例M7] 55 地址单前导连续流 (全编码, {len(words)} CW): PASS")
    else:
        print(f"  [用例M7] 55 地址单前导连续流: FAIL (ae={ae}, skip={skip}, len={len(words)})"); failures += 1

    # 用例 M8: 7 位地址 (消息 4 CW) 帧 6/7, 消息跨 SYNC, 内容完整
    # 帧6 (t=12) 消息: 槽 13,14,15 + 下批 0 (跨 1 个 SYNC); 帧7 (t=14): 跨 2 槽
    # 4 地址贪心避让 → 约 5 batch, 全部消息跨 SYNC 延续
    addrsL = [2097150, 2097151, 2097142, 2097143]  # 帧 6,7,6,7; ≤ 21 bit 合法
    ae, words, skip = run_multi(multi_bin, 1, 0, 0, addrsL)
    if ae == 4 and not any(skip.values()) and check_multi(addrsL, words, skip, False, 1, 4):
        print("  [用例M8] 7位地址 消息跨 SYNC: PASS")
    else:
        print(f"  [用例M8] 7位地址 消息跨 SYNC: FAIL (ae={ae}, skip={skip}, len={len(words)})"); failures += 1

    print()
    if failures:
        print(f"FAILED: {failures} 个用例失败")
        sys.exit(1)
    print("ALL PASS")


if __name__ == "__main__":
    main()
