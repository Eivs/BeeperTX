# 硬件连接指南

> BeeperTX 项目中 ESP32-S3 与 Si4463 射频模块的完整引脚连接说明。

---

## 1. ESP32-S3 ↔ Si4463 信号连接

### 1.1 SPI 总线

| ESP32-S3 GPIO | 方向 | Si4463 引脚 | 功能 |
|---------------|------|-------------|------|
| GPIO11 | → | SDI (MOSI) | SPI 数据输出 |
| GPIO12 | → | SCLK | SPI 时钟 |
| GPIO13 | ← | SDO (MISO) | SPI 数据输入 |
| GPIO10 | → | nSEL | SPI 片选 (CS, 低电平有效) |

### 1.2 控制信号

| ESP32-S3 GPIO | 方向 | Si4463 引脚 | 功能 |
|---------------|------|-------------|------|
| GPIO5 | → | SDN | 模块关断 (高电平复位) |
| GPIO4 | ← | IRQ | 中断请求 (轮询模式可悬空) |

### 1.3 发射数据

| ESP32-S3 GPIO | 方向 | Si4463 引脚 | 功能 |
|---------------|------|-------------|------|
| GPIO16 | → | GPIO0 | TX 基带数据 (Direct Mode bit-banging) |

### 1.4 RF 天线开关 (可选)

仅使用单天线半双工设计（RF 开关切换收发）时需要。由 **Si4463 芯片的 GPIO2/GPIO3**
状态输出直接驱动外部 RF 开关（如 UPG2179 或 SP4T），**不占用 ESP32 引脚**：

| Si4463 引脚 | 模式 | TX 时 | RX 时 |
|-------------|------|-------|-------|
| GPIO2 | TX_STATE (0x21) | HIGH | LOW |
| GPIO3 | RX_STATE (0x20) | LOW | HIGH |

> 固件通过 SPI `GPIO_PIN_CFG` 命令配置（`Si446x_configRFswitch()`，见
> `src/main.cpp`）。本项目为直连天线，未使用 RF 开关，GPIO1/2/3 悬空。

### 1.5 电源

| ESP32-S3 | 方向 | Si4463 引脚 | 功能 |
|----------|------|-------------|------|
| 3V3 | → | VDD | 电源 (3.3V) |
| GND | → | GND | 共地 |

---

## 2. 接口总览

```
┌─────────────┐  SPI                     ┌─────────────┐
│             │←───── GPIO11 (MOSI) ─────→│             │
│             │←───── GPIO12 (SCLK) ─────→│             │
│  ESP32-S3   │────── GPIO13 (MISO) ─────→│   Si4463    │
│             │←───── GPIO10 (nSEL) ─────→│             │
│             │                            │             │
│             │────── GPIO5  (SDN) ───────→│             │
│             │←────── GPIO4  (IRQ) ──────→│  (可选)     │
│             │                            │             │
│             │────── GPIO16 (GPIO0) ─────→│  (TX Data)  │
│             │←────── 3V3 (VDD) ─────────→│             │
│             │←────── GND ───────────────→│             │
│             │                            └─────────────┘
│             │
│  [RF 开关] ←──── Si4463 GPIO2/GPIO3 芯片输出 (未使用则悬空)
└─────────────┘
```

---

## 3. Si4463 模块引脚排列

模块正面朝上，从左到右排列：

| 左侧 (引脚 1-6) | 右侧 (引脚 7-12) |
|-----------------|------------------|
| VDD | GND |
| GPIO0 | SDN |
| GPIO1 | GPIO3 |
| IRQ | GPIO2 |
| SCK | NSEL |
| MISO | MOSI |

**典型接线示例:**
- Si4463 GPIO0 → ESP32 GPIO16 (TX 数据)
- Si4463 IRQ → ESP32 GPIO4 (中断)
- Si4463 SDN → ESP32 GPIO5 (关断)

---

## 4. ESP32-S3 开发板引脚排列

以 合宙 ESP32S3-CORE 开发板为例：

| 左侧排针 | 右侧排针 |
|----------|----------|
| GND | GND |
| GPIO1 | 3.3V |
| GPIO2 | GPIO18 |
| GPIO3 | GPIO17 |
| GPIO4 | GPIO16 |
| GPIO5 | GPIO15 |
| GND | GPIO14 |
| GPIO6 | GPIO13 |
| GPIO7 | GND |
| GPIO8 | 3.3V |
| GPIO9 | GPIO12 |
| REST | GPIO11 |
| 3.3V | GPIO10 |
| GND | GPIO0 |
| PWB | 5V |
| 5V | GND |

---

## 5. 关键项目配置对照

| 配置项 | 值 | 对应硬件 |
|--------|-----|---------|
| TX 数据引脚 | GPIO16 | Si4463 GPIO0 |
| SPI CS | GPIO10 | Si4463 nSEL |
| SPI MOSI | GPIO11 | Si4463 SDI |
| SPI SCK | GPIO12 | Si4463 SCLK |
| SPI MISO | GPIO13 | Si4463 SDO |
| 关断引脚 | GPIO5 | Si4463 SDN |
| 中断引脚 | 未使用 (`PIN_NIRQ=-1`) | 轮询模式, GPIO4 可不接 |
| RF 开关 | 未使用 | Si4463 GPIO2/GPIO3 芯片输出 (若启用) |


> 引脚默认值定义在 `src/main.cpp` 顶部（`PIN_CS`/`PIN_SDN`/`PIN_TX_DATA`/...），
> 在 `Si446x_init()` 之前通过 `Si446x_setPins()` 传入驱动。

---

## 6. 注意事项

- **电平匹配**: ESP32-S3 与 Si4463 均为 3.3V 逻辑，可直接连接，无需电平转换。
- **SDN 引脚**: 拉高时 Si4463 进入硬件复位/低功耗状态。上电后需拉低才能正常通信。
- **GPIO0 直连**: Si4463 GPIO0 配置为异步直接 TX 模式输出，由 MCU 通过 GPIO16 驱动数据。
- **天线开关**: 若使用 RF 开关, Si4463 GPIO2 (TX_STATE) 与 GPIO3 (RX_STATE) **互斥**——发射和接收不能同时使能，防止 PA 损坏。本项目未使用。
- **IRQ 轮询**: GPIO4 可悬空。固件已硬编码 `PIN_NIRQ = -1`（见 `src/main.cpp`），驱动使用轮询模式代替中断（ESP32 ISR 中不能调用 AVR 库的 attach/detachInterrupt）。
