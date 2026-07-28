# BeeperTX — Si4463 POCSAG / FLEX 寻呼发射机

基于 Si4463 + ESP32-S3 的双协议寻呼发射机，支持 POCSAG 和 FLEX 两种编码，
通过 GB2312 汉字字符集 + 控制字符实现中英文混排消息传输。

提供 **Web 控制面板**、**串口命令行** 和 **WiFi 配网** 三种交互方式，
所有控制接口共享同一套 API 命令集。

## 硬件连接

### 引脚映射

| ESP32-S3 | → | Si4463 | 功能 |
|:---|---:|:---|---:|
| **GPIO16** | → | **GPIO0** | **TX 数据线 (Direct Mode)** |
| GPIO5 | → | SDN | 关断控制 |
| GPIO10 | → | nSEL | SPI 片选 |
| GPIO11 | → | SDI | SPI MOSI |
| GPIO12 | → | SCLK | SPI 时钟 |
| GPIO13 | ← | SDO | SPI MISO |

### RF 天线开关

| ESP32-S3 | 功能 |
|----------|------|
| GPIO2 | TX 使能 (发射模式) |
| GPIO3 | RX 使能 (接收模式) |

### 频率范围

119–1050 MHz (6 个频段，自动选择 OUTDIV / CLKGEN_BAND)。
通过 Web 面板或 API 命令动态配置（默认 152675 kHz）。

## 项目架构

```
src/main.cpp                         组装层: 硬件初始化和组件编排
├── src/Controller/                  控制层: API 调度 + Transmitter + Web + 串口
│   ├── API.h / .cpp                 (命令调度: CmdRequest → CmdResponse 统一分发)
│   ├── Transmitter.h / .cpp         (Direct TX 控制 + POCSAG/FLEX 发射调度)
│   ├── WebServer.h / .cpp           (REST API + LittleFS 静态文件)
│   └── SerialConsole.h / .cpp       (串口命令行: 行解析 + 命令分发)
├── src/WiFiManager/                 WiFi 管理: AP 配网 / STA 连接 / NVS 持久化
│   └── WiFiManager.h / .cpp
├── lib/Pocsag/                      编码层: POCSAG 协议 (含 GB2312 中文)
│   └── Pocsag.h / Pocsag.cpp
├── lib/Flex/                        编码层: FLEX 协议
│   ├── flex.h              (C 单头文件编码库)
│   └── FlexEncoder.h / .cpp (C++ 封装)
├── lib/si4463/                      Si4463 SPI 射频芯片驱动
│   ├── Si446x.h / .cpp (Zak Kemble 移植版, ESP32-S3 适配)
│   ├── Si446x_defs.h
│   └── radio_config.h   (WDS 3.2.11.0 生成)
├── lib/Charset/                     字符集: UTF-8 → GB2312 转换
│   └── Charset.h / .cpp
└── data/                            Web 前端资源 (LittleFS)
    ├── index.html                   (SPA 控制面板)
    ├── css/style.css
    ├── css/terminal.min.css
    └── js/
        ├── main.js                  (应用入口)
        ├── api-client.js            (REST API 客户端)
        ├── config-panel.js          (配置面板逻辑)
        ├── terminal-view.js         (终端视图 + 消息发送)
        └── wifi-modal.js            (WiFi 配网弹窗)
```

### 数据流

```
[Web UI / 串口 / WiFi]
  → CmdRequest (统一命令结构)
    → api_dispatch() 命令调度器
      → Transmitter::send(Config)          // 统一入口, 自动路由编码器
        → [POCSAG] PocsagEncoder::encode() + GB.get() GB2312
        → [FLEX]   FlexEncoder::encode()  + GB.get() GB2312
      → Transmitter::enterTx()            // Si446x_startDirectTX()
      → [POCSAG] sendPreambleBits() + sendCodewords()  // GPIO0 bit-banging
      → [FLEX]   sendBytes()                              // GPIO0 bit-banging
      → Transmitter::exitTx()             // Si446x_stopTX()
  → CmdResponse → 序列化返回
  → Si4463 直接 FSK 调制发射 (Direct TX Mode, 非 FIFO)
```

