# POCSAG / FLEX 编码过程详解

> 基于 si4463 项目代码分析。文档日期: 2026-07-21

---

## 目录

1. [总体架构](#1-总体架构)
2. [数据流总览](#2-数据流总览)
3. [POCSAG 编码详解](#3-pocsag-编码详解)
4. [FLEX 编码详解](#4-flex-编码详解)
5. [中文编码机制](#5-中文编码机制)
6. [物理层发射](#6-物理层发射)
7. [完整示例](#7-完整示例)

---

## 1. 总体架构

```
main.cpp                 配置层: 宏定义发射参数
    ↓ Transmitter::send(Config)
Transmitter.cpp          控制层: 统一调度、Direct TX 启停、位流发送
    ├─ sendPocsag() → PocsagEncoder 编码 → GPIO0 bit-banging
    └─ sendFlex()   → FlexEncoder 编码 → GPIO0 bit-banging
```

**关键文件:**

| 文件 | 角色 |
|------|------|
| `src/main.cpp` | 发射参数宏、硬件初始化、循环调度 |
| `src/Controller/Transmitter.h/cpp` | 统一发射控制 |
| `lib/Pocsag/Pocsag.h/cpp` | POCSAG 编码器 |
| `lib/Flex/FlexEncoder.h/cpp` | FLEX 编码器 C++ 封装 |
| `lib/Flex/flex.h` | FLEX 编码器 C 核心 (单头文件) |
| `lib/Charset/Charset.h/cpp` | UTF-8 → GB2312 字符集转换 |

---

## 2. 数据流总览

```
UTF-8 消息字符串
    │
    ▼
UTF8 → GB2312 转换 (GB.get())
    │
    ▼
协议编码 (POCSAG / FLEX)
    │
    ▼
基带码字 / 字节数组
    │
    ▼
Si4463 Direct TX Mode (GPIO0 bit-banging) → 射频输出
```

### 发射配置示例 (`main.cpp`)

```cpp
#define TX_ENCODING   Transmitter::POCSAG   // POCSAG / FLEX
#define TX_PHASE      Transmitter::NORMAL    // 正相 / 反相
#define TX_BAUD_RATE  1200                   // 512 / 1200 / 1600 / 2400 / 3200 / 6400
#define TX_MSG_TYPE   Transmitter::ALPHANUMERIC // NUMERIC / ALPHANUMERIC
#define TX_ADDR       1225429
#define TX_MSG        "你好世界 Hello"
#define TX_REPEAT     1
```

---

## 3. POCSAG 编码详解

### 3.1 协议帧结构

POCSAG 一帧 (Batch) 的结构:

```
┌──────────┬──────────┬──────────┬──────────┬──────────┬──────────┬──────────┐
│ Preamble │  Sync    │  Slot 0  │  Slot 1  │  ...     │  Slot 14 │  Slot 15 │
│ 576 bits │  32-bit  │  32-bit  │  32-bit  │          │  32-bit  │  32-bit  │
└──────────┴──────────┴──────────┴──────────┴──────────┴──────────┴──────────┘
                            ↑ 每帧 17 个码字 (1 Sync + 16 时隙)
                            ↑ 每时隙 = 2 个码字 (地址/消息/空闲)
```

**关键常量:**

| 常量 | 值 | 说明 |
|------|-----|------|
| `POCSAG_PREAMBLE` | `0xAAAAAAAA` | 前导码: 0101... 交替模式 |
| `POCSAG_SYNC` | `0x7CD215D8` | 帧同步码字 |
| `POCSAG_IDLE` | `0x7A89C197` | 空闲码字 (无数据时填充) |

### 3.2 编码流程 (`PocsagEncoder::encode()`)

```
encode(addr, func, msg, len, mode, invert)
  │
  └─→ genSignal(addr, func, msg, len, mode, invert)
        │
        ├─ 1. genPreamble() — 生成 576 位前导码
        │
        ├─ 2. 编码消息:
        │      ├─ MODE_ALPHANUMERIC → deCodeAlpha()
        │      │     7-bit 字符编码 + GB2312 中文支持
        │      │
        │      └─ MODE_NUMERIC → deCodeNumeric()
        │            4-bit BCD 编码
        │
        ├─ 3. 计算地址所在时隙: frameNum = addr & 0x07
        │
        ├─ 4. 组装批次:
        │      ┌──────────────────────────────┐
        │      │ Sync Codeword (32-bit)       │
        │      ├──────────────────────────────┤
        │      │ Slot 0: [地址码字/空闲]      │ ← 地址在 frameNum 对应时隙
        │      │ Slot 1: [消息码字/空闲]      │
        │      │ ...                          │
        │      │ Slot 15: [消息码字/空闲]     │
        │      └──────────────────────────────┘
        │
        ├─ 5. 尾部填充 16 个空闲码字
        │
        └─ 6. 如果 invert=true, 全部码字取反
```

### 3.3 码字结构

#### 地址码字 (bit31 = MSB)

```
Bit:  31    30..13    12..11     10..1     0
    ┌─────┬─────────┬─────────┬──────────┬─────┐
    │  0  │ 18-bit  │ 2-bit   │ 10-bit   │ 偶  │
    │     │ 地址    │ 功能码  │ BCH 校验 │ 校验 │
    └─────┴─────────┴─────────┴──────────┴─────┘
```

- bit31 = `0` 标识地址码字
- 21-bit 地址右移 3 位后放入 bit30..13
- 功能码 0-3 (对应 TONE_A~TONE_D)
- BCH(31,21) 生成多项式: `G(x) = x¹⁰ + x⁹ + x⁸ + x⁶ + x⁵ + x³ + 1` = `0x769`

#### 消息码字

```
Bit:  31    30..11     10..1     0
    ┌─────┬──────────┬──────────┬─────┐
    │  1  │ 20-bit   │ 10-bit   │ 偶  │
    │     │ 消息数据 │ BCH 校验 │ 校验 │
    └─────┴──────────┴──────────┴─────┘
```

- bit31 = `1` 标识消息码字
- 20-bit 消息片段

### 3.4 Alphanumeric 编码 (`deCodeAlpha()`)

**字符编码规则:**

| 字节范围 | 编码方式 | 说明 |
|----------|----------|------|
| `< 0x80` | 7-bit LSB-first 位反转 | ASCII 单字节 |
| `>= 0x80` | 同上 (只取低 7 位) | GB2312 双字节 |
| `\x0E` | 7-bit 位反转后编码 | Shift Out → 中文模式 |
| `\x0F` | 7-bit 位反转后编码 | Shift In → 英文模式 |
| `\x04` | 7-bit 位反转后编码 | EOT 消息结束 |

**位反转示例:**

```
输入: '1' = 0x31 = 0b00110001
                          ↓ 7-bit 位反转 (LSB-first)
输出:              0b1000110 = 0x46

输入: 0xB2 (GB2312 高字节)
                          ↓ 7-bit 位反转
输出:              0b0100110 = 0x26
```

**打包过程:** 每 7-bit 字符依次填入 20-bit 缓冲区，填满后生成一个消息码字。

**控制字符自动插入逻辑:**

```cpp
bool inGB = true;  // 寻呼机默认中文模式

for each byte in message:
    if byte >= 0x80 && !inGB:
        插入 0x0E (SO)  → 切中文模式
        inGB = true
    else if byte < 0x80 && inGB:
        插入 0x0F (SI)  → 切英文模式
        inGB = false
    编码并打包当前字节

追加 0x04 (EOT) 消息结束符
```

### 3.5 Numeric 编码 (`deCodeNumeric()`)

**字符映射表 (4-bit BCD):**

| 输入字符 | BCD 值 | 输入字符 | BCD 值 |
|----------|--------|----------|--------|
| `0`-`9` | `0x0`-`0x9` | ` `(空格) | `0xA` |
| `U` | `0xB` | `-` | `0xD` |
| `]` → `)` | `0xE` | `[` → `(` | `0xF` |

**编码过程:**
1. 每个字符 → 4-bit BCD
2. 每个 nibble 做 4-bit 位反转 (LSB-first)
3. 依次填入 20-bit 缓冲区
4. 尾部用 SPACE (`0xA`) 填充至 20-bit 边界

---

## 4. FLEX 编码详解

### 4.1 协议帧结构 (1600 bps, 单帧)

```
┌──────┬──────┬──────┬──────┬─────┬──────┬─────────────────────────┐
│  ERS  │  S1  │ FIW  │ S2   │ BIW │ Addr │  Vector + Message       │
│ ×35   │      │      │      │     │ (1-2)│  (块交织 11块×8码字)    │
└──────┴──────┴──────┴──────┴─────┴──────┴─────────────────────────┘
```

**每部分详解:**

| 部分 | 内容 | 大小 | 说明 |
|------|------|------|------|
| **ERS** | BS + AR + inv(BS) + inv(AR) | 35 × 8 bytes | Emergency Re-Synchronization |
| **S1** | BitSync1 + A1 + B + inv(A1) | 14 bytes | 同步信号 1 |
| **FIW** | 帧信息字 | 4 bytes | Frame Information Word |
| **S2** | C block | 5 bytes | 同步信号 2 |
| **数据区** | BIW1 + 地址 + 向量 + 消息 | 11块×8字×4字节 | 块交织数据 |

### 4.2 编码流程 (`tf_encode_flex_message()`)

```
tf_encode_flex_message(msg, capcode, buffer, size, &error)
  │
  └─→ tf_encode_flex_packet_internal(msg, capcode, ..., &alpha_config, NULL)
        │
        ├─ 1. 参数校验 (消息长度、Capcode 有效性、缓冲区大小)
        │
        ├─ 2. 发送 ERS × 35:
        │      for i in 0..34:
        │        发送 flex_bs  (0xAA, 0xAA)
        │        发送 flex_ar (0xCB, 0x20, 0x59, 0x39)
        │        发送 flex_bs_inv  (0x55, 0x55)
        │        发送 flex_ar_inv (0x34, 0xDF, 0xA6, 0xC6)
        │
        ├─ 3. 发送 S1:
        │      flex_bit_sync_1 (0xAA, 0xAA, 0xAA, 0xAA)
        │      flex_a1  (0x78, 0xF3, 0x59, 0x39)
        │      flex_b   (0x55, 0x55)
        │      flex_a1_inv (0x87, 0x0C, 0xA6, 0xC6)
        │
        ├─ 4. FIW: create_fiw(cycle=0, frame=0, ...)
        │
        ├─ 5. S2: flex_cblock (0xAE, 0xD8, 0x45, 0x12, 0x7B)
        │
        ├─ 6. BIW1: create_biw1(prio=0, e_biw=0, s_vfield=2+is_long, ...)
        │
        ├─ 7. 地址:
        │      ├─ Short (≤1933312): create_short_address(capcode)
        │      └─ Long  (≥2101249): create_long_address(capcode) → 2 words
        │
        ├─ 8. 消息 (create_alphanumeric_msg):
        │      ├─ 首字标志位 (f0f1=11, mail_drop, K-bit)
        │      ├─ 每 3 个 7-bit 字符填入一个消息字 (21-bit)
        │      ├─ 未使用的字符位置填充 ETX (0x03)
        │      ├─ 计算 S-bit (签名) → 放入第二个消息字
        │      ├─ 计算 K-bit (校验和) → 放入首字
        │      └─ 生成向量字指明消息位置
        │
        ├─ 9. 空闲填充 (未满块: 偶字0xFFFFFFFF, 奇字0x00000000)
        │
        └─ 10. 块交织 (11块, 每块8字行→列重排)
```

### 4.3 地址编码

**Short Address (1 ~ 1,933,312):**

```c
address = (capcode + 32768) & 0x1FFFFF  // 21-bit
// → encode_word(rev32(address))
```

**Long Address (2,101,249 ~ 4,297,068,542):**

分多组映射为 2 个码字，使用 `create_long_address()`。

### 4.4 Alphanumeric 消息编码

```
消息 "ABC" → 3 个字符

填充到消息字:
┌─────────────────────────────────────┐
│ Word 0: [f0f1=11][K-bit(10)][reserved] │  ← 首字: 标志 + 校验和
├─────────────────────────────────────┤
│ Word 1: [S-bit(7)]['C'(7)]['B'(7)]  │  ← 含签名 S-bit
├─────────────────────────────────────┤
│ Word 2: ['A'(7)][0x03(7)][0x03(7)]  │  ← 未用位置填 ETX
└─────────────────────────────────────┘

最大消息长度: MAX_CHARS_ALPHA = (11*8 - 4) * 3 - 4 = 248 字符
```

**S-bit 计算:** 对 word[1..n-1] 的三字节组求和后取反。

**K-bit 计算:** 对所有消息字的 g1+g2+g3 求和后取反，放入 word[0]。

### 4.5 块交织

```
块交织前 (8 字 × 32-bit):
    Word 0: b31 b30 ... b0
    Word 1: b31 b30 ... b0
    ...
    Word 7: b31 b30 ... b0

块交织后 (32 "行" × 8-bit):
    Row 0:  W0b31 W1b31 W2b31 ... W7b31
    Row 1:  W0b30 W1b30 W2b30 ... W7b30
    ...
    Row 31: W0b0  W1b0  W2b0  ... W7b0
```

这样突发错误只会损坏每字的 1-bit，BCH 可纠正。

### 4.6 FLEX Numeric 编码

每个数字字符用 4-bit 编码，最大 8 个字。字符映射与 POCSAG 数字模式类似:

| 字符 | 值 | 字符 | 值 |
|------|-----|------|-----|
| `0`-`9` | `0x0`-`0x9` | `U`/`u` | `0xB` |
| ` ` | `0xC` | `-`/`_` | `0xD` |
| `]` | `0xE` | `[` | `0xF` |

---

## 5. 中文编码机制

### 5.1 原理

两种协议都使用 **GB2312-80 字符集 + CCITT No.5 7-bit 编码**，通过控制字符切换中英文模式:

| 控制字符 | 名称 | 作用 |
|----------|------|------|
| `\x0E` | SO (Shift Out) | 进入中文 GB2312 模式 |
| `\x0F` | SI (Shift In) | 进入英文 ASCII 模式 |
| `\x04` | EOT | 消息结束 |

### 5.2 POCSAG 中文路径

```
1. main.cpp: "你好世界Hello" (UTF-8)
         ↓
2. GB.get("你好世界Hello")  →  GB2312 字节串
         ↓
3. deCodeAlpha() 遍历 GB2312 字节:
   字节 0xC4 (≥0x80) → 中文模式, 直接编码 (7-bit 位反转)
   字节 0xBA (≥0x80) → 中文模式, 直接编码
   字节 'H'  (<0x80) → 自动插 0x0F 切英文, 编码 'H'
   字节 'e'  (<0x80) → 英文模式, 编码
   ... 直到另一中文字符 → 自动插 0x0E 切中文
   最后追加 0x04 (EOT)
         ↓
4. 打包为 20-bit 消息码字
```

### 5.3 FLEX 中文路径

```
1. main.cpp: "你好世界Hello" (UTF-8)
         ↓
2. GB.get("你好世界Hello")  →  GB2312 字节串
         ↓
3. sendFlex() 中遍历 GB2312 字节:
   字节 ≥0x80 → 中文模式, 直接输出
   字节 <0x80 → 自动插 0x0F/0x0E 控制字符
   (与 POCSAG 机制相同)
         ↓
4. 混合后的字节串传给 FlexEncoder::encode(ALPHA)
         ↓
5. FLEX 7-bit 字段编码 (每个字符 & 0x7F 后填入)
```

**关键区别:**
- POCSAG: 控制字符也需要做 7-bit 位反转后放入
- FLEX: 控制字符直接 `& 0x7F` 放入 7-bit 字段

---

## 6. 物理层发射

### 6.1 Direct TX 流程

```
enterTx()
  └─ Si446x_startDirectTX(0, 1)  → 启动异步直接模式
  └─ delay(1000 ms)              → 等待 PLL 锁相环稳定

发送数据 (位时间由波特率决定):
  ├─ POCSAG: sendPreambleBits() → sendCodewords()
  │     每 bit: digitalWrite(GPIO0, HIGH/LOW)
  │     + busy-wait 精确时序
  │
  └─ FLEX: sendBytes()
        每 byte MSB-first, 逐位输出

exitTx()
  └─ Si446x_stopTX()  → 停止发射
```

### 6.2 码率与位时间

| 波特率 | 位时间 (µs) | POCSAG | FLEX |
|--------|-------------|--------|------|
| 512 | 1953 | ✓ | ✗ |
| 1200 | 833 | ✓ | ✗ |
| 1600 | 625 | ✓ | ✓ |
| 2400 | 417 | ✓ | ✗ |
| 3200 | 312 | ✓ | ✗ |
| 6400 | 156 | ✓ | ✗ |

### 6.3 看门狗保护

每发送 500 位调用 `esp_task_wdt_reset()` 喂一次看门狗，防止长帧触发 WDT 复位。

---

## 7. 完整示例

### 示例 1: POCSAG 数字消息

**输入配置:**
```cpp
TX_ENCODING = POCSAG
TX_MSG_TYPE = NUMERIC
TX_MSG      = "1234567890"
TX_ADDR     = 1225429
TX_BAUD     = 1200
TX_PHASE    = NORMAL
```

**编码过程:**

```
1. BCD 转换:
   '1'(0x01) '2'(0x02) '3'(0x03) '4'(0x04) '5'(0x05)
   '6'(0x06) '7'(0x07) '8'(0x08) '9'(0x09) '0'(0x00)

2. 4-bit LSB-first 反转:
   0x01→0x08, 0x02→0x04, 0x03→0x0C, 0x04→0x02, ...

3. 打包: 每 5 个 nibble 填满一个 20-bit 消息码字
   10 digits → 2 个消息码字

4. 帧组装:
   [Preamble 576bit][Sync][Slot4=地址码字][Slot5..=消息码字/空闲]...

5. 发射时序:
   576 preamble bits × 833µs = ~480ms
   + 17 codewords × 32 bits × 833µs = ~453ms
   ≈ 总计 933ms (单帧)
```

### 示例 2: POCSAG 中文消息

**输入配置:**
```cpp
TX_ENCODING = POCSAG
TX_MSG_TYPE = ALPHANUMERIC
TX_MSG      = "测试ABC"
TX_ADDR     = 1000
```

**编码过程:**

```
1. UTF-8 → GB2312:
   "测" (E6B58B) → 0xB2 0xE2
   "试" (E8AF95) → 0xCA 0xD4
   "ABC" (unchanged)

2. GB2312 字节串: B2 E2 CA D4 41 42 43

3. deCodeAlpha() 处理:
   inGB = true (默认中文模式)
   B2 (≥0x80) → 中文模式: 位反转 0x4E, 打包
   E2 (≥0x80) → 中文模式: 位反转 0x47, 打包
   CA (≥0x80) → 中文模式: 位反转 0x53, 打包
   D4 (≥0x80) → 中文模式: 位反转 0x2B, 打包
   41 (<0x80) → 中文→英文, 插 0x0F: 位反转 0x78, 打包
   42 (<0x80) → 英文模式: 位反转 0x42, 打包  ← 注意: 0x42 的 7-bit 反转
   43 (<0x80) → 英文模式: 位反转 0x43, 打包
   EOT → 插 0x04: 位反转 0x20, 打包

4. 码字数 = ceil(总 7-bit 字符 / 20 * 7) ≈ 3-4 个消息码字
```

### 示例 3: FLEX 字母消息

**输入配置:**
```cpp
TX_ENCODING = FLEX
TX_BAUD     = 1600
TX_MSG_TYPE = ALPHANUMERIC
TX_MSG      = "Hello"
TX_ADDR     = 37137
```

**编码过程:**

```
1. 消息 "Hello" → 5 个 7-bit 字符

2. 消息字组装:
   Word 0: 0x1800 | K-bit  (首字: 标志+校验和)
   Word 1: S-bit | 'o'(7) | 'l'(7)  (含签名)
   Word 2: 'l'(7) | 'e'(7) | 'H'(7)

3. 字较少, 剩余位置填充空闲字:
   偶索引 = 0xFFFFFFFF, 奇索引 = 0x00000000

4. 块交织后发送

5. 发射总长度 ≈ ERS(280B) + S1(14B) + FIW(4B) + S2(5B) + Data(352B)
   = ~655 bytes × 8 × 625µs = ~3.28s (1600 bps)
```

### 示例 4: FLEX 中文消息

**输入配置:**
```cpp
TX_ENCODING = FLEX
TX_BAUD     = 1600
TX_MSG_TYPE = ALPHANUMERIC
TX_MSG      = "简体中文 123"
TX_ADDR     = 37137
```

**编码过程:**

```
1. UTF-8 → GB2312:
   "简体中文" → 0xBC 0xF2 0xCC 0xE5 0xD6 0xD0 0xCE 0xC4
   " 123" → 0x20 0x31 0x32 0x33

2. 控制字符插入 (sendFlex 中处理):
   inChinese = true (默认)
   0xBC ≥0x80 → 中文模式, 输出 0xBC
   0xF2 ≥0x80 → 中文模式, 输出 0xF2
   0xCC ≥0x80 → 中文模式, 输出 0xCC
   0xE5 ≥0x80 → 中文模式, 输出 0xE5
   0xD6 ≥0x80 → 中文模式, 输出 0xD6
   0xD0 ≥0x80 → 中文模式, 输出 0xD0
   0xCE ≥0x80 → 中文模式, 输出 0xCE
   0xC4 ≥0x80 → 中文模式, 输出 0xC4
   0x20 <0x80 → 中文→英文, 插 0x0F, 输出 0x20
   0x31 <0x80 → 英文模式, 输出 0x31
   0x32 <0x80 → 英文模式, 输出 0x32
   0x33 <0x80 → 英文模式, 输出 0x33

3. 混合后字节串: BC F2 CC E5 D6 D0 CE C4 0F 20 31 32 33

4. 每个字节 & 0x7F 后填入 FLEX 7-bit 字段:

   MSB  bit7    bit6..0
   BC → 1  → 0x3C  (去掉 bit7)
   F2 → 1  → 0x72
   ...

5. 寻呼机侧: 遇到 ≥0x80 的字节对自动重组为 GB2312 字符
   遇到 0x0F 切换显示模式
```

---

## 附录: 关键代码调用栈

### POCSAG 发射

```
main.cpp: s_tx.send(makeConfig())
  └─ Transmitter::send(Config)
       └─ sendPocsag(Config)
            ├─ GB.get(msg) → UTF-8→GB2312
            ├─ PocsagEncoder::encode(addr, tone, data, len, mode, invert)
            │    └─ genSignal()
            │         ├─ genPreamble()
            │         ├─ deCodeAlpha() 或 deCodeNumeric()
            │         │    └─ genAddrCode() / genMsgCode()
            │         │         ├─ calcCRC()  ← BCH(31,21)
            │         │         └─ evenCheck() ← 偶校验
            │         └─ 帧组装
            ├─ enterTx() → Si446x_startDirectTX()
            ├─ sendPreambleBits(576, 833, false)
            ├─ sendCodewords(baseband, len, 833)
            └─ exitTx() → Si446x_stopTX()
```

### FLEX 发射

```
main.cpp: s_tx.send(makeConfig2())
  └─ Transmitter::send(Config)
       └─ sendFlex(Config)
            ├─ GB.get(msg) → UTF-8→GB2312 + SO/SI 控制字符
            ├─ FlexEncoder::encode(addr, mixed, ALPHA)
            │    └─ tf_encode_flex_message()
            │         └─ tf_encode_flex_packet_internal()
            │              ├─ ERS × 35
            │              ├─ S1
            │              ├─ create_fiw() → FIW
            │              ├─ S2
            │              ├─ create_biw1() → BIW1
            │              ├─ create_short/long_address()
            │              ├─ create_alphanumeric_msg()
            │              │    ├─ BCH encode_word()
            │              │    ├─ S-bit / K-bit 计算
            │              │    └─ create_alphanum_vector_word()
            │              └─ block interleave × 11
            ├─ enterTx() → Si446x_startDirectTX()
            ├─ sendBytes(data, len, 625)
            └─ exitTx() → Si446x_stopTX()
```
