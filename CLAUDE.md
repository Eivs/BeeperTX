# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & Test Commands

```bash
pio run                                    # 编译
pio run --target upload                    # 烧录固件到开发板
pio run --target uploadfs                  # 烧录 LittleFS 文件系统 (Web 前端)
pio run --target upload_all                # 烧录固件 + LittleFS (一键)
pio device monitor                         # 串口监视器 (115200 bps, 由 platformio.ini 配置)
pio run --target upload --target monitor   # 烧录并打开串口
```

没有单元测试 — 验证方式是通过串口观察发射日志输出，或使用频谱仪/RTL-SDR 确认射频输出。

## 项目架构

四层设计：

```
src/main.cpp                 → 配置层: 引脚、频率、功率、发射参数、HW 初始化、循环调度
src/Controller/              → 控制层: API 调度 + Transmitter + WebServer + SerialConsole
src/WiFiManager/             → WiFi 管理: AP 配网 / STA 连接 / NVS 持久化
lib/Pocsag/                  → 编码层: POCSAG 协议 (BCH, 码字组装, GB2312 中文)
lib/Flex/                    → 编码层: FLEX 协议 (flex.h 单头文件, 含 GB2312 中文)
lib/si4463/                  → 驱动层: Si4463 SPI 射频芯片 (Zak Kemble 移植版)
lib/Charset/                 → 字符集: UTF-8 → GB2312 转换
```

### 关键数据流

```
main.cpp: 配置宏 (TX_ENCODING, TX_ADDR, TX_MSG ...)
  → Transmitter::send(Config)
    → [POCSAG] PocsagEncoder::encode() + GB.get() GB2312
    → [FLEX]   FlexEncoder::encode() + GB.get() GB2312 + \x0E/\x0F 控制字符
  → Transmitter::enterTx()  Si446x_startDirectTX()
  → [POCSAG] sendPreambleBits() + sendCodewords()  GPIO0 bit-banging
  → [FLEX]   sendBytes()                             GPIO0 bit-banging
  → Transmitter::exitTx()   Si446x_stopTX()
  → Si4463 直接 FSK 调制发射 (Direct TX Mode)
```

### 发射模式

- **Direct TX Mode** (非 FIFO): MCU 通过 GPIO0 逐位驱动调制数据
- `Si446x_startDirectTX(0, 1)` 启动异步直接模式
- `Transmitter::sendPreambleBits()` / `sendCodewords()` / `sendBytes()` 处理实际位流
- `Transmitter::bitTimeUs(baudRate)` 码率→位时间查表 (512→1953, 1200→833, 1600→625, 2400→417, 3200→312, 6400→156 µs)
- 忙等待时序 (`while (esp_timer_get_time() < next)` 循环, 每 500 位喂一次 WDT)

### POCSAG 编码 (lib/Pocsag/)

- `PocsagEncoder::encode(addr, func, msg, len, mode, invert)` — 主入口
- `deCodeAlpha()` — 处理含 GB2312 中文的字母消息, 自动插入 `\x0E`/`\x0F` 控制字符切换中英文
- `deCodeNumeric()` — 4-bit BCD 数字编码
- `genSignal()` — 组装完整基带: 同步码 + 16 帧数据 (地址 + 消息码字 + 空闲码字填充)
- 码字使用 BCH(31,21) + 偶校验 (生成多项式 0x769)
- 中文字符集基于 GB2312-80 + CCITT No.5 7-bit LSB-first 标准
- `\x0E`(SO)=中文模式, `\x0F`(SI)=英文模式, `\x04`(EOT)=消息结束

### FLEX 编码 (lib/Flex/)

- **flex.h**: 单头文件 C 库 (Unlicense), 1600 bps / 2-FSK, 单帧
- `FlexEncoder` C++ 封装类, 接口类似 `PocsagEncoder`
- `FlexEncoder::encode(capcode, msg, type)` — 入口 (ALPHA/NUMERIC/TONE_ONLY)
- 输出完整 FLEX 帧: ERS(35次) → S1 → FIW → S2 → 11块×8字 (块交织) 数据
- FLEX 1600 bps = 625 µs/bit (通过 `Transmitter::bitTimeUs(1600)`)

**FLEX 中文编码**: 使用 Alphanumeric 向量类型 (V=101), 非 Hex/Binary 透明模式：
1. UTF-8 → `GB.get()` → GB2312 双字节
2. GB2312 字节经 `& 0x7F` 剥离 bit 7 后放入 7-bit 字段 (每字 3 字符)
3. 通过 `\x0E`(SO)/`\x0F`(SI) 控制字符切换中英文模式 (与 POCSAG 同机制)
4. 寻呼机默认中文模式, 每两字节配对重组为 GB2312 字符
5. 中英混排时自动插入控制字符 (在 `Transmitter::sendFlex()` 中实现)

### Transmitter 控制层 (src/Controller/)

- `Transmitter` 类封装 Direct TX 公共流程, 统一调度 POCSAG / FLEX
- `Transmitter::Config` 结构体包含全部发射参数: encoding, phase, baudRate, tone, address, message, msgType, repeat
- `Transmitter::send(Config)` — 统一入口, 自动选择编码器
- `Transmitter::bitTimeUs(baudRate)` — 码率→位时间查表 (512/1200/1600/2400/3200/6400)
- FLEX 中文路径在 `sendFlex()` 中完成 GB2312 转换 + 控制字符注入
- POCSAG 中文路径在 `deCodeAlpha()` 中完成控制字符注入
- `main.cpp` 只定义宏和调用 `s_tx.send(makeConfig())`, 无编码逻辑

### Si4463 驱动 (lib/si4463/)