## WiFi 与 Web 控制面板

### WiFi 模式

设备启动时自动选择：

1. **有保存的凭据** → STA 模式连接路由器（10s 超时），hostname `BeeperTX.local`
2. **连接失败 / 无凭据** → AP 模式回退，SSID `BeeperTX`（开放网络），IP `192.168.4.1`

AP 模式下内置 Captive Portal DNS，手机连接后自动弹出配网页面。
凭据通过 NVS (Preferences) 持久化保存，配网成功后自动重启进入 STA 模式。

### Web 控制面板

浏览器访问 `http://BeeperTX.local/`（STA 模式）或 `http://192.168.4.1/`（AP 模式），
提供完整的设备控制功能：

- **配置面板**: 协议选择 (POCSAG/FLEX)、相位、铃音、消息类型、频率、码率、功率、地址
- **消息发送**: 输入 UTF-8 消息文本，一键发射
- **WiFi 配网**: 扫描附近网络、输入密码、连接并重启
- **实时状态**: 连接状态指示灯、IP 地址、累计发射次数
- **预设管理**: 保存/加载发射配置预设 (LocalStorage)

### REST API 端点

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/api/ping` | 心跳检测 |
| GET | `/api/status` | 设备状态 (频率/功率/芯片信息/运行时间/发射次数) |
| GET | `/api/config` | 当前发射配置 |
| POST | `/api/config` | 设置发射配置 (JSON body) |
| POST | `/api/transmit` | 发射消息 |
| POST | `/api/reset` | 重启设备 |
| GET | `/api/wifi/status` | WiFi 状态 (含扫描结果) |
| GET | `/api/wifi/status?quick=1` | WiFi 状态 (不触发扫描) |
| GET | `/api/wifi/scan` | 扫描附近 WiFi 网络 |
| POST | `/api/wifi` | 设置 WiFi 凭据 (JSON: `{"ssid":"...","password":"..."}`) |
| GET | `/api/log` | 最近发射日志 |

## 串口控制台

串口波特率 **115200 bps**，支持以下命令（`<CMD> [key=value ...]\r\n` 格式）：

| 命令 | 说明 | 示例 |
|------|------|------|
| `HELP` | 打印帮助 | `HELP` |
| `STATUS` | 设备状态 | `STATUS` |
| `CONFIG` | 当前配置 | `CONFIG` |
| `SET` | 修改配置 | `SET freq=152675 power=100` |
| `TX` | 发射消息 | `TX msg=测试消息` |
| `WIFI` | WiFi 状态/配网 | `WIFI` / `WIFI ssid=MyWiFi pass=12345678` |
| `SCAN` | 扫描 WiFi | `SCAN` |
| `RESET` | 重启设备 | `RESET` |
| `PING` | 心跳检测 | `PING` |

## 命令系统 (API)

所有外部接口（Web / 串口 / 未来 MQTT 等）共享同一套命令定义：

```
传输层                    命令调度
┌──────────┐  序列化    ┌──────────┐
│ Web JSON │ ────────→  │          │
│ 串口文本 │ ────────→  │  CmdID   │ → api_dispatch()
│ MQTT TLV │ ────────→  │  枚举    │       ↓
└──────────┘            └──────────┘  CmdResponse
```

命令 ID 枚举 (`API.h`):

| 类别 | 命令 | ID | 说明 |
|------|------|----|------|
| 系统 | `CMD_PING` | 0x00 | 心跳检测 |
| 系统 | `CMD_GET_STATUS` | 0x01 | 读取设备状态 |
| 系统 | `CMD_GET_CONFIG` | 0x02 | 读取发射配置 |
| 系统 | `CMD_RESET` | 0x03 | 重启设备 |
| 配置 | `CMD_SET_CONFIG` | 0x10 | 设置发射配置 |
| 配置 | `CMD_SAVE_CONFIG` | 0x11 | 保存配置到 NVS |
| 发射 | `CMD_TRANSMIT` | 0x20 | 发射消息 |
| 查询 | `CMD_GET_LOG` | 0x30 | 读取发射日志 |
| WiFi | `CMD_SET_WIFI` | 0x40 | 设置 WiFi 凭据 |
| WiFi | `CMD_SCAN_WIFI` | 0x41 | 扫描 WiFi 网络 |
| WiFi | `CMD_GET_WIFI_STATUS` | 0x42 | 查询 WiFi 状态 |

## 发射配置

通过 Web 面板、串口命令或 API 均可动态修改以下参数（无需重新编译）：

| 参数 | 范围 | 默认值 | 说明 |
|------|------|--------|------|
| encoding | POCSAG / FLEX | POCSAG | 编码协议 |
| freqKHz | 119000–1050000 | 152675 | 发射频率 (kHz) |
| txPower | 0–127 | 127 | PA 功率 (127=+20dBm) |
| phase | NORMAL / INVERTED | NORMAL | FSK 相位极性 |
| tone | A / B / C / D | D | POCSAG 铃音/功能码 |
| baudRate | 512/1200/1600/2400/3200/6400 | 1200 | 码率 (bps) |
| msgType | NUMERIC / ALPHANUMERIC | ALPHANUMERIC | 消息类型 |
| address | 0–2097151 (POCSAG) | 1000 | 寻呼地址 / CAPCODE |
| message | UTF-8, 最大 255 字节 | "BeeperTX OK" | 消息内容 |

## POCSAG 编码详解

### 协议帧结构

POCSAG (Post Office Code Standardisation Advisory Group) 每批由同步码字 + 16 帧组成:

```
前导码 (576 bits 交替 1/0)
  → 同步码字 0x7CD215D8
  → 帧 0: [地址码字 | 空闲码字 ...]
  → 帧 1: [消息码字 | 消息码字 ...]
  → ...
  → 帧 15: [空闲码字 ...]
