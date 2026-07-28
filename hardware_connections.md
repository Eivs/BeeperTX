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

### 1.4 RF 天线开关

| ESP32-S3 GPIO | 方向 | 功能 |
|---------------|------|------|
| GPIO2 | → | TX 使能 (发射模式时拉高) |
| GPIO3 | → | RX 使能 (接收模式时拉高) |

> **注意:** GPIO2/GPIO3 不直接连接 Si4463，而是控制外部 RF 开关 (如 UPG2179 或 SP4T)，实现天线在 TX 与 RX 路径之间的切换。

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
│             │←────── GPIO4  (IRQ) ──────→│             │
│             │                            │             │
│             │────── GPIO16 (GPIO0) ─────→│  (TX Data)  │
│             │                            └─────────────┘
│             │
│  [TX ANT] ←──── GPIO2 (TX_EN)
│  [RX ANT] ←──── GPIO3 (RX_EN)
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
| TX 使能 | GPIO2 | RF 开关 TX 端 |
| RX 使能 | GPIO3 | RF 开关 RX 端 |

> 引脚默认值定义在 `lib/si4463/Si446x_config.h`，可在运行时通过 `Si446x_setPins()` 覆盖。

---

## 6. 注意事项

- **电平匹配**: ESP32-S3 与 Si4463 均为 3.3V 逻辑，可直接连接，无需电平转换。
- **SDN 引脚**: 拉高时 Si4463 进入硬件复位/低功耗状态。上电后需拉低才能正常通信。
- **GPIO0 直连**: Si4463 GPIO0 配置为异步直接 TX 模式输出，由 MCU 通过 GPIO16 驱动数据。
- **天线开关**: GPIO2 与 GPIO3 **互斥**——发射和接收不能同时使能，防止 PA 损坏。
- **IRQ 轮询**: 若 GPIO4 未连接，可将 `Si446x_config.h` 中的 `IRQ` 设置为 `-1`，驱动将使用轮询模式代替中断。