- 基于 Zak Kemble 的 AVR Arduino 库, 移植到 ESP32-S3
- 增强功能: 运行时频率控制 (`Si446x_setFrequencyKHz`)、64 位整数频率计算、GPIO2/GPIO3 RF 天线开关控制
- 初始化流程: `Si446x_setPins()` → `Si446x_init()` → `Si446x_setFrequencyKHz()` → `Si446x_setTxPower()` → `Si446x_configRFswitch()`
- WDS (Wireless Development Studio) 生成的配置在 `radio_config.h`
- 支持 Si4463 Rev B1 (ROM ID 3) 和 C2 (ROM ID 6)
- 可选的 Rev C2 补丁在 `radio_config/Si4463c2/si4463_c2_patch.h`

### 引脚映射

| ESP32-S3 | Si4463 | 功能 |
|----------|--------|------|
| GPIO16   | GPIO0  | TX 数据 (Direct Mode) |
| GPIO5    | SDN    | 关断 |
| GPIO10   | nSEL   | SPI CS |
| GPIO11   | SDI    | SPI MOSI |
| GPIO12   | SCLK   | SPI SCK |
| GPIO13   | SDO    | SPI MISO |
| GPIO4    | IRQ    | 中断 (轮询模式时接 -1) |

### RF 天线开关

| ESP32-S3 | 功能 |
|----------|------|
| GPIO2 | TX 使能 (发射模式) |
| GPIO3 | RX 使能 (接收模式) |

### 频率配置

支持 119–1050 MHz (6 个频段, 自动选择 OUTDIV/CLKGEN_BAND)。
`main.cpp` 顶部 `#define TX_FREQ_KHZ` 为可调参数 (当前 156150 kHz)。

## 已知问题

1. **忙等待阻塞**: `Transmitter` 的位流发送使用 `while (esp_timer_get_time() < next)` 忙等待, 约 1–2s 阻塞, 可能触发 WDT。代码中每 500 位 `esp_task_wdt_reset()` 一次。
2. **POCSAG 中文默认模式**: `deCodeAlpha()` 硬编码寻呼机默认中文模式 (`inGB = true`, func=3), 纯 ASCII 消息会额外插入 `\x0F` 控制字符, 但对显示无影响。
3. **单帧 FLEX**: 当前只发送单帧消息 (frame 0), 不涉及跨帧拼接。

## 项目配置

- `platformio.ini` — 环境: `luatos-esp32s3` (ESP32-S3, 16MB Flash, PSRAM, LittleFS)
- `Si446x_config.h` — 编译时引脚默认值 (可被 `Si446x_setPins()` 覆盖)
- `radio_config.h` — WDS 3.2.11.0 生成的射频配置文件 (1200 bps, ±4.5 kHz FSK)
- `radio_config/` — 子目录包含 Rev B1 / C2 的多种配置 (FLEX/POCSAG, 不同速率)

<!-- gitnexus:start -->
# GitNexus — Code Intelligence

This project is indexed by GitNexus as **BeeperTX** (1677 symbols, 2952 relationships, 148 execution flows). Use the GitNexus MCP tools to understand code, assess impact, and navigate safely.

> Index stale? Run `node .gitnexus/run.cjs analyze` from the project root — it auto-selects an available runner. No `.gitnexus/run.cjs` yet? `npx gitnexus analyze` (npm 11 crash → `npm i -g gitnexus`; #1939).

## Always Do

- **MUST run impact analysis before editing any symbol.** Before modifying a function, class, or method, run `impact({target: "symbolName", direction: "upstream"})` and report the blast radius (direct callers, affected processes, risk level) to the user.
- **MUST run `detect_changes()` before committing** to verify your changes only affect expected symbols and execution flows. For regression review, compare against the default branch: `detect_changes({scope: "compare", base_ref: "main"})`.
- **MUST warn the user** if impact analysis returns HIGH or CRITICAL risk before proceeding with edits.
- When exploring unfamiliar code, use `query({search_query: "concept"})` to find execution flows instead of grepping. It returns process-grouped results ranked by relevance.
- When you need full context on a specific symbol — callers, callees, which execution flows it participates in — use `context({name: "symbolName"})`.
- For security review, `explain({target: "fileOrSymbol"})` lists taint findings (source→sink flows; needs `analyze --pdg`).

## Never Do

- NEVER edit a function, class, or method without first running `impact` on it.
- NEVER ignore HIGH or CRITICAL risk warnings from impact analysis.
- NEVER rename symbols with find-and-replace — use `rename` which understands the call graph.
- NEVER commit changes without running `detect_changes()` to check affected scope.

## Resources

| Resource | Use for |
|----------|---------|
| `gitnexus://repo/BeeperTX/context` | Codebase overview, check index freshness |
| `gitnexus://repo/BeeperTX/clusters` | All functional areas |
| `gitnexus://repo/BeeperTX/processes` | All execution flows |
| `gitnexus://repo/BeeperTX/process/{name}` | Step-by-step execution trace |

## CLI

| Task | Read this skill file |
|------|---------------------|
| Understand architecture / "How does X work?" | `.claude/skills/gitnexus/gitnexus-exploring/SKILL.md` |
| Blast radius / "What breaks if I change X?" | `.claude/skills/gitnexus/gitnexus-impact-analysis/SKILL.md` |
| Trace bugs / "Why is X failing?" | `.claude/skills/gitnexus/gitnexus-debugging/SKILL.md` |
| Rename / extract / split / refactor | `.claude/skills/gitnexus/gitnexus-refactoring/SKILL.md` |
| Tools, resources, schema reference | `.claude/skills/gitnexus/gitnexus-guide/SKILL.md` |
| Index, status, clean, wiki CLI commands | `.claude/skills/gitnexus/gitnexus-cli/SKILL.md` |

<!-- gitnexus:end -->