```

### 码字结构 (32-bit)

```
地址码字:  0 | 18-bit 地址 | 2-bit 功能码 | 10-bit BCH | 偶校验
消息码字:  1 | 20-bit 消息片段 | 10-bit BCH | 偶校验
```

功能码定义:

| Func | 模式 | 说明 |
|------|------|------|
| 0 (00) | Numeric | 数字消息 (4-bit BCD) |
| 1 (01) | Tone-only | 仅提示音 |
| 2 (10) | Alphanumeric | 字母/英文 (CCITT No.5 ASCII) |
| 3 (11) | Tone-D | 中文字母 (GB2312, 也称 Tone-D) |

### Alphanumeric 字母编码 (func=2/3)

POCSAG 标准只定义了 7-bit ASCII 字母消息。中文寻呼机使用 GB2312-80 双字节字符集，
通过**功能位 3 (11)** 激活中文模式，并借助控制字符切换中英文:

| 控制字符 | 值 | 含义 |
|---------|----|------|
| `\x0E` (Shift Out / SO) | 14 | 切换到 GB2312 中文模式 (两字节配对) |
| `\x0F` (Shift In / SI) | 15 | 切回 ASCII 英文模式 (单字节) |
| `\x04` (EOT) | 4 | 消息结束 |

中文模式下，寻呼机将收到的每两个 7-bit 字符配成一对，
重组为 GB2312-80 编码的中文字符。

### 7-bit 位反转编码 (LSB-first)

根据 CCITT No.5 标准，每个字节在 POCSAG 中以 LSB-first 顺序传输:

```
输入: '1' = 0x31 = 0b00110001
输出: 7-bit 反转 = 0b1000110 (MSB → LSB 依次为 1,0,0,0,1,1,0)

输入: GB2312 0xB2 = 0b10110010
输出: 7-bit 反转 = 0b0100110 (bit7 丢弃, 仅传输 bit6..bit0)
```

### 中英混排编码流程

```
UTF-8 输入: "ABCD - 简体中文 -"
  ↓ GB.get() 转换为 GB2312
GB2312 字节: 41 42 43 44 20 2D 20 BC F2 CC E5 D6 D0 CE C4 20 2D

  ↓ deCodeAlpha() 处理 (寻呼机默认进入中文模式)
  ↓ 遍历每个字节:

