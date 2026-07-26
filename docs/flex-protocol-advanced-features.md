# FLEX 协议高级特性调研文档

> 调研日期: 2026-07-23
> 参考标准: ARIB STD-43A (RCR STD-43), FLEX-93001 Issue G1.8
> 当前 flex.h 版本: 基于 Davidson Francis 的 minimal single-header 实现, 单帧/单网络

---

## 1. 概述

当前 `lib/Flex/flex.h` 是一个极简 FLEX 编码器, 只实现最基本的点对点寻呼场景:
- 固定 cycle=0, frame=0
- 无 SSID/NID 支持
- 无系统 collapse (节电周期) 支持
- 无时间信息广播

本文档详细描述 FLEX 协议中这些缺失的高级特性, 供后续实现参考。

---

## 2. FLEX 帧结构基础

### 2.1 时间层次

```
1 小时 = 15 个 Cycle (编号 0-14)
1 Cycle = 128 个 Frame (编号 0-127) = 4 分钟
1 Frame = 1.875 秒
1 Frame 包含 11 个 Block (1600 bps), 每个 Block 8 个 Word
```

### 2.2 帧内结构 (1600 bps, 单相)

```
┌──────┬──────┬──────┬──────┬──────┬──────────────────────────┐
│ Sync1│ FIW  │ Sync2│ BIW  │Addr..│ 11 Blocks × 8 Words      │
│      │      │      │      │Vector│ (含 Block Interleaving)   │
└──────┴──────┴──────┴──────┴──────┴──────────────────────────┘
```

- **Sync1**: 112 bits @ 1600 bps, 2-FSK (bit_sync_1 + A1 + B + A1_inv)
- **FIW**: Frame Information Word (32-bit codeword)
- **Sync2**: 40 bits @ 1600 bps (C block)
- **Block 0**: 首 1-4 个 word 为 BIW, 之后是 Address Field, 再后是 Vector Field + Message Field

### 2.3 多相位 (3200/6400 bps)

1600 bps 只有 Phase A。3200 bps 有 Phase A,B。6400 bps 有 Phase A,B,C,D。

寻呼机被分配到特定 phase 以分散负载和节约电池。

---

## 3. Frame Information Word (FIW) — 帧信息字

### 3.1 当前实现

```c
// flex.h:691 — 硬编码为全零
SAVE_WORD(flex_pkt_ptr, create_fiw(0, 0, 0, 0, 0));
//                                 ^  ^  ^  ^  ^
//                              cycle frame n  r  t
```

### 3.2 FIW 完整字段 (ARIB 3.6)

`create_fiw()` 构建的 21-bit 数据字段:

| 位域 | 位宽 | 参数 | 含义 |
|------|------|------|------|
| c3-c0 | 4 | cycle | Cycle 号 (0–14) |
| f6-f0 | 7 | frame | Frame 号 (0–127) |
| n0 | 1 | n | 漫游支持位: 0=仅归属网络, 1=支持漫游 |
| r0 | 1 | r | 多帧传输指示 |
| t3-t0 | 4 | t | 低流量标志 (Low Traffic Flags) |

其中:
- **cycle**: 必须与 GPS 时间同步, Cycle 0 的 Frame 0 对应 GPS 整小时时刻
- **frame**: 128 个帧循环, 寻呼机只在分配的帧号唤醒
- **n (Roaming Support Bit)**: 设为 1 表示本信道遵循漫游放置规则 (需 SSID)
- **t (Low Traffic Flags)**: 指示哪些时分复用相位流量低, Address Field 不出 Block 0

### 3.3 缺失

当前完全缺失 cycle/frame 动态设置, 且 n/r/t 硬编码为 0。

---

## 4. Block Information Word (BIW) — 块信息字

### 4.1 BIW 类型

BIW 是帧内 Block 0 的前几个 word。当前 `flex.h` 只实现了 `create_biw1()` (格式类型 000 的特例, 11 种类型之一):

| f2f1f0 | 类型 | 内容 |
|--------|------|------|
| 000 | Local ID / Coverage Zone | 本地标识 + 覆盖区域 (含 BIW1: 帧结构信息) |
| 001 | 日期 | Month (4b), Day (5b), Year (5b) |
| 010 | 时间 | Second (3b), Minute (6b), Hour (5b) |
| 101 | 系统消息/数据 | Time Zone, DST, 精确秒等 |
| 111 | 国家码 + TMF | Country Code (10b), Traffic Flags (4b) |
| 011/100/110 | 保留 | - |

### 4.2 BIW1 (当前 flex.h 已实现, 但未暴露参数)

```c
static uint32_t create_biw1(uint32_t prio, uint32_t e_biw, uint32_t s_vfield,
                            uint32_t carry, uint32_t collapse) {
```

| 参数 | 位宽 | 含义 |
|------|------|------|
| prio | 4 | 优先地址数量 (0–15), 放在 Address Field 最前面 |
| e_biw | 2 | Address Field 起始位置 (0–3), 即 BIW 占几个 word |
| s_vfield | 6 | Vector Field 起始偏移 (1–63 words) |
| carry | 2 | Carry-on 标志: 0=不跨越帧, 1–3=后续帧数量 |
| collapse | 3 | **系统节电周期**: 0=全部帧, 1=2¹=每 2 帧, …, 7=2⁷=每 128 帧 |

