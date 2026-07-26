# Sweeper 追频/追码模块 — 详细设计文档

> **版本:** v3 — 最终版  
> **日期:** 2026-07-22  
> **项目:** BeeperTX — Si4463 POCSAG/FLEX 寻呼发射机

---

## 目录

1. [概述](#1-概述)
2. [模块架构](#2-模块架构)
3. [类设计](#3-类设计)
4. [POCSAG 批量编码设计](#4-pocsag-批量编码设计)
5. [FLEX 追码设计](#5-flex-追码设计)
6. [追频设计](#6-追频设计)
7. [状态机与调度](#7-状态机与调度)
8. [命令接口](#8-命令接口)
9. [Web 界面设计](#9-web-界面设计)
10. [TX 延时优化](#10-tx-延时优化)
11. [性能估算](#11-性能估算)
12. [文件更改清单](#12-文件更改清单)
13. [风险与注意事项](#13-风险与注意事项)

---

## 1. 概述

### 1.1 需求

在现有 BeeperTX 固件基础上增加两种扫描功能：

| 功能 | 目标 | 已知 | 未知 |
|------|------|------|------|
| **追码** (Address Sweep) | 找出寻呼机的地址码 | 发射频率 | 地址码 |
| **追频** (Frequency Sweep) | 找出寻呼机的工作频率 | 地址码 | 工作频率 |

### 1.2 设计目标

- 独立模块，不破坏现有发射功能
- 非阻塞运行，不阻塞主循环
- POCSAG 追码通过批量拼装加速（地址复用前导码）
- FLEX 追码使用完整帧逐地址发送（不改 flex.h）
- 追频同时支持 POCSAG/FLEX
- 范围扫完自动停止，STOP 命令也可停止
- 消息仅发送纯数字（地址值/频率值）

---

## 2. 模块架构

```
┌─────────────────────────────────────────────────────────┐
│                     main.cpp                             │
│  setup(): HW init → API init → Web init                  │
│  loop(): Si446x_SERVICE → wifi → serial → api → sweeper │
└──────────┬──────────────────────────────────┬────────────┘
           │                                  │
           ▼                                  ▼
┌──────────────────────┐      ┌────────────────────────────┐
│    API 层 (API.cpp)  │      │  Sweeper (新模块)           │
│                      │      │                            │
│  CMD_SWEEP_START ────┼──────►  start(cfg)                │
│  CMD_SWEEP_STOP ─────┼──────►  stop()                    │
│  CMD_SWEEP_STATUS ◄──┼─────── getStatus()                │
└──────────────────────┘      │  loop() ← 主循环每帧调用   │
                              │    ├─ runAddrBatch()        │
                              │    └─ runFreqPoint()        │
                              └────────┬───────────────────┘
                                       │
                              ┌────────▼───────────────┐
                              │  Transmitter + 编码器   │
                              │  PocsagEncoder          │
                              │  FlexEncoder            │
                              │  Si446x 驱动            │
                              └────────────────────────┘
```

---

## 3. 类设计

### 3.1 头文件 — `lib/Sweeper/Sweeper.h`

```cpp
#pragma once

#include <stdint.h>
#include "../Transmitter/Transmitter.h"

/**
 * @brief 追频/追码扫描器 — 独立状态机模块
 *
 * 两种扫描模式:
 *   - 追码: 遍历地址 (POCSAG 批量 / FLEX 逐帧)
 *   - 追频: 遍历频率 (换频 + 标准帧)
 *
 * 非阻塞运行, 由主循环 loop() 每帧调用 Sweeper::loop()。
 */
class Sweeper {
public:
  /** 扫描模式 */
  enum Mode : uint8_t {
    MODE_ADDR = 0,   //!< 追码: 遍历地址
    MODE_FREQ = 1    //!< 追频: 遍历频率
  };

  /** 扫描配置 */
  struct Config {
    // 通用
    Mode     mode        = MODE_ADDR;

    // 追码参数 (mode = MODE_ADDR)
    uint32_t addrStart   = 0;          // 起始地址 (Web UI: 取当前面板值)
    uint32_t addrEnd     = 9999999;    // 终止地址 (固定, 用户不可见)
    uint8_t  addrStep    = 1;          // 地址步进 (固定, 用户不可见)
    uint8_t  batchSize   = 10;         // POCSAG 每批地址数

    // 追频参数 (mode = MODE_FREQ)
    uint32_t freqStart   = 150000;     // 起始频率 kHz (Web UI: 取当前面板值)
    uint32_t freqEnd     = 999999;     // 终止频率 kHz (固定, 用户不可见)
    uint32_t freqStep    = 25;         // 频率步进 kHz (固定, 用户不可见)

    // 发射参数 (贯通到 Transmitter::Config)
    uint8_t  encoding    = 0;          // 0=POCSAG, 1=FLEX
    uint8_t  phase       = 0;          // 0=NORMAL, 1=INVERTED
    uint8_t  tone        = 0;          // TONE_A~D
    uint8_t  msgType     = 1;          // 0=NUMERIC, 1=ALPHANUMERIC
    uint16_t baudRate    = 1200;       // 512/1200/1600/...
    uint16_t intervalMs  = 500;        // 批次/频率点 间隔 (ms)
  };

  /** 扫描状态 (运行时只读) */
  struct Status {
    bool     running    = false;   // 是否运行中
    Mode     mode       = MODE_ADDR;
    uint32_t current    = 0;       // 当前地址/频率
    uint32_t total      = 0;       // 总步数
    uint32_t done       = 0;       // 已完成
  };

  Sweeper();

  /** 注入 Transmitter 实例 (由 main.cpp 调用) */
  void setTransmitter(Transmitter* tx) { m_tx = tx; }

  /** 启动扫描 (返回 false = 参数非法) */
  bool start(const Config& cfg);

  /** 停止扫描 */
  void stop();

  /** 是否正在运行 */
  bool isRunning() const { return m_running; }

  /** 读取当前状态 */
  Status getStatus() const;

  /** 获取当前配置 (只读) */
  const Config& getConfig() const { return m_cfg; }

  /** 非阻塞调度 — 由主循环每帧调用 */
  void loop();

private:
  Transmitter* m_tx = nullptr;
  Transmitter  m_dummy;              //!< 占位构造 (实际用注入的指针)
  Config       m_cfg;
  bool         m_running  = false;
  uint32_t     m_nextAddr = 0;       // 追码: 下一个待发地址
  uint32_t     m_nextFreq = 0;       // 追频: 下一个待发频率
  uint32_t     m_total    = 0;       // 总步数
  uint32_t     m_done     = 0;       // 已完成
  uint32_t     m_lastTick = 0;       // 上次操作的时间戳 (ms)

  // ---- 内部方法 ----
  bool calcTotalSteps();             // 计算总步数
  void logProgress();                // 日志输出进度

  // ---- 追码 ----
  void runAddrBatch();               // 发一批地址 (POCSAG 批量/FLEX 单帧)

  // POCSAG 批量拼装
  bool buildPocsagBatch(             // 拼装一批地址的基带
    unsigned long* outBuf, int& outLen, int maxLen,
    uint32_t* addrs, int addrCount,
    const Transmitter::Config& txCfg);

  // ---- 追频 ----
  void runFreqPoint();               // 切频 + 发射一次
};
```

### 3.2 核心数据结构

| 结构体 | 用途 | 存储位置 |
|--------|------|----------|
| `Sweeper::Config` | 扫描参数字段，由 START 命令填充 | 栈/持久化 |
| `Sweeper::Status` | 运行时进度，只读查询 | 堆/API |

---

## 4. POCSAG 批量编码设计

### 4.1 原理

POCSAG 帧结构：一个 batch = `[SYNC] + 8 帧 × 2 CW` = 17 个码字。

每帧 2 个码字可以放：
- `[地址 CW][消息 CW]` — 发给此地址的一条消息
- `[地址 CW][地址 CW]` — 同一帧内两个不同地址
- `[空闲 CW][空闲 CW]` — 空帧

**关键发现：** 连续 N 个地址依次均匀分布在 8 帧中。例如地址 0~7 分别在帧 0~7。
每帧放一个地址 + 一条短消息（1 CW），一个 batch 可以处理 **最多 8 个地址**。

对于 batchSize > 8，追加第 2 个 batch（又一个 SYNC + 8 帧）。

### 4.2 批量帧结构

```
batchSize ≤ 8 → 1 组:

[SYNC CW]
[Frame 0] [Addr_n+0][Msg_n+0]   ← 地址在帧 0
[Frame 1] [Addr_n+1][Msg_n+1]   ← 地址在帧 1
[Frame 2] [Addr_n+2][Msg_n+2]
[Frame 3] [Addr_n+3][Msg_n+3]
[Frame 4] [Addr_n+4][Msg_n+4]
[Frame 5] [Addr_n+5][Msg_n+5]
[Frame 6] [Addr_n+6][Msg_n+6]
[Frame 7] [Addr_n+7][Msg_n+7]
[IDLE] × 16

batchSize = 10 → 2 组:

[SYNC CW]                        ← 组 1 开始
[F0] [Addr_n+0][Msg_n+0]
[F1] [Addr_n+1][Msg_n+1]
...
[F7] [Addr_n+7][Msg_n+7]
[SYNC CW]                        ← 组 2 开始
[F0] [Addr_n+8][Msg_n+8]
[F1] [Addr_n+9][Msg_n+9]
[F2] [空闲][空闲]   ← 剩余帧填充空闲
...
[F7] [空闲][空闲]
[IDLE] × 16
```

### 4.3 消息编码细节

每条消息 = 地址数值的十进制字符串，例如地址 `1234` → `"1234"`。

经 `deCodeAlpha()` 编码为 POCSAG 消息码字。因信息很短（4~7 位数字 = 1~2 个消息 CW），**每组消息只需 1~2 个 CW**，不会出现消息跨组分裂。

### 4.4 流程图

```
runAddrBatch()
  │
  ├── mode == POCSAG ──► buildPocsagBatch()
  │                       │
  │                       ├── 取 min(batchSize, remaining) 个地址
  │                       ├── 对每个地址 genAddrCode()
  │                       ├── 对每个地址 genMsgCode("1234")
  │                       ├── 按帧号排入 baseband[]
  │                       │    (多 batch 依次编排)
  │                       └── 返回 CW 数组 (不含 preamble)
  │
  │               enterTx()
  │               sendPreambleBits()
  │               sendCodewords()         ← 批量 CW 数组
  │               exitTx()
  │
  ├── mode == FLEX ──► 构建单地址 FlexEncoder
  │               enterTx()
  │               sendBytes()
  │               exitTx()
  │
  ├── 更新 m_done, m_nextAddr
  └── 等待 m_cfg.intervalMs
```

### 4.5 `buildPocsagBatch()` 算法伪代码

```
输入: outBuf[], &outLen, maxLen
      addrs[addrCount]    — 本次要发的地址列表
      baudRate, invert    — 来自 config
输出: outBuf = CW array (不含 preamble)
      outLen = CW 数量

算法:
  1. groups = ceil(addrCount / 8)    // 需要几组 batch
  2. 对每组 g = 0..groups-1:
     a. 写入 SYNC CW (0x7CD215D8)
     b. 对 frame = 0..7:
        - idx = g * 8 + frame
        - 若 idx < addrCount:
           写入 genAddrCode(addr[idx], func)    // 地址 CW
           写入 genMsgCode(msgData)              // 消息 CW (编号数据)
        - 否则:
           写入 IDLE × 2
  3. 写入 16 × IDLE
  4. 若 invert: 所有 CW 取反
```

---

## 5. FLEX 追码设计

### 5.1 原则

**不对 flex.h 做任何改造。** FLEX 的帧结构固定（ERS + S1 + FIW + S2 + 11 块 × 8 字），比 POCSAG 复杂，不支持批量拼装。

### 5.2 实现

每次调用 `runAddrBatch()` → FLEX 分支：

```cpp
void Sweeper::runAddrBatch() {
  if (m_cfg.encoding == FLEX) {
    // FLEX: 每批只发 1 个地址 → 调用现有 FlexEncoder
    char msgBuf[16];
    snprintf(msgBuf, sizeof(msgBuf), "%lu", m_nextAddr);

    Transmitter::Config txCfg = buildTxCfg();
    txCfg.message = msgBuf;
    m_tx->send(txCfg);  // 标准 FlexEncoder 路径
  }
}
```

### 5.3 FLEX 追码时序 (1600 baud)

| 环节 | 时间 |
|------|------|
| Direct TX 锁定 | 2 ms |
| 完整 FLEX 帧 (ERS~数据) | ~1.875 s |
| **合计/地址** | **~1.88 s** |
| 1000 个地址 | ~1880 s ≈ 31 min |

> POCSAG 批量 10 个/批处理 1000 个地址约 2 min，FLEX 因无法批量会慢很多。这在预期内。

---

## 6. 追频设计

### 6.1 原理

已知寻呼机地址，未知工作频率 → 遍历频率范围，每次切换频率后发送标准完整帧。

```cpp
void Sweeper::runFreqPoint() {
  // 1. 切换频率
  Si446x_setFrequencyKHz(m_nextFreq);
  delay(2);  // PLL 稳定时间 (200µs + 余量)

  // 2. 构建标准发射配置 (地址已知)
  Transmitter::Config txCfg = buildTxCfg();

  // 3. 发射 (POCSAG 或 FLEX 完整帧)
  m_tx->send(txCfg);

  // 4. 推进
  m_done++;
  m_nextFreq += m_cfg.freqStep;
}
```

### 6.2 频率切换流程

```
Si446x_setFrequencyKHz(freq)
  ├── 计算 OUTDIV / CLKGEN_BAND / FREQ_CONTROL 寄存器值
  ├── 写入 FREQ_CONTROL_INTE, FRAC_2/1/0
  ├── 写入 MODEM_CLKGEN_BAND (若频段切换)
  ├── 等待 CTS
  └── 返回

(总耗时 < 500 µs, 实际设频后需 PLL 重新锁定 ~200 µs)

Si446x_startDirectTX(channel, async=1)
  ├── 进入 IDLE → 配置 MODEM_MOD_TYPE → START_TX
  └── 等待 PLL 锁定 → GPIO0 开始逐位输出
```

### 6.3 追频配置兼容性

| 参数 | POCSAG | FLEX |
|------|--------|------|
| 追频支持 | ✅ | ✅ |
| 需改编码器 | 否 | 否 |
| 消息内容 | 频率数值 | 频率数值 |

---

## 7. 状态机与调度

### 7.1 状态图

```
         ┌──────────────────────────────────┐
         │        IDLE                      │
         │   m_running = false              │
         └────┬─────────────────────────────┘
              │ SWEEP START (mode=addr/freq)
              ▼
         ┌──────────────────────────────────┐
         │     SWEEPING                     │
         │   m_running = true              │
         │                                  │
         │   每帧 loop():                   │
         │     if (now - lastTick < interval)│
         │       return  → 跳过(非阻塞)     │
         │                                  │
         │     if (mode == ADDR):           │
         │       runAddrBatch()             │
         │       m_nextAddr += addrStep     │
         │                                  │
         │     if (mode == FREQ):           │
         │       runFreqPoint()             │
         │       m_nextFreq += freqStep     │
         │                                  │
         │     if (done >= total):          │
         │       → IDLE  (自动停止)          │
         └────┬─────────────────────────────┘
              │ SWEEP STOP / 范围完成
              ▼
         ┌──────────────────────────────────┐
         │        IDLE                      │
         │   log "sweep complete"           │
         └──────────────────────────────────┘
```

### 7.2 `loop()` 实现

```cpp
void Sweeper::loop() {
  if (!m_running || !m_tx) return;

  // 间隔节流 (非阻塞)
  uint32_t now = millis();
  if (now - m_lastTick < m_cfg.intervalMs) return;

  // 检测是否完成
  if (m_done >= m_total) {
    stop();
    api_log("sweep complete");
    return;
  }

  // 执行一个工作单元
  if (m_cfg.mode == MODE_ADDR) {
    runAddrBatch();
  } else {
    runFreqPoint();
  }

  m_lastTick = now;
  logProgress();
}
```

### 7.3 间隔参数

| 场景 | 推荐间隔 | 说明 |
|------|----------|------|
| 追码 POCSAG 批量 | 300~500 ms | 寻呼机解码 + 用户观察时间 |
| 追码 FLEX 单帧 | 500~1000 ms | 同上，FLEX 帧已较长 |
| 追频 | 500~1000 ms | 换频 + 寻呼机同步时间 |

---

## 8. 命令接口

### 8.1 API 枚举扩展 (`API.h`)

```cpp
enum class CmdID : uint8_t {
  // ... 现有命令 ...

  CMD_SWEEP_START  = 0x50,   //!< 启动扫描
  CMD_SWEEP_STOP   = 0x51,   //!< 停止扫描
  CMD_SWEEP_STATUS = 0x52,   //!< 查询扫描状态
};
```

`CmdRequest` union 新增字段：

```cpp
struct CmdRequest {
  CmdID cmd;
  union {
    // ... 现有字段 ...
    Sweeper::Config sweepConfig;    // CMD_SWEEP_START
  };
};
```

`CmdResponse` union 新增字段：

```cpp
struct CmdResponse {
  CmdID cmd; RespCode code; int32_t detail;
  union {
    // ... 现有字段 ...
    Sweeper::Status sweepStatus;    // CMD_SWEEP_STATUS
  };
};
```

### 8.2 API Handler (`API.cpp`)

```cpp
static void handleSweepStart(const CmdRequest* req, CmdResponse* resp) {
  if (!s_sweeper) { resp->code = ERR_HW; return; }
  if (s_sweeper->isRunning()) { resp->code = ERR_BUSY; return; }

  // 从 req->sweepConfig 只读取 mode + 发射参数
  // 起始 addr/freq 由调用方从当前 TxConfig 提取并填入
  bool ok = s_sweeper->start(req->sweepConfig);
  resp->code = ok ? OK : ERR_PARAM;
}

/** Web UI 快捷入口: 从当前 TxConfig 填充起始值 */
void api_sweepStartSimple(uint8_t mode) {
  CmdRequest getR; CmdResponse getResp;
  memset(&getR, 0, sizeof(getR));
  getR.cmd = CmdID::CMD_GET_CONFIG;
  api_dispatch(&getR, &getResp);

  Sweeper::Config scfg;
  scfg.mode = (mode == 0) ? Sweeper::MODE_ADDR : Sweeper::MODE_FREQ;
  scfg.encoding = getResp.config.encoding;
  scfg.baudRate = getResp.config.baudRate;
  scfg.phase    = getResp.config.phase;
  scfg.tone     = getResp.config.tone;
  scfg.msgType  = getResp.config.msgType;

  if (mode == 0) {
    scfg.addrStart = getResp.config.address;
    scfg.freqStart = scfg.freqEnd = 0;       // 追码忽略频率
  } else {
    scfg.freqStart = getResp.config.freqKHz;
    scfg.freqEnd   = 999999;
    scfg.addrStart = scfg.addrEnd = 0;        // 追频忽略地址
  }

  CmdRequest req; CmdResponse resp;
  memset(&req, 0, sizeof(req));
  req.cmd = CmdID::CMD_SWEEP_START;
  req.sweepConfig = scfg;
  api_dispatch(&req, &resp);
}

static void handleSweepStop(const CmdRequest* req, CmdResponse* resp) {
  if (!s_sweeper) { resp->code = ERR_HW; return; }
  s_sweeper->stop();
  resp->code = OK;
}

static void handleSweepStatus(const CmdRequest* req, CmdResponse* resp) {
  if (!s_sweeper) { resp->code = ERR_HW; return; }
  resp->code = OK;
  resp->sweepStatus = s_sweeper->getStatus();
}
```

### 8.3 串口命令 (`SerialConsole.cpp`)

```
SWEEP START addr                             ← 追码 (从当前地址开始)
SWEEP START freq                             ← 追频 (从当前频率开始)

SWEEP START addr [encoding=pocsag] [baud=1200] [interval=500]  ← 带参数覆盖
SWEEP START freq [encoding=flex] [interval=1000]

SWEEP STOP                                   ← 停止扫描

SWEEP STATUS                                 ← 显示当前进度
```

串口输出示例：

```
> SWEEP START addr
OK SWEEP started  mode=addr addr=1000→9999999 batch=10

> SWEEP STATUS
OK SWEEP running mode=addr current=1050 done=50/9999000

> SWEEP STOP
OK SWEEP stopped at addr=1050

> SWEEP START freq
OK SWEEP started  mode=freq freq=150000→999999 step=25

> SWEEP STATUS
OK SWEEP idle mode=freq done=34000/34000  ← 完成
```

### 8.4 `main.cpp` 集成

```cpp
#include "Sweeper.h"

static Sweeper s_sweeper;

void setup() {
  // ... 现有初始化 ...
  s_sweeper.setTransmitter(&s_tx);
  api_setSweeper(&s_sweeper);
}

void loop() {
  Si446x_SERVICE();
  s_wifi.loop();
  s_serial.loop();
  api_loop();

  s_sweeper.loop();              // ← 新增: Sweeper 非阻塞调度
}
```

---

## 9. Web 界面设计

### 9.1 设计理念

**极简操作** — 不设 start/end/step 输入框：

| 按钮 | 起始值 | 终止值 | 步进 |
|------|--------|--------|------|
| 追码 | 从面板 Address 字段当前值开始 | 9999999 (固定) | 1 (固定) |
| 追频 | 从面板 Frequency 字段当前值开始 | 999999 (固定) | 25 kHz (固定) |

所有扫频/扫码进度输出到右侧终端 (`terminal-output`)，不另建独立状态区。

### 9.2 UI 布局调整

**移除：**
```
[Repeat / Interval]              ← 整个 fieldset 删除
  Repeat: [2]  Interval ms: [2000]

[Actions]                         ← 整个 fieldset 删除
  [▶ Loop Start]
  [■ Loop Stop]
  [Save Preset] [Load]
```

**替换为：**

```
[Sweep — 追码/追频]               ← 新的 fieldset, 放在 Preset 前面
  [▶ 追码] [▶ 追频] [■ STOP]

[Actions]                          ← 仅保留 Preset 功能
  [Save Preset] [Load]
  [-- saved presets --]
```

### 9.3 交互流程

```
用户点击 [▶ 追码]
  │
  ├── 获取面板当前 Address 值 → 作为 addrStart
  ├── API.sweepStart({mode:'addr', addrStart: <当前值>})
  │   └── terminal 输出: ">>> 追码开始  addr=1000 → 9999999  batch=10"
  └── 按钮状态: [▶ 追码][▶ 追频] disabled, [■ STOP] enabled

逐批进行中 (由 loop() 驱动, 不阻塞)：
  └── WebSocket / 轮询 推送到 terminal:
      ">>> addr=1200  (200/9999000)"

用户/完成 停止：
  ├── 用户点 [■ STOP] → API.sweepStop()
  │   └── terminal: ">>> 追码停止  addr=1234"
  └── 范围完成 → terminal: ">>> 追码完成  addr=1000→9999999  done=9999000"
      └── 按钮恢复
```

### 9.4 REST API 端点

| 方法 | 路由 | 请求体 | 响应 |
|------|------|--------|------|
| `POST` | `/api/sweep/start` | `{"mode":"addr"}` 或 `{"mode":"freq"}` | `{"code":0}` |
| `POST` | `/api/sweep/stop` | - | `{"code":0}` |
| `GET` | `/api/sweep/status` | - | `{"running":true/false,"mode":"addr","current":1200,"total":9999000,"done":200}` |

追码/追频的起始值取自当前 `TxConfig`：
- 追码: `config.address` → `sweepConfig.addrStart`
- 追频: `config.freqKHz` → `sweepConfig.freqStart`

### 9.5 终端输出格式

终端的 sweep 信息通过两种方式更新：

**方案 A — 日志轮询（优先实现）**

Sweeper 每步调用 `api_log()` 写入日志环形缓冲区：

```
SWEEP: mode=addr addr=1200 done=200/9999000
```

客户端 `GET /api/log` 轮询获取并显示。利用现有 `info` 行样式：

```
[10:30:01] >>> 追码开始  addr=1000→9999999  batch=10  POCSAG
[10:30:02] >>> addr=1100  done=100/9999000
[10:30:03] >>> addr=1200  done=200/9999000
[10:30:04] >>> 追码停止  addr=1234  (STOP)
```

日志频率：**每批/每点输出一行**（POCSAG 每 10 个地址一行，FLEX 每个地址一行）。

**方案 B — SSE/EventSource（后续增强）**

`GET /api/sweep/events` 返回 `text/event-stream`：

```
event: sweep_progress
data: {"mode":"addr","current":1200,"done":200,"total":9999000}

event: sweep_complete
data: {"mode":"addr","done":9999000}

event: sweep_stopped
data: {"mode":"addr","current":1234}
```

### 9.6 前端更改清单

| 文件 | 更改 |
|------|------|
| `data/index.html` | 移除 Repeat/Interval + Actions 中的 Loop 按钮；新增 Sweep fieldset |
| `data/js/api-client.js` | 新增 `sweepStart(mode)`、`sweepStop()`、`sweepStatus()` |
| `data/js/config-panel.js` | 新增 `handleSweepAddr()`、`handleSweepFreq()`、`handleSweepStop()`；移除 Loop 控制 |
| `data/js/main.js` | 可选: 轮询 sweepStatus 写入 terminal |
| `data/css/style.css` | 可选: sweep 按钮特殊配色 |

### 9.7 按钮状态管理

```
初始化:
  [▶ 追码] [▶ 追频]  enabled
  [■ STOP]             disabled

扫描进行中:
  [▶ 追码] [▶ 追频]  disabled
  [■ STOP]             enabled

扫描停止/完成:
  [▶ 追码] [▶ 追频]  enabled
  [■ STOP]             disabled
```

---

## 10. TX 延时优化

### 10.1 TX_SETTLE_MS 调整

```cpp
// Transmitter.cpp — 此行修改
static const int TX_SETTLE_MS   = 2;  // 原 1000ms → 2ms
```

| 状态 | 原值 | 新值 | 依据 |
|------|------|------|------|
| `TX_SETTLE_MS` | 1000 ms | **2 ms** | Si4463 数据手册: PLL 锁定时序典型值 200 µs, 2 ms 含 10 倍余量 |

### 10.2 影响范围

```cpp
void Transmitter::enterTx() {
  Si446x_startDirectTX(0, 1);
  delay(TX_SETTLE_MS);  // 所有发射路径均受益
}
```

| 场景 | 原时间 | 优化后 | 加速比 |
|------|--------|--------|--------|
| 单次 POCSAG 发射 | 1000ms + 帧时间 | 2ms + 帧时间 | ~500x |
| 追码 1 批 (POCSAG) | 1000+1360=2360ms | 2+1360=1362ms | 1.7x |
| 追频 1 点 | 1000+960=1960ms | 2+960=962ms | 2x |
| 追频 40 点 (1MHz范围) | ~78s | ~38s | 2x |

---

## 11. 性能估算

### 11.1 追码性能

| 参数 | POCSAG (1200 baud) | FLEX (1600 baud) |
|------|--------------------|------------------|
| 批量大小 | 10 地址/批 | 1 地址/次 |
| 每步耗时 | ~1.36 s + 0.5s 间隔 | ~1.88 s + 0.5s 间隔 |
| 每步处理地址数 | 10 | 1 |
| 平均速度 | ~5.4 地址/s | ~0.42 地址/s |
| 扫 1000 地址 | **~3.1 min** | ~39.7 min |
| 扫 10000 地址 | **~31 min** | ~6.6 h |

### 11.2 追频性能

| 参数 | POCSAG (1200 baud) | FLEX (1600 baud) |
|------|--------------------|------------------|
| 每步耗时 | ~0.96 s + 0.5s 间隔 | ~1.88 s + 0.5s 间隔 |
| 150000→151000 (40步) | **~58 s** | ~95 s |
| 150000→160000 (400步) | **~9.7 min** | ~15.8 min |

### 11.3 总线时间对比（不批量 vs 批量）

**场景：** 追码 100 个地址 (POCSAG 1200 baud)

```
不批量 (逐地址):
  100 × (preamble 480ms + batch 880ms + 间隔 500ms) = 100 × 1.86s = 186s ≈ 3.1 min

批量 (batch=10):
  10组 × (preamble 480ms + 2批batch 1760ms + 间隔 500ms) = 10 × 2.74s = 27.4s ≈ 0.46 min

加速比: ~6.8 倍
```

---

## 12. 文件更改清单

### 12.1 新增文件

| 文件 | 内容 | 预估行数 |
|------|------|----------|
| `lib/Sweeper/Sweeper.h` | 类定义 + Config/Status | ~120 |
| `lib/Sweeper/Sweeper.cpp` | 状态机 + 批量编码 + 追频 | ~350 |

### 12.2 修改文件

| 文件 | 修改内容 | 影响 |
|------|----------|------|
| `src/Controller/Transmitter.cpp` | `TX_SETTLE_MS` 1000→2 | 所有发射路径 |
| `src/Controller/API.h` | 新增 `SweepConfig`、`CmdID::CMD_SWEEP_*`、union字段 | API 接口 |
| `src/Controller/API.cpp` | 新增 handleSweepStart/Stop/Status、s_sweeper 注入 | 命令调度 |
| `src/Controller/SerialConsole.h` | 新增 `cmdSweep()` | 串口层 |
| `src/Controller/SerialConsole.cpp` | `SWEEP` 命令解析+执行 | 串口层 |
| `src/main.cpp` | `#include Sweeper.h`、loop() 中 `s_sweeper.loop()` | 总入口 |
| `src/Controller/WebServer.h/cpp` | 新增 REST + WebSocket sweep 路由 | Web UI |

---

## 13. 风险与注意事项

### 13.1 POCSAG 批量帧兼容性

**风险：** 标准 POCSAG 协议要求地址必须放在其正确的帧槽位（由 `addr & 0x07` 决定）。本设计中地址序列呈连续排列，连续 N 个地址自然分布在 0~(N-1) 帧中,寻呼机若严格按帧号匹配应能接收。

**缓解：** 如果遇到某些寻呼机不识别非标准帧内的地址，可降级为逐地址发送（`batchSize=1`）。

### 13.2 忙等待阻塞

**现状：** `Transmitter::sendPreambleBits()`/`sendCodewords()` 使用 `while(esp_timer_get_time() < next)` 忙等待，发射期间阻塞主循环约 1-2 秒。

**影响：** Sweeper 的 `loop()` 仅在发射完成后才能返回。这是发射器层面的既有限制。

**缓解：** 发射期间仍会每 500bit `esp_task_wdt_reset()` 防 WDT。Sweeper 的间隔参数 (`intervalMs`) 确保发射之间有足够的空闲时间给主循环处理 WiFi/串口。

### 13.3 Si446x_setFrequencyKHz() 调用时机

**注意：** 必须在 `exitTx()` 之后、下一次 `enterTx()` 之前调用。Si4463 不允许在 TX 活跃时修改频率寄存器。当前 `runFreqPoint()` 顺序为：

```cpp
m_tx->send(txCfg);    // 内含 enterTx→send→exitTx
// 此时 Si4463 已回到 IDLE
Si446x_setFrequencyKHz(m_nextFreq + m_cfg.freqStep);  // 安全切换
```

### 13.4 日志输出频率

每批/每次发射都输出日志会刷屏。建议：

- 每完成 10%（或每 50 步）输出一次汇总
- 异常/完成/停止时输出详细行
- 串口 `SWEEP STATUS` 支持人工查询当前进度

### 13.5 内存使用

POCSAG 批量编码需要临时缓冲区存放 CW 数组。

| 最大 batchSize | 最大 CW 数 | 缓冲区大小 (4B/CW) |
|---------------|-----------|-------------------|
| 10 | ~70 CW | ~280 bytes (栈) |
| 20 | ~135 CW | ~540 bytes (栈) |

使用 `static` 局部缓冲区或成员数组，避免栈溢出。现有 `PocsagEncoder` 的 `MAX_BASEBAND = 768`（~3KB），Sweeper 的临时缓冲区应使用独立空间不超过此量。

---

## 附录 A: 串口命令完整参考

```
SWEEP START addr [encoding=pocsag|flex] [baud=<rate>]
                [phase=norm|inv] [tone=a|b|c|d]
                [msgType=num|alpha] [interval=<ms>]
  启动追码，从当前地址到 9999999

SWEEP START freq [encoding=pocsag|flex] [baud=<rate>]
                [phase=norm|inv] [tone=a|b|c|d]
                [interval=<ms>]
  启动追频，从当前频率到 999999 kHz，步进 25kHz

SWEEP STOP
  立即停止扫描

SWEEP STATUS
  查询扫描状态
```

---

## 附录 B: 后续扩展（不在本次范围）

- **追码自适应加速：** 如果长时间没有收到反馈，自动增大步进（如 1→10→100）先粗扫再精扫。
- **命中记录：** 预留 `Status.hits` 字段，未来配合 RTL-SDR 或寻呼机回传信号自动标记命中频率/地址。
- **NVS 持久化：** 保存 SweepConfig 到 NVS，重启后恢复上次扫描参数。
- **预设场景：** 预置常用频率范围（如 137-174 MHz VHF、406-470 MHz UHF 寻呼频段）。