[41] ASCII → 插入 \x0F 切英文 → 7-bit 反转编码
[42] ASCII → 7-bit 反转编码
...
[BC] GB2312 → 插入 \x0E 切中文 → 7-bit 反转编码
[F2] GB2312 → 7-bit 反转编码  ← 与 BC 配成 "简"
...

  ↓ 追加 \x04 (EOT) 结束符
  ↓ 20-bit 打包 → 消息码字 → POCSAG 基带
  ↓ 可选反相处理 (invert=true)
```

### 数字编码 (4-bit BCD, func=0)

POCSAG 数字消息使用 4-bit BCD (Binary Coded Decimal) 编码，由 `deCodeNumeric()` 处理。
与字母消息不同，数字消息使用功能码 func=0，且字符映射遵循 POCSAG 标准:

| 输入字符 | BCD 值 | 说明 |
|---------|--------|------|
| 0-9 | 0x0-0x9 | 数字 |
| 空格 | 0xA | SPACE |
| U | 0xB | 紧急指示字符 |
| - | 0xD | 连字符 |
| ) | 0xE | 右括号 |
| ( | 0xF | 左括号 (对调映射) |
| 未知字符 | 0xA | → 空格 |

每个 BCD 半字节同样进行 LSB-first 位反转 (`flip4bitOrder`)，
并在消息末尾用 0xA (SPACE) 填充至 20-bit 字边界。

### 地址与消息字发送顺序修复

`genSignal()` 确保**地址码字在消息码字之前发送**: 每帧先检查地址是否已发出，
再填入剩余消息码字，末尾用空闲码字 (`0x7A89C197`) 补齐。
此前版本可能在地址之前就发送了消息码字，导致部分寻呼机无法正确解码。

## FLEX 编码详解

### 协议帧结构 (1600 bps)

FLEX (FLEX-TD, ARIB STD-43A) 每帧结构:

```
ERS (紧急重同步, 35 次重复: BS + AR + /BS + /AR)
  → S1 (位同步: 0xAA 0xAA 0xAA 0xAA + A1 + B + /A1)
  → FIW (帧信息字: 周期号 0 + 帧号 0)
  → S2 (/C Block: 块同步)
  → BIW1 + 地址字段 + 向量字段 + 消息字段 (11 块 × 8 字, 交织)
```

### 帧内布局

每帧 88 个字，每个字 32-bit BCH(31,21)+偶校验:

```
帧 0:
  ┌───────┬───────┬───────┬──────────────────────────┐
  │ BIW1  │ 地址  │ 向量  │ 消息字 [0..84]           │
  │ word 0│ word 1│ word 2│ word 3..87               │
  └───────┴───────┴───────┴──────────────────────────┘
  │← 块 0 →│← 块 1 →│← ... →│← 块 10                →
```

每块 8 个字经**块交织** (Block Interleaving) 处理:
突发错误只损坏每个字 1 bit，BCH(31,21) 可纠正 2 bit/字。

### 向量类型 (Vector Type)

向量字的 bits 6:4 定义消息类型 (来自 PCD5008 datasheet):

| v2v1v0 | 类型 | 说明 |
|--------|------|------|
| 010 | Tone-only | 仅提示音 |
| 011 | Numeric | 数字 (0-9, -, _, [, ], 空格, U) |
| 101 | **Alphanumeric** | **字母消息 (中文使用此类型)** |
| 110 | Hex/Binary | 8-bit 二进制透明传输 |

### 中文编码 (Alphanumeric V=101)

区别于常见的 Hex/Binary 透明模式，国内 FLEX 中文寻呼机使用
**Alphanumeric 向量类型 (V=101)** 传输中文:

1. UTF-8 消息经 `GB.get()` 转为 GB2312 双字节
2. GB2312 每个字节的 bit7 被 `& 0x7F` 剥离后放入 7-bit 字段
3. 寻呼机将所有 7-bit 字节重组为 8-bit 值并配对解码

### 7-bit 打包 (3 字符/字)

每个 21-bit 数据字存储 3 个 7-bit 字符:

```
字 N: [char0(bit6..0) | char1(bit6..0) | char2(bit6..0)]
       bits 20..14      bits 13..7       bits 6..0