当前调用: `create_biw1(0, 0, 2 + is_long, 0, 0)` — 全部默认值, collapse=0。

### 4.3 缺失

- BIW 格式类型 001/010/101/111 (时间/系统消息/国家码) 完全没有实现
- collapse 参数硬编码为 0 (无系统级节电)
- 不支持 SSID 相关 BIW (BIW000/BIW111)

---

## 5. SSID (Simulcast System ID) — 同播系统标识

### 5.1 概念

SSID 用于标识一个同播系统 (多个发射塔在同一频率上同步发射)。每个 System 分配一个唯一 SSID, 不同频率/区域使用不同 SSID。

**即使在相同频率的不同区域, 也必须使用不同 SSID**, 否则寻呼机会误认为在同一个同播区。

### 5.2 SSID 组成 (28 bits)

| 字段 | 位宽 | 说明 |
|------|------|------|
| LID (Local ID) | 9 bits (0–511) | 在给定国家码和频段内唯一 |
| Coverage Zone | 5 bits (0–31) | 服务区域/覆盖区标识 |
| Country Code | 10 bits (0–1023) | 国家码 (ITU 标准) |
| TMF (Traffic Management Flags) | 4 bits (0–15) | 流量管理标志 |

### 5.3 SSID 在帧中的放置

SSID 通过两个特殊 BIW 传输:

| BIW | 何时发送 | 说明 |
|-----|---------|------|
| **BIW000** | **每帧都发** (0–127), 每 phase | 格式类型 000, 承载部分 SSID 信息 |
| **BIW111** | **仅 Frame 0–3**, 每 phase | 格式类型 111, 承载国家码和部分 SSID |

Phase 分配规则: `Phase = Frame # mod 4`
- Frame 0,4,8,… → Phase A
- Frame 1,5,9,… → Phase B
- Frame 2,6,10,… → Phase C
- Frame 3,7,11,… → Phase D

### 5.4 强制要求

1. Frame 0–3 必须传输 (至少 7.5 秒)
2. FIW 中 n 位必须 = 1
3. GPS 时间同步 (Frame 0 对齐 GPS 整小时)
4. 所有信道时间对齐在 ±1 ms 内 (信道间 ±2 ms)

### 5.5 SSID 变更指令

- 系统 SSID 变更时: 在变更所在 Cycle 的**前 2 和后 2 个 Cycle** 发送变更通知 (共 5 Cycle)
- 之后建议每小时发送一次
- 使用 Operator Messaging Address 1110

---

## 6. NID (Network ID) — 网络标识

### 6.1 概念

NID 允许在**一个 System (SSID) 内**划分多个逻辑网络, 用于:
- 不同运营商共享同一同播系统
- 创建跨区域漫游网络 (同一 NID 可出现在多个 SSID 中)

### 6.2 NID 结构

NID 通过 **Network Address** + **Short Message Vector** (向量类型 010, t1t0 = 00) 承载:

| 字段 | 位宽 | 说明 |
|------|------|------|
| T3–T0 | 4 bits | 流量管理标志 |
| M2–M0 | 3 bits | 乘数 |
| A4–A0 | 5 bits | 服务区号 |

### 6.3 NID 放置规则

NID 出现的 Frame 由以下公式决定:

```
Frame # mod 8 = (M + N + C) mod 8
```

其中:
- **M** = 标称 RF 频率 (kHz) ÷ 25 kHz, 取整数部分
- **N** = NID 的 Network Address 部分
- **C** = 当前 FLEX Cycle 号 (0–14)

NID 所在 phase: `Phase = Frame # mod 4`

### 6.4 NID 漫游示例

```
NID Local 1 → 覆盖区域 A, B, C
NID Local 2 → 覆盖区域 C, D, E
NID Wide 1   → 覆盖区域 A, B, C, D, E, F

寻呼机扫描列表:
  频率 + SSID + NID + Overlap 标志
```

### 6.5 NID 类型

- **NID1–NID8**: 一个 System 最多 8 个 NID
- 每个 NID 可在不同 System 间重用, 形成大范围逻辑漫游网

---

## 7. 节电周期 (Collapse / Battery Saving)

### 7.1 两级 Collapse

| 级别 | 位置 | 位宽 | 说明 |
|------|------|------|------|
| **System Collapse** | BIW1 的 collapse 字段 (3 bits) | 0–7 | 系统级, 对全网寻呼机生效 |
| **Pager Collapse (b)** | 寻呼机 Codeplug | 0–7 | 每台寻呼机独立设置 |

### 7.2 工作原理

寻呼机不监听所有 128 帧, 而只监听满足以下条件的帧:

```
Active Frame = BaseFrame + k × 2^collapse   (k = 0,1,2,…, 结果<128)
```

**示例**: BaseFrame=3, collapse=4 (2⁴=16)
→ 活动帧: 3, 19, 35, 51, 67, 83, 99, 115 (每 ~30 秒醒一次)

