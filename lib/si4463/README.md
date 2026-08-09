# Si4463 射频模块使用手册

> **版本**: 1.1  
> **芯片**: Si4463 Rev B1 (兼容 C2A)  
> **MCU**: ESP32-S3 (Arduino 框架)  
> **库**: Zak Kemble Si446x 移植版 + Direct TX / RF 开关 / 频率控制增强  
> **日期**: 2026-08-06

---

## 目录

1. [概述](#1-概述)
2. [接线与引脚](#2-接线与引脚)
3. [快速开始](#3-快速开始)
4. [API 参考](#4-api-参考)
5. [WDS 配置指南](#5-wds-配置指南)
6. [频率计算](#6-频率计算)
7. [Direct TX 模式](#7-direct-tx-模式)
8. [RF 天线开关](#8-rf-天线开关)
9. [FIFO 收发模式](#9-fifo-收发模式)
10. [状态与寄存器读取](#10-状态与寄存器读取)
11. [常见问题与排错](#11-常见问题与排错)
12. [附录](#12-附录)

---

## 1. 概述

Si4463 是 Silicon Labs 的 Sub-GHz 射频收发器芯片，支持：
- **频率范围**: 119–1050 MHz（分 6 个频段）
- **调制方式**: (G)FSK, 4(G)FSK, (G)MSK, OOK
- **数据速率**: 0.1 bps – 1 Mbps
- **TX 功率**: 最高 +20 dBm（100 mW）
- **灵敏度**: 典型 -124 dBm @ 1.2 kbps
- **接口**: SPI（命令/状态）+ 可选 GPIO 直连 TX/RX 数据

本手册对应的 ESP32 移植库在此库基础上增加了：
- **Direct TX 模式**（GPIO 直连数据输出，不占 FIFO）
- **RF 天线开关控制**（GPIO2 = TX_STATE, GPIO3 = RX_STATE）
- **运行时频率控制**（`Si446x_setFrequencyKHz()`）
- **频率回读**（`Si446x_getFrequencyKHz()`）

---

## 2. 接线与引脚

### 2.1 硬件接线

| Si4463 引脚 | ESP32-S3 | 功能 |
|-------------|----------|------|
| SDN         | GPIO5    | 关机复位 (高=复位) |
| nSEL/CS     | GPIO10   | SPI 片选 (低有效) |
| SCLK        | GPIO12   | SPI 时钟 |
| SDI/MOSI    | GPIO11   | SPI 主出从入 |
| SDO/MISO    | GPIO13   | SPI 主入从出 |
| nIRQ        | (可选)   | 中断输出 (低有效)。ESP32 固件默认轮询模式 (`PIN_NIRQ=-1`), 可不接; 若接, GPIO 由应用方定义 |
| GPIO0       | GPIO16   | Direct TX 数据输出 |
| VDD         | 3V3      | 电源 (3.3V) |
| GND         | GND      | 共地 |

### 2.2 天线开关 (RF Switch)

若采用单天线半双工设计，Si4463 的 GPIO2/GPIO3 可输出状态信号控制 RF 开关
（由芯片直接驱动，**不占用 MCU 引脚**）：

| Si4463 引脚 | 模式值 | TX 时 | RX 时 |
|-------------|--------|-------|-------|
| GPIO2       | 0x21 (TX_STATE) | HIGH | LOW |
| GPIO3       | 0x20 (RX_STATE) | LOW | HIGH |

芯片侧引脚完整分配（WDS 配置 + `Si446x_configRFswitch()`）:

| Si4463 引脚 | 模式 | 用途 |
|-------------|------|------|
| GPIO0 | INPUT | Direct TX 数据输入 (bit-bang 调制) |
| GPIO1 | TX_DATA_CLK | 数据时钟输出 (外部数据源同步, 通常不用) |
| GPIO2 | TX_STATE | RF 开关: 发射时 HIGH |
| GPIO3 | RX_STATE | RF 开关: 接收时 HIGH |

未使用 RF 开关时 GPIO1/2/3 可全部悬空，不影响功能。

### 2.3 引脚配置 (应用方定义)

引脚是**板级硬件信息**, 库头文件不再内置默认引脚宏 — 由应用方定义并
在 `Si446x_init()` 之前通过 `Si446x_setPins()` 传入。

BeeperTX 中定义在 `src/main.cpp` 顶部:

```c
// main.cpp
#define PIN_CS       10   // nSEL (SPI 片选)
#define PIN_SDN       5   // SDN (关断)
#define PIN_TX_DATA  16   // → Si4463 GPIO0 (TX 调制数据)
#define PIN_SCLK     12   // SPI SCK
#define PIN_MISO     13   // SPI MISO (SDO)
#define PIN_MOSI     11   // SPI MOSI (SDI)
#define PIN_NIRQ     -1   // 中断 (-1 = 轮询)

Si446x_setPins(PIN_CS, PIN_SDN, PIN_NIRQ, PIN_TX_DATA,
               PIN_SCLK, PIN_MOSI, PIN_MISO);
```

> **⚠️ IRQ = -1**: ESP32 上建议设为 `-1` 使用轮询模式。AVR 库的中断处理逻辑依赖 `detachInterrupt()`/`attachInterrupt()`，在 ESP32 的 ISR 中调用这些函数会导致 Watchdog 崩溃。

### 2.4 setPins 语义

调用 `Si446x_init()` **之前**:

```c
Si446x_setPins(csn, sdn, irq, txData, sclk, mosi, miso);
// 传 -1 表示保持当前值; 初始均为 -1 (未设置)。
// 除 IRQ 外的 6 个引脚必须设置, 否则 Si446x_init() 打印错误并中止。
```

---

## 3. 快速开始

### 3.1 最简单的 Direct TX 发送程序

```c
#include <Arduino.h>
#include "Si446x.h"

void setup()
{
    Serial.begin(9600);

    // 先指定引脚 (必填 6 个, IRQ 可 -1), 再初始化
    Si446x_setPins(10, 5, -1, 16, 12, 11, 13);

    Si446x_init();          // 初始化芯片，加载 WDS 配置
    Si446x_setTxPower(0x7F); // 最大功率
    Si446x_configRFswitch(); // 配置 RF 开关 (GPIO2/3)

    // 设置频率（可选，WDS 默认频率会自动生效）
    Si446x_setFrequencyKHz(152675); // 152.675 MHz

    Serial.println("Ready");
}

void loop()
{
    Si446x_SERVICE();  // 轮询事件（必须周期调用）

    Si446x_startDirectTX(0);           // 进入 TX 模式
    delay(1000);                       // 让 Si4463 锁相环稳定

    const uint8_t data[] = "Hello RF!";
    Si446x_txDirectWrite(data, sizeof(data)); // 逐位发送

    Si446x_stopTX();                   // 停止 TX, 回到 READY
    delay(5000);
}
```

### 3.2 初始化流程

```
Si446x_setPins(csn, sdn, irq, txData, sclk, mosi, miso)  // 先设引脚 (IRQ 可 -1)
Si446x_init()
 ├── 校验引脚已设置 (未设置 → 打印错误并中止)
 ├── SPI.begin(sclk, miso, mosi, -1)
 ├── SDN 拉高 → 延时 50ms → 拉低（硬件复位）
 ├── applyStartupConfig() 加载 WDS radio_config.h
 ├── interrupt(NULL)        清除所有待处理中断
 ├── Si446x_sleep()         进入 SLEEP → 再回到 READY
 └── 完成

Si446x_setTxPower(pwr)
 └── 设置 PA_PWR_LVL (0-127)

Si446x_configRFswitch()
 └── 发送 GPIO_PIN_CFG 命令
     GPIO0=nothing, GPIO1=nothing,
     GPIO2=TX_STATE, GPIO3=RX_STATE,
     NIRQ=nothing, SDO=nothing
```

---

## 4. API 参考

### 4.1 初始化和配置

| 函数 | 说明 |
|------|------|
| `Si446x_setPins(csn, sdn, irq, txData, sclk, mosi, miso)` | 指定引脚（init 前调用, 必填 6 个, IRQ 可 -1） |
| `Si446x_init()` | 初始化芯片，须先 setPins 再调用 |
| `Si446x_configGPIO(gpio0..sdo, genConfig)` | 自定义 GPIO 配置 |
| `Si446x_configRFswitch()` | 快速配置 GPIO2=TX_STATE, GPIO3=RX_STATE |
| `Si446x_getInfo(&info)` | 读取芯片版本信息 |

### 4.2 Direct TX（直接模式）

| 函数 | 说明 |
|------|------|
| `Si446x_startDirectTX(channel)` | 进入 Direct TX 状态 |
| `Si446x_txDirectWrite(data, len)` | 逐位发送数据（阻塞） |
| `Si446x_stopTX()` | 停止发射，回到 IDLE |

### 4.3 FIFO 模式（传统收发）

| 函数 | 说明 |
|------|------|
| `Si446x_TX(packet, len, channel, onTxFinish)` | 发送 FIFO 数据包 |
| `Si446x_RX(channel)` | 进入接收模式 |
| `Si446x_read(buff, len)` | 读取 RX FIFO |
| `Si446x_SERVICE()` | 轮询处理事件（loop 中周期调用） |
| `Si446x_setupCallback(callbacks, state)` | 启用/禁用回调中断 |

### 4.4 频率控制

| 函数 | 说明 |
|------|------|
| `Si446x_setFrequencyKHz(freqKHz)` | 设置频率（119000–1050000 kHz） |
| `Si446x_setFrequency(inte, frac, step, band)` | 直接写入寄存器值 |
| `Si446x_getFrequencyKHz()` | 读取当前频率（kHz） |

### 4.5 功率控制

| 函数 | 说明 |
|------|------|
| `Si446x_setTxPower(pwr)` | 设置 TX 功率 0–127 |
| `Si446x_getTxPower()` | 读取当前功率值 |

### 4.6 状态与 ADC

| 函数 | 说明 |
|------|------|
| `Si446x_getState()` | 获取芯片状态 (READY/TX/RX/SLEEP…) |
| `Si446x_getRSSI()` | 读取当前 RSSI（dBm） |
| `Si446x_adc_gpio(pin)` | 读取 GPIO 引脚 ADC |
| `Si446x_adc_battery()` | 读取供电电压（mV） |
| `Si446x_adc_temperature()` | 读取芯片温度（°C） |

### 4.7 睡眠与唤醒

| 函数 | 说明 |
|------|------|
| `Si446x_sleep()` | 进入 SLEEP 模式 |
| `Si446x_setupWUT(r, m, ldc, config)` | 配置唤醒定时器 |
| `Si446x_disableWUT()` | 禁用唤醒定时器 |

### 4.8 高级

| 函数 | 说明 |
|------|------|
| `Si446x_writeGPIO(pin, value)` | 设置单个 GPIO 模式 |
| `Si446x_readGPIO()` | 读取 GPIO 电平状态 |
| `Si446x_dump(buff, group)` | 读取某组所有属性寄存器 |
| `Si446x_irq_off()` / `Si446x_irq_on()` | 手动开关中断 |

---

## 5. WDS 配置指南

### 5.0 Si4463 硬件版本 (B1B / C2A) 与 radio_config

Si4463 存在两个主要硬件版本，射频参数（校准、patch 等）不同，**必须使用对应版本的 WDS 配置**：

| 版本 | PART_INFO romId | WDS 配置来源 | 备注 |
|------|----------------|--------------|------|
| B1 (B1B) | 3 | `radio_config/Si4463b1/` | 当前工程硬件 |
| C2 (C2A) | 6 | `radio_config/Si4463c2/` | 需加载 `si4463_c2_patch.h` |

- `radio_config/` 目录是 WDS 生成的**原始配置库**，按硬件版本分子目录存放，不要手动修改。
- `radio_config.h` 是从 `radio_config/Si4463b1/` 复制过来的**当前使用**配置
  （`radio_config_Si4463b1_pocsag_153.00_1200_2fsk_4.5.h`，153.00 MHz / 1200 bps / 2FSK /
  Direct TX，WDS 3.2.11 生成），射频寄存器部分**不要手改**。
- 更换 C2 芯片时：从 `radio_config/Si4463c2/` 选对应配置**整体替换** `radio_config.h`
  （保持文件名不变），并按 WDS 要求加载 C2 patch。
- 换配置后**检查两处用户参数**（文件顶部 `USER DEFINED PARAMETERS` 段）：
  1. `RADIO_CONFIGURATION_DATA_RSYMB_SPS` — 必须等于新配置的 WDS `Rsymb`
     （512/1200/1600 sps）。驱动用它推导 `Si446x_txDirectWrite()` 的位敲击码率；
     缺失时回退 1200 sps 并给编译警告。
  2. `RADIO_CONFIGURATION_DATA_RADIO_XO_FREQ` — WDS 自带（30 MHz），
     `Si446x_setFrequencyKHz()` 运行时读取，换晶振只需改这里。
- 芯片版本可通过 `Si446x_getInfo()` 的 `romId` 字段确认（3 = B1B，6 = C2A）。

### 5.1 使用 Silicon Labs WDS 生成配置

1. 打开 **WDS (Wireless Development Suite)**
2. 选择设备: **Si4463 Rev B1**（C2 芯片选 Rev C2）
3. 配置参数（参考下面的示例）
4. 点击 **Generate** → 复制输出的 `radio_config.h`

### 5.2 Direct TX 模式的 WDS 关键设置

| 参数 | 值 | 说明 |
|------|-----|------|
| Modulation Type | 2 = 2FSK | 也可选 4FSK/GMSK/OOK |
| Rsymb | 1200 sps | 符号率 = 1200 bps（2FSK 时） |
| Fdev | 4500 Hz | 频偏（影响接收带宽） |
| RX BW | 150 kHz | 接收滤波器带宽 |
| Crystal Freq | 30 MHz | XO 频率 |
| RF Freq | 156 MHz | 默认频率（运行时可通过 API 修改） |
| **D_source** | **1 = Direct Sync** | ⭐ 启用 Direct TX 模式 |
| **TX Data Source** | **GPIO0** | ⭐ 数据从 GPIO0 引脚输入 |
| Hi_pfm_div | 1 | 高速 PFM 分频 |

> **关键**: `D_source = 1` 使得调制数据来自 GPIO0 而不是 SPI FIFO。  
> 如果使用 `D_source = 0`（FIFO 模式），则需用 `Si446x_TX()` 发送数据。

### 5.3 GPIO 配置页 - ⚠️ 注意事项

WDS **默认**会在所有 GPIO 上启用内部上拉 (`0x40`)，这在 ESP32 上会导致 **初始化卡死**：

```
// ❌ WDS 默认 — SDO 有上拉，MISO 读到 0xFF 伪装 CTS 就绪
0x13, 0x44, 0x50, 0x60, 0x61, 0x40, 0x40, 0x00

// ✅ 修复 — 去掉所有上拉位
0x13, 0x04, 0x10, 0x20, 0x21, 0x00, 0x00, 0x00
```

**必须**: 在 WDS GPIO 配置页取消勾选所有引脚的 **Pull-up** 选项。

### 5.4 推荐的 WDS 参数配置

```
Freq:       156 MHz (可在代码中更改)
Modulation: 2FSK
Rsymb:      1200 sps
Fdev:       4.5 kHz
RXBW:       150 kHz
D_source:   1 (Direct Sync / GPIO0)
Hi_pfm_div: 1
PA:         0x08 (Class E), GAIN = 127
```

### 5.5 WDS 配置再生成

如果你需要修改配置（数据速率、频偏、调制方式等）：

1. 在 WDS 中修改参数并重新生成
2. 替换 `lib/si4463/radio_config.h`
3. **必须同时修改** 以下两处：
   - `RF_GPIO_PIN_CFG` — 去掉所有 `0x40` 上拉位
   - `RF_FRR_CTL_A_MODE_4` — 将 FRR_B_MODE 改为 `0x00`（晶振状态）：
     ```
     // 改前: 0x11, 0x02, 0x04, 0x00, 0x01, 0x07, 0x05, 0x09
     //                                    ^^^^^^^^ FRR_B = GPIO_STATE
     // 改后: 0x11, 0x02, 0x04, 0x00, 0x01, 0x00, 0x05, 0x09
     //                                    ^^^^^^^^ FRR_B = CHIP_STATE
     ```
4. 如果修改了数据速率，更新 `Si446x_txDirectWrite()` 中的 `BIT_PERIOD_US`：
   - `BIT_PERIOD_US = 1_000_000 / 数据速率(bps)`
5. 编译测试

---

## 6. 频率计算

### 6.1 原理

Si4463 使用 Fractional-N PLL 合成载波频率：

```
fRF = fXO / OUTDIV × (FC_INTE + 1 + FC_FRAC / 2^19)
```

其中：
- `fXO` = 晶振频率（`Si446x_setFrequencyKHz()` 运行时读取
  `RADIO_CONFIGURATION_DATA_RADIO_XO_FREQ`，当前 **30 MHz**；换晶振改
  `radio_config.h` 即可，无需改代码）
- `OUTDIV` = 输出分频比（根据频段自动选择 2/3/4/6/8/12）
- `FC_INTE` = 整数部分（8 位寄存器 FREQ_CONTROL_INTE）
- `FC_FRAC` = 小数部分（20 位寄存器，**bit 19 恒为 1**）

### 6.2 频段与分频比

| 频率范围 (MHz) | OUTDIV | CLKGEN_BAND |
|----------------|--------|-------------|
| 119 – 220      | 12     | 0x0D        |
| 220 – 295      | 8      | 0x0C        |
| 295 – 390      | 6      | 0x0B        |
| 390 – 520      | 4      | 0x0A        |
| 520 – 700      | 3      | 0x09        |
| 700 – 1050     | 2      | 0x08        |

### 6.3 手动计算示例

以 152.675 MHz 为例（OUTDIV=12, fXO=30 MHz）：

```
N      = 152.675 × 12 / 30 = 61.07
整数部分 = floor(61.07) = 61
FC_INTE = 61 - 1 = 60 = 0x3C

小数部分 = N - floor(N) = 61.07 - 61 = 0.07
FC_FRAC + 2^19  = (0.07 + 1) × 2^19 
                 = 1.07 × 524288 
                 = 560988 ≈ 0x88F5C

实际写入: FC_INTE = 0x3C, FC_FRAC = 0x88F5C
```

> ⚠️ **关键细节**: FC_FRAC 的 bit 19（最高位）**必须始终为 1**。  
> 有效分数部分 = FC_FRAC & 0x7FFFF（去除 bit 19 后）。

### 6.4 库函数实现

```c
// 设置频率（kHz 输入）
Si446x_setFrequencyKHz(152675);  // 152.675 MHz

// 读取当前频率回读（从芯片寄存器计算）
uint32_t fKHz = Si446x_getFrequencyKHz();
Serial.printf("Freq: %u.%03u MHz\n", fKHz / 1000, fKHz % 1000);
```

### 6.5 精度说明

库内部使用 **64 位整数运算**实现频率计算，无浮点误差。  
实测精度：±500 Hz 以内（取决于晶振精度和温度）。

### 6.6 频率切换流程

调用 `Si446x_setFrequencyKHz()` 后会自动执行：

1. 查找频段表选择 OUTDIV & CLKGEN_BAND
2. 写入 MODEM_CLKGEN_BAND
3. 写入 CHANNEL_STEP_SIZE
4. 写入 FREQ_CONTROL_INTE 和 FREQ_CONTROL_FRAC
5. **触发 IRCAL**（图像抑制校准）— 约需 10–20 ms

> 调用时建议芯片处于 READY 状态（不要在 TX/RX 中切换频率）。

---

## 7. Direct TX 模式

### 7.1 原理

Direct TX 模式下，数据不经过 SPI FIFO，而是直接从 **GPIO0** 引脚串行输出。Si4463 内部 PLL 以配置的数据速率产生位时钟，GPIO0 上的电平被直接调制到载波上。

```
MCU (GPIO16) ──→ Si4463 GPIO0 ──→ (FSK 调制) ──→ PA ──→ 天线
```

### 7.2 时序

数据以 **UART 式异步串行**方式输出：

```
1 字节 = 1 起始位(L) + 8 数据位(LSB first) + 1 停止位(H)
        ┌──┐  ┌──┐  ┌──┐  ┌──┐  ┌──┐  ┌──┐  ┌──┐  ┌──┐  ┌──┐  ┌──┐
─ ─ ─ ─ ┤L0├──┤L1├──┤L2├──┤L3├──┤L4├──┤L5├──┤L6├──┤L7├──┤H ├─── ─ ─ ─
        └──┘  └──┘  └──┘  └──┘  └──┘  └──┘  └──┘  └──┘  └──┘  └──┘
       ^起始      ^ bit 0 LSB ...        ... bit 7 MSB     ^停止
```

**时序参数**: `BIT_PERIOD_US = 1_000_000 / dataRate(bps)`

- 1.2 kbps → 833 µs/bit → 1 字节约 8.33 ms
- 10 kbps → 100 µs/bit → 1 字节约 1 ms
- 100 kbps → 10 µs/bit → 1 字节约 0.1 ms

### 7.3 使用步骤

```c
// 1. 确保 WDS 配置了 D_source=1（Direct Sync 模式）

// 2. 进入 TX 状态
Si446x_startDirectTX(0);   // channel 0
delay(10);                // 等待 PLL 锁定（10ms 足够）

// 3. 发送数据（阻塞）
Si446x_txDirectWrite(data, len);

// 4. 停止发射
Si446x_stopTX();
```

### 7.4 接收端

接收端需配置与发送端完全一致的参数：
- 调制方式：2FSK
- 数据速率：1200 bps
- 频偏：4500 Hz
- RX 带宽：≥ 150 kHz

可以用 **SDR（RTL-SDR / HackRF / LimeSDR）** 配合解码软件查看信号：
- `gqrx` / `sdrangel` / `hackrf` 等
- FSK 解码时需匹配波特率、频偏和中心频率

### 7.5 发送 Beacon 示例

参见 `src/main.cpp` — 每 5s 发送一次包含递增序列号和校验的数据包。

---

## 8. RF 天线开关

### 8.1 单天线半双工设计

Si4463 GPIO 可输出 TX/RX 状态信号，控制 RF 开关芯片（如 **PE4259** / **SKY13350** / **UPG2179**）：

```
Si4463 GPIO2 (TX_STATE) ─── RF 开关 CTL_A
Si4463 GPIO3 (RX_STATE) ─── RF 开关 CTL_B

| TX_STATE | RX_STATE | 状态 |
|----------|----------|------|
| 1        | 0        | TX → 天线连接 PA 输出 |
| 0        | 1        | RX → 天线连接 LNA 输入 |
| 0        | 0        | 空闲 → 天线悬空（或默认 RX） |
```

> **🚨 安全隐患**: 若同时为 HIGH (1+1)，PA 输出直连 LNA 输入，可能烧毁 LNA。  
> Si4463 保证 TX_STATE 和 RX_STATE **不会同时为 HIGH**。

### 8.2 软件配置

```c
// 方式 1: 调用便捷函数（推荐）
Si446x_configRFswitch();

// 方式 2: 手动调用（等效）
Si446x_configGPIO(
    0x00, 0x00,                        // GPIO0, GPIO1 = 无操作
    SI446X_GPIO_MODE_TX_STATE | SI446X_GPIO_PULL_EN,  // GPIO2
    SI446X_GPIO_MODE_RX_STATE | SI446X_GPIO_PULL_EN,  // GPIO3
    SI446X_NIRQ_MODE_DONOTHING,
    SI446X_SDO_MODE_DONOTHING,
    0x00
);
```

> **注意**: 如果 WDS 配置中已包含正确的 GPIO_PIN_CFG，则不需要调用 `Si446x_configRFswitch()`。它只是一个覆盖方法，用于 WDS 配置未正确设置 GPIO 的场景。

---

## 9. FIFO 收发模式

当 WDS 配置中的 `D_source = 0` 时，数据通过 SPI FIFO 传输。

### 9.1 发送

```c
uint8_t data[] = "Hello Si4463!";
Si446x_TX(data, sizeof(data), 0, SI446X_STATE_READY);
```

参数说明：
- `channel`: 频率信道（基于 CHANNEL_STEP_SIZE 偏移）
- `onTxFinish`: TX 完成后进入的状态（如 READY / SLEEP / RX）

### 9.2 接收

```c
// 启动接收
Si446x_RX(0);

// 在回调中读取数据（需定义弱符号回调）
void SI446X_CB_RXCOMPLETE(uint8_t length, int16_t rssi)
{
    uint8_t buf[64];
    Si446x_read(buf, length);
    Serial.printf("RX %d B, RSSI %d dBm\n", length, rssi);
}

void SI446X_CB_RXINVALID(int16_t rssi)
{
    Serial.printf("CRC err, RSSI %d dBm\n", rssi);
}

void SI446X_CB_RXBEGIN(int16_t rssi)
{
    Serial.printf("Sync detect, RSSI %d dBm\n", rssi);
}
```

### 9.3 回调列表

| 回调函数 | 触发条件 |
|----------|----------|
| `SI446X_CB_RXBEGIN(rssi)` | 检测到同步字 |
| `SI446X_CB_RXCOMPLETE(len, rssi)` | 成功接收完整数据包 |
| `SI446X_CB_RXINVALID(rssi)` | CRC 校验失败 |
| `SI446X_CB_SENT()` | 数据包发送完成 |
| `SI446X_CB_WUT()` | 唤醒定时器触发 |
| `SI446X_CB_LOWBATT()` | 低电压检测触发 |
| `SI446X_CB_CMDTIMEOUT()` | SPI 命令超时 |

> 回调是 **弱符号 (weak)** 定义，只需在你的代码中定义同名函数即可覆盖默认空实现。

---

## 10. 状态与寄存器读取

### 10.1 芯片状态

```c
si446x_state_t st = Si446x_getState();

switch(st) {
    case SI446X_STATE_NOCHANGE:  // = 0x00, FRR_B 返回 SLEEP 时为 0
    case SI446X_STATE_SLEEP:     // = 0x01
        Serial.println("SLEEP");
        break;
    case SI446X_STATE_SPI_ACTIVE: // = 0x02
        Serial.println("SPI_ACTIVE");
        break;
    case SI446X_STATE_READY:     // = 0x03
    case SI446X_STATE_READY2:    // = 0x04 → 被映射为 READY
        Serial.println("READY");
        break;
    case SI446X_STATE_TX:        // = 0x07
    case SI446X_STATE_TX_TUNE:   // = 0x05 → 被映射为 TX
        Serial.println("TX");
        break;
    case SI446X_STATE_RX:        // = 0x08
    case SI446X_STATE_RX_TUNE:   // = 0x06 → 被映射为 RX
        Serial.println("RX");
        break;
}
```

> **重要**: FRR_B 寄存器返回的 SLEEP 值是 `0x00`（不是 `0x01`），因为 Si4463 在 SLEEP 模式下 FRR 返回的是 NOCHANGE(0)。库中 `getState()` 正确处理了此映射。

### 10.2 Fast Response Registers (FRR)

FRR 是 Si4463 的快速状态寄存器，**无需发送 SPI 命令**即可读取，只需一次 SPI 读操作：

| 寄存器 | 命令字节 | 默认内容 | 可配置为 |
|--------|----------|----------|---------|
| FRR_A  | 0x50     | RSSI     | 晶振状态/GPIO 状态/… |
| FRR_B  | 0x51     | 芯片状态 | 晶振状态/GPIO 状态/… |
| FRR_C  | 0x53     | INT_PEND | 晶振状态/GPIO 状态/… |
| FRR_D  | 0x57     | INT_PEND | 晶振状态/GPIO 状态/… |

通过 `FRR_CTL_A_MODE` ~ `FRR_CTL_D_MODE` 属性寄存器配置每个 FRR 的内容。

默认配置（本库）：
- FRR_A = RSSI (0x01)
- FRR_B = CHIP_STATE (0x00)
- FRR_C = 0x05
- FRR_D = 0x09

### 10.3 寄存器转储

```c
// 读取某组所有属性
uint8_t buf[64];
uint8_t len = Si446x_dump(buf, SI446X_PROP_GROUP_FREQ_CONTROL);
// buf[0..7] 包含 PROP_GROUP_FREQ_CONTROL 所有寄存器值
// len 返回该组大小
```

### 10.4 芯片信息

```c
si446x_info_t info;
Si446x_getInfo(&info);

Serial.printf("Part: 0x%04X\n", info.part);   // Si4463 = 0x4463
Serial.printf("ROM:  %d\n", info.romId);       // 3=B1B, 6=C2A
Serial.printf("Firmware ext:%d branch:%d\n",
    info.revExternal, info.revBranch);
```

---

## 11. 常见问题与排错

### 11.1 `Si446x_init()` 卡死无输出

**原因**: WDS 配置中 SDO 引脚的内部上拉 (`0x40`) 导致 MISO 在芯片准备好前读到 0xFF，被误认为 CTS 就绪。

**解决**:
1. 优先在 WDS 中取消 GPIO Pull-up 重新生成配置；若手改，在 `radio_config.h` 中找到 `RF_GPIO_PIN_CFG` 行
2. 将第 6、7 字节的 `0x40` 改为 `0x00`
3. 或者直接在 WDS 重新生成时取消 GPIO Pull-up 选项（推荐，保持 radio_config.h 为纯生成物）

```
// ❌ 改前
0x13, 0x44, 0x50, 0x60, 0x61, 0x40, 0x40, 0x00
                                  ^^^^  ^^^^
// ✅ 改后
0x13, 0x04, 0x10, 0x20, 0x21, 0x00, 0x00, 0x00
```

### 11.2 ESP32 中断崩溃 (Guru Meditation / ISR WDT)

**原因**: AVR 库在 ISR 中调用了 `detachInterrupt()` 和 `attachInterrupt()`，ESP32 架构不允许在 ISR 中操作中断注册。

**解决**: 将 IRQ 传 -1, 使用轮询模式：

```c
// setPins 第 3 个参数传 -1 即可禁用中断
Si446x_setPins(10, 5, -1, 16, 12, 11, 13);

// loop() 中周期调用
void loop()
{
    Si446x_SERVICE();  // 轮询事件
    // ...其他代码...
}
```

### 11.3 频率不准（SDR 显示偏差）

**场景**: 设置 152.675 MHz，SDR 在 152.5875 MHz 最强。

**原因**: `setFrequencyKHz()` 中 FC_FRAC 计算错误 — 分数右移了 1 位。

```c
// ❌ 错误 — 右移导致分数贡献减半
const uint32_t frac = (N_frac >> 1) + (1 << 19);

// ✅ 正确
const uint32_t frac = N_frac | (1 << 19);
```

### 11.4 状态显示 "?"

**原因**: FRR_B 返回 0x00 (SLEEP)，但枚举 `SI446X_STATE_SLEEP = 0x01`。`SI446X_STATE_NOCHANGE = 0x00` 未被 stateStr 涵盖。

**解决**: 状态显示函数增加 `case SI446X_STATE_NOCHANGE: return "SLEEP";`

### 11.5 PA 功率参数参考

| PA_PWR_LVL 值 | 典型输出功率 |
|---------------|------------|
| 0x00–0x07     | ~0 dBm (1 mW) |
| 0x08–0x16     | ~+10 dBm (10 mW) |
| 0x17–0x28     | ~+15 dBm (30 mW) |
| 0x29–0x7F     | ~+20 dBm (100 mW) |

> 实际功率取决于 PCB 设计、匹配网络和供电电压。上述仅供参考。

### 11.6 NIRQ 浮空问题

**场景**: 不使用 nIRQ（IRQ = -1），但引脚浮空可能导致不稳定。

**解决**: 在 WDS GPIO 配置中将 NIRQ 模式设为 `0x00`（DONOTHING），或者在硬件上拉一个 10kΩ 电阻到 VCC。

### 11.7 FRR_B 返回 GPIO_STATE 

**场景**: Si446x_getState() 返回错误状态。

**原因**: WDS 可能将 FRR_B 配置为 `0x07`（GPIO_STATE）而不是 `0x00`（CHIP_STATE）。

**解决**: 检查 `RF_FRR_CTL_A_MODE_4` 定义，将 FRR_B_MODE 字节设为 `0x00`：

```
// RF_FRR_CTL_A_MODE_4: FRR_A=0x01, FRR_B=0x00, FRR_C=0x05, FRR_D=0x09
0x11, 0x02, 0x04, 0x00, 0x01, 0x00, 0x05, 0x09
```

---

## 12. 附录

### 12.1 项目文件结构

```
si4463-demo/
├── platformio.ini                 # PIO 配置
├── Si4463_Manual.md               # 本文档
├── src/
│   └── main.cpp                   # 主程序（Direct TX Beacon）
├── lib/
│   └── Si446x/
│       ├── Si446x.h               # 公共头文件（API + SPI 行为配置 + 命令/属性定义，
│       │                          #   原 Si446x_config.h / Si446x_defs.h 已合并于此;
│       │                          #   引脚由应用方 setPins 传入, 见 §2.3）
│       ├── Si446x.cpp             # 库实现
│       ├── radio_config.h         # WDS 生成的配置数据（B1，勿手改）
│       └── radio_config/          # WDS 原始配置库（B1B/C2A 按硬件版本分目录）
└── wds/
    └── si4463_revb1_direct_30m_tx.xml  # WDS 项目文件
```

### 12.2 相关命令速查

| Si446x 命令 (十六进制) | 功能 |
|------------------------|------|
| 0x00 (NOP) | 空操作、CTS 轮询 |
| 0x02 (POWER_UP) | 上电 |
| 0x11 (SET_PROPERTY) | 写属性寄存器 |
| 0x12 (GET_PROPERTY) | 读属性寄存器 |
| 0x13 (GPIO_PIN_CFG) | GPIO 配置 |
| 0x17 (IRCAL) | 图像抑制校准 |
| 0x31 (START_TX) | 开始发射 |
| 0x32 (START_RX) | 开始接收 |
| 0x34 (CHANGE_STATE) | 状态切换 |
| 0x44 (READ_CMD_BUFF) | 读取命令缓冲区（CTS） |
| 0x50–0x57 (FRR A–D) | 快速读寄存器 |

### 12.3 关键寄存器地址

| 属性寄存器 | 地址 | 大小 | 功能 |
|-----------|------|------|------|
| GLOBAL_XO_TUNE | `0x0000` | 1 | 晶振匹配电容调谐 |
| GLOBAL_CONFIG | `0x0003` | 1 | 高频/低频模式 |
| INT_CTL_ENABLE | `0x0100` | 1 | 中断组全局使能 |
| FRR_CTL_A_MODE | `0x0200` | 1 | FRR_A 内容选择 |
| FRR_CTL_B_MODE | `0x0201` | 1 | FRR_B 内容选择 |
| MODEM_MOD_TYPE | `0x2000` | 1 | 调制类型 + 数据源 |
| MODEM_DATA_RATE | `0x2003` | 3 | 数据速率 (24-bit) |
| MODEM_FREQ_DEV | `0x200A` | 2 | 频偏 (17-bit) |
| MODEM_CLKGEN_BAND | `0x2051` | 1 | PLL 输出分频比 |
| PA_MODE | `0x2200` | 1 | PA 模式 (Class E/B) |
| PA_PWR_LVL | `0x2201` | 1 | PA 输出功率级 |
| FREQ_CONTROL_INTE | `0x4000` | 1 | PLL 整数分频 |
| FREQ_CONTROL_FRAC | `0x4001` | 3 | PLL 小数分频 (20-bit) |
| FREQ_CONTROL_CHANNEL_STEP_SIZE | `0x4004` | 2 | 信道步长 |

### 12.4 参考资料

- **Si4463/62/61/60 数据手册 (AN633)**: Silicon Labs 官网
- **WDS (Wireless Development Suite)**: Silicon Labs 官方配置工具
- **Zak Kemble 原版库**: http://blog.zakkemble.co.uk/si4463-radio-library-avr-arduino/
- **Si4463 寄存器说明**: AN685 — Si446x EZRadioPRO Register Description
- **频率计算**: AN634 — EZRadioPRO Frequency Programming Guide