```

`create_alphanumeric_msg()` 逐字打包:

```c
msg_word[word_idx] |= ((uint32_t)msg[i] & 0x7F) << shift;
shift += 7;
if (shift == 21) { word_idx++; shift = 0; }  // 满 3 字符 → 写下一字
```

### 中英混排控制字符 (与 POCSAG 相同机制)

国内 FLEX 寻呼机也使用 `\x0E`(SO)/`\x0F`(SI) 控制字符切换中英文，
与 POCSAG `deCodeAlpha()` 完全一致:

```
GB2312 字节流:  BC F2 CC E5 D6 D0 CE C4 20 41 42 43 20 31 32 33
                        "简体中文字 ABC 123"

寻呼机默认中文模式:
  BC F2 → "简"     (中文配对)
  CC E5 → "体"
  D6 D0 → "中"
  CE C4 → "文"

  20 41 → 遇见 0x20(空格) — 在上层插入 \x0F 切英文前就已发出
  ⚠ 0x41('A') 被当作 GB2312 第二字节与 0x20 配对 → 乱码！

插入控制字符后:
  BC F2 CC E5 D6 D0 CE C4    → "简体中文" (中文配对)
  0F 20 41 42 43 20 31 32 33 → 切英文: " ABC 123" (单字节 ASCII)
```

在 `sendFlex()` 中实现:

```cpp
static uint8_t mixed[512];
size_t m = 0;
bool inChinese = true;  // 寻呼机默认中文模式

for (size_t i = 0; i < gbLen && m < sizeof(mixed) - 8; i++) {
  uint8_t byte = (uint8_t)gbStr[i];

  if (byte >= 0x80) {          // GB2312 中文字节
    if (!inChinese) {
      mixed[m++] = 0x0E;       // 切到中文
      inChinese = true;
    }
    mixed[m++] = byte;
  } else {                     // ASCII 字节
    if (inChinese) {
      mixed[m++] = 0x0F;       // 切到英文
      inChinese = false;
    }
    mixed[m++] = byte;
  }
}
mixed[m] = '\0';
encoder.encode(cfg.address, (const char*)mixed, FlexEncoder::ALPHA);
```

`\x0E` (0x0E) 和 `\x0F` (0x0F) 都 < 0x80，
经过 `& 0x7F` 后保持原值，随普通字符一起 7-bit 打包传输。

### FLEX 帧字生成流程

```
                      ┌─────────────────┐
                      │ tf_encode_flex_ │
                      │ packet_internal │
                      └────────┬────────┘
                               │
               ┌───────────────┼───────────────┐
               v               v               v
        create_          create_          create_
        alphanumeric_    numeric_msg     tone_only_msg
        msg              (V=011)         (V=010)
        (V=101)
               │               │               │
               v               v               v
        ┌─────────────────────────────────────────┐
        │  BCH(31,21) 编码 + 偶校验              │
        │  encode_word(rev32(dw))                │
        │  → 每字 32-bit codeword                │
        └────────────────┬────────────────────────┘
                         │
                         v
        ┌─────────────────────────────────────────┐
        │  块交织 (Block Interleaving)            │
        │  interleave_block(0..10)                │
        │  → 列优先重排位序                       │
        └────────────────┬────────────────────────┘
                         │
                         v
        ┌─────────────────────────────────────────┐
        │  字节化输出                              │
        │  SAVE_VEC() / SAVE_WORD()               │
        │  → 同步头 + 帧数据 → uint8_t[]          │
        └─────────────────────────────────────────┘
