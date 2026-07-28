# POCSAG / FLEX 编码过程文档

> 基于 `si4463` 项目当前源码, 适用于 Si4463 Direct TX Mode 发射寻呼信号。

---

## 目录

- [POCSAG / FLEX 编码过程文档](#pocsag--flex-编码过程文档)
  - [目录](#目录)
  - [1. 整体架构](#1-整体架构)
  - [2. 字符集转换: UTF-8 → GB2312](#2-字符集转换-utf-8--gb2312)
  - [3. POCSAG 编码](#3-pocsag-编码)
    - [3.1 BCH(31,21) + 偶校验码字](#31-bch3121--偶校验码字)
    - [3.2 地址码字](#32-地址码字)
    - [3.3 消息码字](#33-消息码字)
    - [3.4 Alphanumeric 编码 (含中文)](#34-alphanumeric-编码-含中文)
    - [3.5 Numeric 编码 (4-bit BCD)](#35-numeric-编码-4-bit-bcd)
    - [3.6 基带信号组装](#36-基带信号组装)
    - [3.7 完整示例](#37-完整示例)
  - [4. FLEX 编码](#4-flex-编码)
    - [4.1 FLEX 帧结构](#41-flex-帧结构)
    - [4.2 FLEX 码字 (BCH(31,21))](#42-flex-码字-bch3121)
    - [4.3 地址编码](#43-地址编码)
    - [4.4 Alphanumeric 消息 (V=101)](#44-alphanumeric-消息-v101)
    - [4.5 Numeric 消息](#45-numeric-消息)
    - [4.6 块交织](#46-块交织)
    - [4.7 完整示例](#47-完整示例)
  - [5. 发射层: Direct TX bit-banging](#5-发射层-direct-tx-bit-banging)
  - [6. 中文寻呼机编码: 控制字符机制](#6-中文寻呼机编码-控制字符机制)

---

## 1. 整体架构

```
main.cpp (配置宏: TX_ADDR, TX_MSG, TX_ENCODING, ...)
  │
  ▼
Transmitter::send(Config)
  │
  ├─[POCSAG]──► Transmitter::sendPocsag()
  │               │
  │               ├─ GB.get(msg)         → UTF-8 → GB2312
  │               ├─ PocsagEncoder::encode()  → 基带码字数组
  │               ├─ enterTx()           → Si446x_startDirectTX()
  │               ├─ sendPreambleBits()  → 576-bit 前导码 (GPIO0)
  │               ├─ sendCodewords()     → 32-bit 码字序列 (GPIO0)
  │               └─ exitTx()            → Si446x_stopTX()
  │
  └─[FLEX]─────► Transmitter::sendFlex()
                  │
                  ├─ GB.get(msg)         → UTF-8 → GB2312
                  ├─ 控制字符注入         → \x0E/\x0F 切中英文
                  ├─ FlexEncoder::encode() → FLEX 帧字节数组
                  ├─ enterTx()           → Si446x_startDirectTX()
                  ├─ sendBytes()         → 字节数组 (GPIO0)
                  └─ exitTx()            → Si446x_stopTX()
```

关键常量:

| 协议   | 码率 (常用) | 位时间 µs | 前导/同步                 |
|--------|------------|-----------|--------------------------|
| POCSAG | 512/1200   | 1953/833  | 576 bit 交替 1/0         |
| FLEX   | 1600       | 625       | ERS(35×) + S1 + FIW + S2 |

---

## 2. 字符集转换: UTF-8 → GB2312

**库**: `lib/Charset/` (Tika Flow)

POCSAG 和 FLEX 中文路径均先将 UTF-8 输入转换为 GB2312。

```cpp
String gbStr = GB.get(String("你好世界"));  // UTF-8 → GB2312
```

**转换逻辑** (`U2GB::get()` → `getUnicodeChar()` → `toGB2312()`):

1. **UTF-8 解码**: 解析 UTF-8 多字节序列 (1~6 字节), 提取 Unicode 码点
2. **查表映射**: Unicode → GB2312 双字节编码
   - ASCII (<0x80): 原样保留
   - CJK 统一汉字 (0x4E00~0x9CF0): 查 `gb2312_uni2indx_page4e` 索引表
   - 全角符号 (0xFF00~0xFFF0): 查 `gb2312_uni2indx_pageff` 索引表
3. **GB2312 格式**: 每个汉字输出 2 字节, 均 ≥ 0x80 (bit 7=1)

```
输入: UTF-8 "你好"
  '你': UTF-8 0xE4 0xBD 0xA0 → Unicode U+4F60 → GB2312 0xC4 0xE3
  '好': UTF-8 0xE5 0xA5 0xBD → Unicode U+597D → GB2312 0xBA 0xC3
输出: GB2312 [0xC4, 0xE3, 0xBA, 0xC3]
```

> **判别规则**: GB2312 字节 ≥ 0x80, ASCII 字节 < 0x80。编码器据此自动切换中/英文模式。

---

## 3. POCSAG 编码

**文件**: `lib/Pocsag/Pocsag.cpp`

POCSAG 协议标准:
- 码率: 512/1200/2400 bps
- 调制: FSK
- 码字: 32-bit (BCH(31,21) + 1-bit 偶校验)
- 帧结构: 每帧 2 个码字 (地址槽位 0~7), 16 帧 = 1 批次

### 3.1 BCH(31,21) + 偶校验码字

**生成多项式**: `G(x) = x^10 + x^9 + x^8 + x^6 + x^5 + x^3 + 1 = 0x769`

**calcCRC()** (`Pocsag.cpp:64`):
```
输入: 21-bit 数据 (左移 10 位后做长除法)
算法: 逐位 XOR 多项式 0x769
输出: 10-bit BCH 校验码
```

**码字结构** (32-bit, bit31=MSB, bit0=LSB):

```
地址码字: [0] [18-bit 地址] [2-bit func] [10-bit BCH] [1-bit parity]
消息码字: [1] [20-bit 消息] [10-bit BCH] [1-bit parity]

bit 31 (标志位): 0=地址, 1=消息
```

### 3.2 地址码字

`genAddrCode(addr, function)` (`Pocsag.cpp:106`):

```cpp
// 输入: 21-bit POCSAG 地址 + 2-bit 功能码
unsigned long code = 0;
addr >>= 3;                              // 去掉低 3 位 (用于帧/槽位计算)
code |= (addr << 13);                    // 18-bit 地址 → bit30..bit13
code |= ((function & 0x03) << 11);       // 2-bit func  → bit12..bit11
code |= (calcCRC(code >> 11) << 1);      // 10-bit BCH  → bit10..bit1
code |= evenCheck(code);                 // 1-bit parity → bit0
```

**示例**: 地址 1225429, func=0

```
1225429 = 0x12B2D5
> 3         = 0x2565A

bit30..bit13: 0x2565A = 00 0010 0101 0110 0101 1010
func=0:     bit12..bit11 = 00

code >> 11 = 0000 0100 1010 1100 1011 0100 00000
            BCH(data) = ... (计算得 10-bit 校验码)
            + even parity

最终 32-bit 码字通过 sendCodewords() 以 MSB-first 逐位发送。
```

### 3.3 消息码字

`genMsgCode(msgBin)` (`Pocsag.cpp:120`):

```cpp
unsigned long code = 0x80000000UL;       // bit31=1 (消息标志)
code |= ((msgBin & 0xFFFFF) << 11);      // 20-bit 消息 → bit30..bit11
code |= (calcCRC(code >> 11) << 1);      // 10-bit BCH
code |= evenCheck(code);                 // parity
```

### 3.4 Alphanumeric 编码 (含中文)

`deCodeAlpha(const uint8_t *charBytes, size_t length)` (`Pocsag.cpp:165`)

**核心编码规则**:
- 字符按 7-bit LSB-first 编码 (CCITT No.5 标准)
- 每个字符经 `flip7bitOrder()` 位反转后打包进 20-bit 消息缓冲区
- 每 20 位生成一个消息码字

**中文寻呼机模式 (GB2312)**:

```
状态机:
  默认 → 中文模式 (inGB=true)
  \x0E (SO) → 切换到中文模式
  \x0F (SI) → 切换到英文模式
  \x04 (EOT) → 消息结束符
```

**算法伪代码**:

```
inGB ← true
for each byte in GB2312 input:
  if byte ≥ 0x80 and not inGB:
    insert flip7bit(0x0E)  // 切中文
    inGB ← true
  else if byte < 0x80 and inGB:
    insert flip7bit(0x0F)  // 切英文
    inGB ← false

  ch ← flip7bitOrder(byte)  // 7-bit 位反转
  packBits(ch, 7)            // 打包进 20-bit 缓冲区, 满则 flush

insert flip7bit(0x04)  // EOT 结束符
if bitCount > 0:  flush (零填充)

for each 20-bit chunk:
  m_msgCode[i] ← genMsgCode(chunk)
余下填充 POCSAG_IDLE
```

**flip7bitOrder() 示例**:

```
'1' = 0x31 = 0b 0011 0001
  7-bit 反转 → 0b 100 0110 = 0x8C

0xB2 (GB2312 首字节) = 0b 1011 0010
  7-bit 反转 → 0b 010 0110 = 0x4C
```

**中英混排示例**: 输入 "Hi你好"

```
GB2312: 'H'=0x48, 'i'=0x69, '你'=0xC4,0xE3, '好'=0xBA,0xC3

byte-by-byte:
  0x48 (<0x80, inGB=true)  → 插入 0x0F(切英文), 然后 0x48
  0x69 (<0x80, inGB=false) → 直接 0x69
  0xC4 (≥0x80, inGB=false) → 插入 0x0E(切中文), 然后 0xC4
  0xE3 (≥0x80, inGB=true)  → 直接 0xE3
  0xBA (≥0x80, inGB=true)  → 直接 0xBA
  0xC3 (≥0x80, inGB=true)  → 直接 0xC3
  EOT                       → 插入 0x04

最终 7-bit 字符序列:
  [0x0F, 0x48, 0x69, 0x0E, 0xC4, 0xE3, 0xBA, 0xC3, 0x04]
  每个经 flip7bitOrder() → 实际电波上传输的 7-bit 值
```

### 3.5 Numeric 编码 (4-bit BCD)

`deCodeNumeric(const uint8_t *charBytes, size_t length)` (`Pocsag.cpp:249`)

- 每个数字字符编码为 4-bit BCD
- 经 `flip4bitOrder()` 位反转 (LSB-first)
- 填充使用 0xA (SPACE), 不用 0

**字符映射表**:

| 输入 | BCD  | 含义     |
|------|------|----------|
| 0-9  | 0-9  | 数字     |
| 空格 | 0xA  | SPACE    |
| U    | 0xB  | Urgent   |
| -    | 0xD  | 连字符   |
| [    | 0xF  | → '('    |
| ]    | 0xE  | → ')'    |

**示例**: 输入 "1234"

```
'1' → BCD=1 → flip4bit=0b1000 → pack 4 bits
'2' → BCD=2 → flip4bit=0b0100 → pack 4 bits
'3' → BCD=3 → flip4bit=0b1100 → pack 4 bits
'4' → BCD=4 → flip4bit=0b0010 → pack 4 bits

20-bit buffer: [0b1100 0010 0100 1000] = 20-bit message
→ genMsgCode(msgBin) → 32-bit codeword

若不满 20 bits:
  填充 0xA (SPACE) flip4bit 后继续 pack, 直至满 20 bits
```

### 3.6 基带信号组装

`genSignal()` (`Pocsag.cpp:291`)

```
基带结构:
  [576-bit 前导码] [Sync + 16-slot batch] × N  [16 × IDLE 尾]

前导码: 18 × 0xAAAAAAAA (32-bit 交替 1/0)
        = 576 位 = POCSAG 标准

帧/槽位计算:
  frameNum = addr & 0x07       // 低 3 位决定帧号
  addrSlot = frameNum * 2       // 每帧 2 个码字槽

组装循环:
  while not all message words sent:
    write POCSAG_SYNC (0x7CD215D8)
    for slot 0..15:
      if slot == addrSlot and not addrSent:
        write address codeword
      else if addrSent and msg remaining:
        write message codeword
      else:
        write POCSAG_IDLE (0x7A89C197)

尾部: 16 × POCSAG_IDLE
```

**反相处理** (`invert=true`):
```
所有 32-bit 码字取反: m_baseband[i] = ~m_baseband[i]
效果: 前导码 → 0x55555555, IDLE → 0x85763E68, 数据码字全部取反
```

### 3.7 完整示例

**配置**:
```
编码:     POCSAG
地址:     1225429 (0x12B2D5)
消息:     "你好"
码率:     1200 bps (bitTime=833µs)
相位:     NORMAL
```

**编码过程**:

```
1. UTF-8 "你好" → GB.get() → [0xC4, 0xE3, 0xBA, 0xC3] (4字节)

2. deCodeAlpha():
   inGB=true (默认中文)
   0xC4 (≥0x80) → flip7bit(0xC4)=... → pack 7 bits
   0xE3 (≥0x80) → flip7bit(0xE3)=... → pack 7 bits
   0xBA (≥0x80) → flip7bit(0xBA)=... → pack 7 bits
   0xC3 (≥0x80) → flip7bit(0xC3)=... → pack 7 bits
   插入 0x04 (EOT) → flip7bit(0x04)=... → pack 7 bits

   满 20-bit → flush → 20-bit chunk → genMsgCode()
   余下 15-bit → 补零 flush → genMsgCode()

   消息码字: [msgWord0, msgWord1]

3. genSignal():
   地址码字: genAddrCode(addr, function) — function 由 Tone 设置指定

   frameNum = 1225429 & 0x07 = 5
   addrSlot = 5 × 2 = 10

   基带 (简化):
     [576-bit preamble]
     [SYNC] [slot 0..9: IDLE, slot 10: addrWord, slot 11: msgWord0,
             slot 12: msgWord1, slot 13..15: IDLE]
     [SYNC] [slot 0..1: IDLE, slot 2: msgWord0, ...]
     ... 直至消息码字全部发送
     [16 × IDLE]

4. 发射:
   enterTx() → Si4463 进入 Direct TX, PLL 锁定 10ms
   sendPreambleBits(576, 833, false) → GPIO0 交替 HIGH/LOW
   sendCodewords(baseband, bandLen, 833) → MSB-first bit-bang
   exitTx() → 停止 TX
```

---

## 4. FLEX 编码

**文件**: `lib/Flex/flex.h` (Davidson Francis), `lib/Flex/FlexEncoder.cpp`

FLEX 协议标准:
- 码率: 1600 bps / 2-FSK
- 码字: 32-bit (BCH(31,21) + 偶校验), 与 POCSAG 相同多项式
- 帧结构: 1 帧 = 11 块 × 8 字, 共 88 个 32-bit 码字
- 块交织: 8 字 → 32 字节 (列方向排列), 抗突发错误

### 4.1 FLEX 帧结构

```
完整发射序列:
  [ERS × 35]  [S1]  [FIW]  [S2]  [Frame 0: 11 blocks × 8 words]

ERS (Emergency Re-Synchronization) — 35 次重复:
  每次: BS + AR + inv.BS + inv.AR
  BS   = [0xAA, 0xAA]        (16-bit 同步)
  AR   = [0xCB, 0x20, 0x59, 0x39]  (区域识别)
  BS_inv = [0x55, 0x55]
  AR_inv = [0x34, 0xDF, 0xA6, 0xC6]

S1 (帧同步 1):
  bit_sync_1 = [0xAA, 0xAA, 0xAA, 0xAA] (32-bit)
  A1         = [0x78, 0xF3, 0x59, 0x39]
  B          = [0x55, 0x55]
  A1_inv     = [0x87, 0x0C, 0xA6, 0xC6]

FIW (Frame Information Word):
  32-bit encoded word: cycle=0, frame=0, n=0, r=0, t=0

S2 (帧同步 2):
  CBlock = [0xAE, 0xD8, 0x45, 0x12, 0x7B] (40-bit)

Frame 0 数据 (88 words):
  Block 0 [word 0..7]   → BIW1, 地址, 向量, 消息, ...
  ...
  Block 10 [word 80..87] → 空闲填充
```

### 4.2 FLEX 码字 (BCH(31,21))

`encode_word(dw)` (`flex.h:165`):

与 POCSAG 同: BCH(31,21) + 偶校验。**关键差异**: 输入数据需先经 `rev32()` 全局位反转。

```c
// 流程:
dw → rev32(dw) → 取高 21-bit → BCH CRC → (data << 10) | ecc → parity
→ (code31 << 1) | parity

// 所有控制字 (FIW, BIW, 地址, 向量) 均通过此流程编码
```

### 4.3 地址编码

**短地址 (Short Address)**: 1 ~ 1,933,312 (7 位 CAPCODE)

```
短地址 = (capcode + 32768) & 0x1FFFFF
然后: encode_word(rev32(address))
```

**长地址 (Long Address)**: 2,101,249 ~ 4,297,068,542 (9-10 位 CAPCODE)

```
根据范围分 3 组:
  Set 1-2: 减 2,068,481, 计算 (w1, w2)
  Set 1-3/1-4: 同上但 w2 不同
  Set 2-3: 减 2,068,479, 不同公式

输出: 两个 32-bit 码字 [w1, w2]
```

### 4.4 Alphanumeric 消息 (V=101)

`create_alphanumeric_msg()` (`flex.h:483`)

**向量类型**: `v2v1v0 = 101` (Alpha Message Vector)

**编码规则**:
- 每 3 个 7-bit 字符填充 1 个 21-bit 字 (3 × 7 = 21)
- 字符取低 7 位 (`char & 0x7F`)
- 字结构: [7-bit char3][7-bit char2][7-bit char1] (先入低位)

```
word 0 (标志字):  f0f1=11 (初始片段), K-bit (10-bit checksum), Mail Drop
word 1 (签名字):  char1, char2, char3 + S-bit (7-bit 签名)
word 2..N:        char4-6, char7-9, ...
```

**S-bit (签名)**: 对所有消息字的 7-bit 字符分组求和, 取反
**K-bit (校验和)**: 对所有字按 g1+g2+g3 分组求和的低 10 bit, 取反

**未用字符填充**: 若最后一个字有未用字符位, 填 `0x03` (ETX)

**示例**: 消息 "ABC D"

```
字符:  A=0x41, B=0x42, C=0x43, ' '=0x20, D=0x44

word_idx=0: msg_word[0] = 0x1800 (f0f1=11)

word_idx=1: [0x43<<14 | 0x42<<7 | 0x41]  = char3='C', char2='B', char1='A'
word_idx=2: [0x44<<7  | 0x20]             = char2='D', char1=' ' (shift=14)

最后 shift=14 → 还有 1 个未用位, 填充 ETX(0x03):
word_idx=2 |= (0x03 << 14)
word_idx=3 (实际 count=2)

S-bit: sum of ((word1&0x7F) + (word1>>7&0x7F) + (word1>>14&0x7F)
              + (word2&0x7F) + (word2>>7&0x7F) + (word2>>14&0x7F))
       = 0x41+0x42+0x43+0x20+0x44+0x03 = sum, then ~sum & 0x7F
K-bit: sum of all word bytes (分 3 组: g1=低8, g2=中8, g3=高5)
       → 存入 msg_word[0] 低 10 bit

最后: 每个 msg_word[i] 经 encode_word(rev32(word)) → 32-bit codeword
```

**中文路径**: 见 [第 6 节](#6-中文寻呼机编码-控制字符机制)

### 4.5 Numeric 消息

`create_numeric_msg()` (`flex.h:854`)

**向量类型**: `v2v1v0 = 011` (Standard Numeric Vector)

- 每 4-bit BCD 字符打包进 21-bit 字
- 字最多 `(21-2)/4 ≈ 4` 字符 + 可能的溢出
- 填充使用 0xC (SPACE)

**字符表** (与 POCSAG 略有不同):

| 输入 | 值  | 含义    |
|------|-----|---------|
| 0-9  | 0-9 | 数字    |
| U/u  | 0xB | Urgent  |
| 空格 | 0xC | SPACE   |
| -_   | 0xD | 连字符  |
| ]    | 0xE | 右括号  |
| [    | 0xF | 左括号  |

**K-bit**: 对所有字按 g1+g2+g3 分组求和, `k_bit = (~sum) & 0xFF`。取高 4 位 k5k4 存入向量字 `bit19..bit16`。

### 4.6 块交织

`interleave_block()` (`flex.h:449`)

```
8 个 32-bit 码字 → 32 个 8-bit 字节 (列方向排列)

src[0..7] = 32-bit words (已位反转)
dst[0..31] = 8-bit bytes

dst[i] = (src[0].bit[31-i] << 7) |
         (src[1].bit[31-i] << 6) |
         (src[2].bit[31-i] << 5) |
         (src[3].bit[31-i] << 4) |
         (src[4].bit[31-i] << 3) |
         (src[5].bit[31-i] << 2) |
         (src[6].bit[31-i] << 1) |
         (src[7].bit[31-i] << 0)

效果: 每个输出字节包含 8 个不同码字同一位置的 1 bit。
      突发错误只损坏每字 1 bit, BCH(31,21) 可纠正 ≤2 bit 错误 → 容错。
```

每帧 11 块全部交织后, `memcpy` 到输出缓冲区。

### 4.7 完整示例

**配置**:
```
编码:     FLEX
地址:     37137 (short)
消息:     "简体中文 ACB 123"
码率:     1600 bps (bitTime=625µs)
```

**编码过程**:

```
1. UTF-8 "简体中文 ACB 123" → GB.get() → GB2312

   '简': UCS U+7B80 → GB2312 0xBC 0xF2
   '体': UCS U+4F53 → GB2312 0xCC 0xE5
   '中': UCS U+4E2D → GB2312 0xD6 0xD0
   '文': UCS U+6587 → GB2312 0xCE 0xC4
   ' ': ASCII 0x20 (保留)
   'A': ASCII 0x41
   'C': ASCII 0x43
   'B': ASCII 0x42
   ' ': ASCII 0x20
   '1': ASCII 0x31
   '2': ASCII 0x32
   '3': ASCII 0x33

   输出: [0xBC,0xF2, 0xCC,0xE5, 0xD6,0xD0, 0xCE,0xC4,
          0x20, 0x41, 0x43, 0x42, 0x20, 0x31, 0x32, 0x33]

2. 控制字符注入 (Transmitter::sendFlex):

   inChinese=true (默认中文)
   0xBC (≥0x80, inChinese=true) → 直接写入
   0xF2 (≥0x80) → 直接写入
   0xCC (≥0x80) → 直接写入
   0xE5 (≥0x80) → 直接写入
   0xD6 (≥0x80) → 直接写入
   0xD0 (≥0x80) → 直接写入
   0xCE (≥0x80) → 直接写入
   0xC4 (≥0x80) → 直接写入
   0x20 (<0x80, inChinese=true)  → 插入 0x0F (切英文), 写入 0x20
   0x41 (<0x80, inChinese=false) → 直接写入
   0x43 (<0x80)                   → 直接写入
   0x42 (<0x80)                   → 直接写入
   0x20 (<0x80)                   → 直接写入
   0x31 (<0x80)                   → 直接写入
   0x32 (<0x80)                   → 直接写入
   0x33 (<0x80)                   → 直接写入

   mixed[]:
     "简体中文" → 8 bytes GB2312
     0x0F → switch to ASCII
     " ACB 123" → 8 bytes ASCII

3. FlexEncoder::encode(mixed, 37137, ALPHA):

   a) 验证地址: 37137 为有效短地址
   b) 构建帧:
      frame_words[0] = BIW1 (prio=0, e_biw=0, s_vfield=2, carry=0, collapse=0)
      frame_words[1] = short_address(37137)
                       = ((37137 + 32768) & 0x1FFFFF)
                       = encode_word(rev32(69905))
      frame_words[2] = create_alphanum_vector_word(msg_start=3, msg_words=N)
      frame_words[3..N+2] = encode_word(rev32(msg_word[i]))
      frame_words[N+3..87] = 空闲: 交替 0xFFFFFFFF / 0x00000000
   c) 块交织: 11 blocks × 8 words → 每个 block interleave
   d) 输出缓冲: ERS×35 + S1 + FIW + S2 + frame bytes

4. 发射:
   sendBytes(data, len, 625) → MSB-first 逐 bit 输出到 GPIO0
```

---

## 5. 发射层: Direct TX bit-banging

**文件**: `src/Controller/Transmitter.cpp`

所有位流通过 **忙等待** (busy-wait) 精确时序发送, 不依赖硬件定时器。

```
发射流程:
  1. Si446x_startDirectTX(0, 1)  — 进入 Direct TX Mode
  2. delay(1000ms)                — 等待 PLL 锁定
  3. 发送位流 (GPIO0 bit-banging)
  4. Si446x_stopTX()              — 停止发射
```

**位发送逻辑**:

```cpp
void sendCodewords(const unsigned long* cw, int count, int bitTimeUs) {
  uint64_t next = esp_timer_get_time();
  for each 32-bit codeword:
    for bit 31 → 0 (MSB first):
      digitalWrite(pin, (word >> bit) & 1)
      next += bitTimeUs
      while (esp_timer_get_time() < next) {}  // 忙等待
    // 每 500 位喂 WDT
}
```

**码率→位时间查表**:

| 码率 (bps) | 位时间 (µs) | 协议   |
|-----------|-------------|--------|
| 512       | 1953        | POCSAG |
| 1200      | 833         | POCSAG |
| 1600      | 625         | FLEX   |
| 2400      | 417         | POCSAG |
| 3200      | 312         | POCSAG |
| 6400      | 156         | POCSAG |

**POCSAG 前导码**:
```
正相 (invert=false): 1, 0, 1, 0, ...  (bit 0→1, bit 1→0)
反相 (invert=true):  0, 1, 0, 1, ...
```

---

## 6. 中文寻呼机编码: 控制字符机制

POCSAG 和 FLEX 共用同一套中文寻呼机控制字符机制:

| 控制字符 | 编码值 | 含义                    |
|----------|--------|------------------------|
| SO (0x0E) | Shift Out | 切换到中文 GB2312 模式 |
| SI (0x0F) | Shift In  | 切换到英文 ASCII 模式  |
| EOT (0x04) | End of Text | 消息结束 (仅 POCSAG) |

**POCSAG 路径** (在 `deCodeAlpha()` 内):

```cpp
bool inGB = true;  // 寻呼机默认中文模式

for each byte:
  if byte ≥ 0x80 && !inGB:  insert SO before byte, inGB=true
  if byte < 0x80 && inGB:   insert SI before byte, inGB=false
  encode byte as 7-bit LSB-first
insert EOT at end
```

**FLEX 路径** (在 `Transmitter::sendFlex()` 内):

```cpp
bool inChinese = true;  // 寻呼机默认中文模式

for each byte in GB2312:
  if byte ≥ 0x80 && !inChinese:  insert 0x0E, inChinese=true
  if byte < 0x80 && inChinese:   insert 0x0F, inChinese=false
  write byte
```

**FLEX 中文字节处理**: GB2312 字节 ≥0x80 直接传入 flex.h encoder, encoder 自动 `& 0x7F` 剥离 bit 7 后放入 7-bit 字段。寻呼机固件在中文模式下将相邻两字节重组为 GB2312 双字节字符。

**与纯 ASCII 消息的区别**:
- 纯 ASCII: 无 GB2312 字节 → 不会插入 0x0E → 寻呼机全程英文模式
- 中英混排: `inGB/inChinese` 状态机追踪模式切换, 遇模式边界自动插入控制字符

**示例**: "Hello 世界" 在不同协议下的序列

```
输入:       H  e  l  l  o    世    界
GB2312:     48 65 6C 6C 6F  CA C0  BD E7

POCSAG (默认中文):
  0x0F 48 0F 65 0F 6C 0F 6C 0F 6F 0E CA 0E C0 0E BD 0E E7 04
  ↑SI  ↑SI  ↑SI  ↑SI  ↑SI  ↑SO     ↑SO     ↑SO     ↑SO    ↑EOT
  每个 ASCII 字符前都有 SI (因为默认中文模式)
  每个 GB2312 字节前都有 SO (inGB 已 false)

FLEX (默认中文):
  0x0F 48 65 6C 6C 6F 0x0E CA C0 BD E7
  ↑SI               ↑SO
  一次集体切换 (状态机聚合)
```

> 当前实现默认中文模式 (inGB=true), 即使纯 ASCII 消息也会发起 SI 切换。这对寻呼机显示无影响, 仅略微增加消息长度。