### 7.3 优先级规则

- **寻呼机 collapse < 系统 collapse**: 寻呼机更频繁唤醒 (对紧急消息响应更快)
- **系统 collapse < 寻呼机 collapse**: 系统强制更长休眠 (保护电池)
- **默认值**: collapse = 4 (2⁴=16 帧, 约 30 秒)

### 7.4 CAPCODE 中的 Collapse

CAPCODE 格式: `[Address][Collapse][Type][Frame][Roaming]`

示例: Collapse=4 时可省略; 若不用标准值则必须显式指定。

---

## 8. CAPCODE → Frame/Phase 计算

### 8.1 标准计算 (类型 A–L)

```
Frame = ((Address - Offset) / 16) mod 128
Phase = (((Address - Offset) / 4) mod 4)  →  0=A, 1=B, 2=C, 3=D
```

其中 Offset 由 CAPCODE 类型字母决定:

| 类型 | Offset |
|------|--------|
| A, E, I | 0 |
| B, F, J | 1 |
| C, G, K | 2 |
| D, H, L | 3 |

### 8.2 显式指定 (类型 U–Z)

类型 U–Z 的寻呼机, frame 和 phase 在 CAPCODE 中直接指定, 不通过公式计算。

### 8.3 举例

CAPCODE `P1225U2234550121`:
- 地址 = 1225, 类型 = U (单 phase, phase-A)
- Collapse = 2 (每 4 帧活跃)
- Frame = 234 (显式指定)
- 等...

---

## 9. 当前 flex.h 能力矩阵

| 特性 | flex.h 状态 | 备注 |
|------|------------|------|
| FIW cycle/frame | ❌ 硬编码 cycle=0, frame=0 | 需改为参数化 |
| FIW n (漫游位) | ❌ 硬编码 0 | 参数已存在但未暴露 |
| FIW r (多帧传输) | ❌ 硬编码 0 | 参数已存在但未暴露 |
| FIW t (低流量标志) | ❌ 硬编码 0 | 参数已存在但未暴露 |
| BIW1 collapse | ❌ 硬编码 0 | `create_biw1()` 参数已存在 |
| BIW1 e_biw | ❌ 硬编码 0 | 同上 |
| BIW1 carry | ❌ 硬编码 0 | 同上 |
| BIW1 prio | ✅ 可设置 (但 C++ 封装层未暴露) | FlexEncoder 无法设置 |
| 时间 BIW (001/010) | ❌ 完全没有 | 需新增 |
| SSID BIW000/BIW111 | ❌ 完全没有 | 需新增 |
| NID | ❌ 完全没有 | 需新增 |
| System Collapse | ❌ 硬编码 0 | 参数已存在 |
| 多 Frame/Cycle | ❌ 仅 Frame 0 | 需框架级改造 |
| Idle Word 填充 | ✅ 已实现 | 交替 0xFFFFFFFF / 0x00000000 |

---

## 10. 实现建议 (按优先级)

### 10.1 最小可用改进 (P0)

1. **FIW frame/cycle 参数化**: 修改 `FlexEncoder::encode()` 签名, 增加 `frame` 和 `cycle` 参数
2. **Collapse 支持**: 暴露 `create_biw1()` 的 collapse 参数到上层
3. **CAPCODE→Frame 计算工具函数**: 实现标准 frame/phase 计算公式

### 10.2 实用增强 (P1)

4. **时间 BIW**: 实现格式 001 (日期) 和 010 (时间) 的 BIW
5. **FIW 漫游位**: 暴露 n/r/t 参数
6. **System Collapse**: 在 BIW1 中正确设置系统级 collapse

### 10.3 完整基站模拟 (P2)

7. **SSID 支持**: BIW000 + BIW111 生成
8. **NID 支持**: Network Address + Short Message Vector
9. **多 Frame 调度**: 支持在指定 frame/cycle 自动发送
10. **GPS 时间同步**: 可选, 用于精确时间对齐

---

## 11. 对 SI4463 项目的影响

对于当前项目场景 (向本地寻呼机单向发送):

| 特性 | 必要性 | 原因 |
|------|--------|------|
| **Frame ID 参数化** | 🔴 必须 | 寻呼机只在分配的 frame 唤醒, frame=0 只有 1/128 的概率命中 |
| **Collapse** | 🟡 重要 | 寻呼机按 collapse 值决定唤醒频率, 不匹配则可能错过消息 |
| **SSID** | 🟢 可选 | 单网络场景不需要; 多网络或同播系统需要 |
| **NID** | 🟢 可选 | 单运营商场景不需要 |
| **时间 BIW** | 🟢 可选 | 寻呼机可能用来自动设置时间 |

---

## 12. 参考资源

- ARIB STD-43A: http://www.arib.or.jp/english/html/overview/doc/1-STD-43_A-E1.pdf
- FLEX-93001 Issue G1.8 (Motorola, under NDA)
- Philips PCD5008 FLEX Decoder Datasheet
- Motorola MC68181 FLEX Decoder Datasheet
- Rohde & Schwarz SME03 FLEX Signal Generator Manual