```

## POCSAG vs FLEX 对比

| 特性 | POCSAG | FLEX |
|------|--------|------|
| 速率 | 512 / 1200 / 2400 bps | 1600 / 3200 / 6400 bps |
| 前导码 | 576 bits 交替 1/0 | ERS 35 次 + S1 同步 |
| 纠错 | BCH(31,21) | BCH(31,21) + 块交织 |
| 地址 | 21-bit, 单帧 1 地址 | 短/长地址, 帧内多地址 |
| 数字模式 | func=0, 4-bit BCD | V=011 Numeric |
| 字母模式 | func=2/3, 7-bit ASCII | V=101 Alphanumeric |
| 中文方式 | func=3 + `\x0E`/`\x0F` | V=101 alpha + `\x0E`/`\x0F` |
| 编码器 | Pocsag::deCodeAlpha/Numeric | FlexEncoder + sendFlex() |
| 调制 | 直接 FSK (GPIO0 bit-bang) | 直接 FSK (GPIO0 bit-bang) |
| 缓冲区 | 静态 (`.bss` 段) | 静态 (`.bss` 段) |

## 构建与烧录

```bash
pio run                                    # 编译
pio run --target upload                    # 烧录固件到开发板
pio run --target uploadfs                  # 烧录 LittleFS 文件系统 (Web 前端)
pio run --target upload_all                # 烧录固件 + LittleFS (一键)
pio device monitor -b 115200               # 串口监视器 (115200 bps)
```

### 依赖库

| 库 | 版本 | 用途 |
|---|------|------|
| ESP32Async/AsyncTCP | ^3.3.2 | Web 服务器底层 TCP |
| ESP32Async/ESPAsyncWebServer | ^3.7.4 | 异步 HTTP 服务器 |
| bblanchon/ArduinoJson | ^7.3.0 | JSON 序列化/反序列化 |

### 开发板

| 环境 | 说明 |
|------|------|
| `luatos-esp32s3` (默认) | ESP32-S3, 16MB Flash, PSRAM, LittleFS |

## 调试输出

串口 115200 bps 输出示例:

```
╔══════════════════════════════════════════╗
║       BeeperTX — POCSAG/FLEX     ║
╚══════════════════════════════════════════╝
Build: Jul 22 2026, 12:00:00

Chip Model: ESP32-S3
Chip Revision: 0

--- Si4463 ---
  Part:   0x4463 (Si4463 = 0x4463)
  ROM ID: 6 (3=B1B, 6=C2A)
  Rev:    ext=2 branch=0
  State:  READY (3)
  Freq:   152.675 MHz
  PA PWR: 127
  RSSI:   -120 dBm

WiFi: Connected to "MyWiFi"
IP:    192.168.1.100
Host:  BeeperTX.local
Web:   http://192.168.1.100/
BeeperTX started
```

## 已知限制

1. **忙等待时序**: `Transmitter` 的位流发送使用 `while (esp_timer_get_time() < next)` 忙等待，
   约 1–2s 阻塞，可能触发 WDT。代码中每 500 位 `esp_task_wdt_reset()` 一次。
2. **POCSAG 中文默认模式**: `deCodeAlpha()` 硬编码寻呼机默认中文模式 (`inGB = true`, func=3)，
   纯 ASCII 消息会额外插入 `\x0F` 控制字符，但对显示无影响。
3. **FLEX 中文长度限制**: 受 FLEX 单帧最大 248 字符限制，中英混排消息
   中每个 GB2312 字计 2 字符，控制字符额外开销约 1-2 字节。
4. **POCSAG 数字消息限制**: 数字消息需寻呼机本身支持 func=0 数字模式；
   字母消息使用 func=3 中文模式，两种模式不可混用。
5. **FLEX 不支持 GB2312 非中文字符**: GB2312 编码中，中文区域为 0xA1-0xF7(首字节)
   和 0xA1-0xFE(次字节)。非汉字区 (如日文假名、俄文字母) 未测试。
6. **单帧 FLEX**: 当前只发送单帧消息 (frame 0)，不涉及跨帧拼接。
7. **异步发射**: `CMD_TRANSMIT` 采用延迟执行模式——发射请求由 `api_dispatch()` 设置标志位，
   实际的 `Transmitter::send()` 在 `setup()` 之后的 `loop()` 中由 `api_loop()` 调度执行，
   避免阻塞 AsyncWebServer 的事件循环。
