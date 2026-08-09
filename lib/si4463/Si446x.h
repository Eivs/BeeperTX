/*
 * Project: Si4463 Radio Library for ESP32
 * Original Author: Zak Kemble, contact@zakkemble.co.uk (AVR/Arduino version)
 * Ported for ESP32 + RF switch control by ASHINING RF design adaptation
 * Copyright: (C) 2017 by Zak Kemble
 * License: GNU GPL v3 (see License.txt)
 * Web: http://blog.zakkemble.co.uk/si4463-radio-library-avr-arduino/
 *
 * ============================================================
 * 本文件为合并后的唯一公共头文件 (原 Si446x.h + Si446x_config.h +
 * Si446x_defs.h), 内容分节:
 *   §1 编译期配置  — SPI 行为 / 空闲模式 (原 Si446x_config.h; 引脚部分已
 *                     移除, 由应用方 setPins 传入)
 *   §2 命令码      — SPI 命令字节 (原 Si446x_defs.h)
 *   §3 属性地址    — 属性组 + 属性宏 (原 Si446x.h / Si446x_defs.h)
 *   §4 枚举与结构  — GPIO/NIRQ/SDO 模式, 状态机, ADC, PART_INFO
 *   §5 其他宏      — 包长 / 功率 / WUT / FIFO / 中断 pending 位
 *   §6 回调弱符号
 *   §7 公共 API
 *
 * Si4463 硬件版本 (详见 README.md "Si4463 硬件版本" 一节):
 *   - B1  (ROM 3, B1B): 使用 radio_config/Si4463b1/* 的 WDS 配置
 *   - C2  (ROM 6, C2A): 使用 radio_config/Si4463c2/* 的 WDS 配置
 *   当前工程硬件为 B1。radio_config.h 即复制自
 *   radio_config/Si4463b1/radio_config_Si4463b1_pocsag_153.00_1200_2fsk_4.5.h
 *   (WDS 生成物, 勿手改; 换硬件版本时整体替换 radio_config.h)。
 * ============================================================
 */

#ifndef SI446X_H_
#define SI446X_H_

#include <Arduino.h>
#include <stdint.h>

/* =================================================================
 * §1 编译期配置 — SPI 行为 (引脚由应用在运行时指定, 见 Si446x_setPins)
 * =================================================================
 */

// 若其他库在中断内访问 SPI, 置 1
#define SI446X_INT_SPI_COMMS 0

// ADC 转换速度 (10-12 推荐)
#define SI446X_ADC_SPEED 10

// 空闲模式 (READY = 快速响应, 1.8mA; SPI_ACTIVE = 低功耗, 1.35mA)
// 注意: 该宏文本引用 §4 的 SI446X_STATE_READY 枚举, 在 .cpp 使用时展开
#define SI446X_IDLE_MODE SI446X_STATE_READY

// 包长模式: 0 = 变长, >0 = 定长
#define SI446X_FIXED_LENGTH 0

// 引脚不在本头定义 — 板级接线由应用方负责:
// 调用 Si446x_setPins() 传入 (main.cpp 中 PIN_CS/PIN_SDN/... 宏), 传 -1
// 表示保持当前值。接线参考表见 README.md §2 与 main.cpp 顶部宏。

/* =================================================================
 * §2 命令码 — SPI 命令字节 (Si446x 数据手册 Command Summary)
 * =================================================================
 */

#define SI446X_CMD_POWER_UP 0x02
#define SI446X_CMD_NOP 0x00
#define SI446X_CMD_PART_INFO 0x01
#define SI446X_CMD_FUNC_INFO 0x10
#define SI446X_CMD_SET_PROPERTY 0x11
#define SI446X_CMD_GET_PROPERTY 0x12
#define SI446X_CMD_GPIO_PIN_CFG 0x13
#define SI446X_CMD_FIFO_INFO 0x15
#define SI446X_CMD_GET_INT_STATUS 0x20
#define SI446X_CMD_REQUEST_DEVICE_STATE 0x33
#define SI446X_CMD_CHANGE_STATE 0x34
#define SI446X_CMD_READ_CMD_BUFF 0x44
#define SI446X_CMD_READ_FRR_A 0x50
#define SI446X_CMD_READ_FRR_B 0x51
#define SI446X_CMD_READ_FRR_C 0x53
#define SI446X_CMD_READ_FRR_D 0x57
#define SI446X_CMD_IRCAL 0x17
#define SI446X_CMD_IRCAL_MANUAL 0x1a
#define SI446X_CMD_START_TX 0x31
#define SI446X_CMD_TX_HOP 0x37
#define SI446X_CMD_WRITE_TX_FIFO 0x66
#define SI446X_CMD_PACKET_INFO 0x16
#define SI446X_CMD_GET_MODEM_STATUS 0x22
#define SI446X_CMD_START_RX 0x32
#define SI446X_CMD_RX_HOP 0x36
#define SI446X_CMD_READ_RX_FIFO 0x77
#define SI446X_CMD_GET_ADC_READING 0x14
#define SI446X_CMD_GET_PH_STATUS 0x21
#define SI446X_CMD_GET_CHIP_STATUS 0x23

/* =================================================================
 * §3 属性地址 — 属性组 (高字节) + 属性宏
 * =================================================================
 */

#define SI446X_PROP_GROUP_GLOBAL 0x00
#define SI446X_PROP_GROUP_INT 0x01
#define SI446X_PROP_GROUP_FRR 0x02
#define SI446X_PROP_GROUP_PREAMBLE 0x10
#define SI446X_PROP_GROUP_SYNC 0x11
#define SI446X_PROP_GROUP_PKT 0x12
#define SI446X_PROP_GROUP_MODEM 0x20
#define SI446X_PROP_GROUP_MODEM_CHFLT 0x21
#define SI446X_PROP_GROUP_PA 0x22
#define SI446X_PROP_GROUP_SYNTH 0x23
#define SI446X_PROP_GROUP_MATCH 0x30
#define SI446X_PROP_GROUP_FREQ_CONTROL 0x40
#define SI446X_PROP_GROUP_RX_HOP 0x50
#define SI446X_PROP_GROUP_PTI 0xF0

// 属性地址拼装: (组 << 8) | 组内 ID
#define GLOBAL_PROP(prop) ((SI446X_PROP_GROUP_GLOBAL << 8) | prop)
#define INT_PROP(prop) ((SI446X_PROP_GROUP_INT << 8) | prop)
#define PKT_PROP(prop) ((SI446X_PROP_GROUP_PKT << 8) | prop)
#define PA_PROP(prop) ((SI446X_PROP_GROUP_PA << 8) | prop)
#define MATCH_PROP(prop) ((SI446X_PROP_GROUP_MATCH << 8) | prop)

// GLOBAL 属性组 (0x00)
#define SI446X_GLOBAL_CONFIG GLOBAL_PROP(0x03)
#define SI446X_FIFO_MODE_HALF_DUPLEX 0x10

#define SI446X_GLOBAL_CLK_CFG GLOBAL_PROP(0x01)
#define SI446X_DIVIDED_CLK_DIS 0x00
#define SI446X_DIVIDED_CLK_EN 0x40
#define SI446X_DIVIDED_CLK_SEL_DIV_1 0 << 5
#define SI446X_DIVIDED_CLK_SEL_DIV_2 1 << 5
#define SI446X_DIVIDED_CLK_SEL_DIV_3 2 << 5
#define SI446X_DIVIDED_CLK_SEL_DIV_7_5 3 << 5
#define SI446X_DIVIDED_CLK_SEL_DIV_10 4 << 5
#define SI446X_DIVIDED_CLK_SEL_DIV_15 5 << 5
#define SI446X_DIVIDED_CLK_SEL_DIV_30 6 << 5
#define SI446X_DIVIDED_CLK_32K_SEL_OFF 0x00
#define SI446X_DIVIDED_CLK_32K_SEL_RC 0x01
#define SI446X_DIVIDED_CLK_32K_SEL_XTAL 0x02

#define SI446X_GLOBAL_LOW_BATT_THRESH GLOBAL_PROP(0x02)
#define SI446X_GLOBAL_WUT_CONFIG GLOBAL_PROP(0x04)
#define SI446X_GLOBAL_WUT_M GLOBAL_PROP(0x05)
#define SI446X_GLOBAL_WUT_R GLOBAL_PROP(0x07)
#define SI446X_GLOBAL_WUT_LDC GLOBAL_PROP(0x08)
#define SI446X_WUT_SLEEP 5
#define SI446X_LDC_MAX_PERIODS_FOREVER 0 << 6
#define SI446X_LDC_MAX_PERIODS_TWO 1 << 6
#define SI446X_LDC_MAX_PERIODS_FOUR 2 << 6
#define SI446X_LDC_MAX_PERIODS_EIGHT 3 << 6
#define SI446X_GLOBAL_WUT_CONFIG_WUT_LDC_EN_RX 1 << 6
#define SI446X_GLOBAL_WUT_CONFIG_WUT_EN 1
#define SI446X_GLOBAL_WUT_CONFIG_WUT_LBD_EN 2

// INT 属性组 (0x01)
#define SI446X_INT_CTL_ENABLE INT_PROP(0x00)
#define SI446X_INT_CTL_PH_ENABLE INT_PROP(0x01)
#define SI446X_INT_CTL_MODEM_ENABLE INT_PROP(0x02)
#define SI446X_INT_CTL_CHIP_ENABLE INT_PROP(0x03)
#define SI446X_INT_CTL_CHIP_LOW_BATT_EN 1
#define SI446X_INT_CTL_CHIP_WUT_EN 0
#define SI446X_FILTER_MATCH_PEND 7
#define SI446X_FILTER_MISS_PEND 6
#define SI446X_PACKET_SENT_PEND 5
#define SI446X_PACKET_RX_PEND 4
#define SI446X_CRC_ERROR_PEND 3
#define SI446X_INVALID_SYNC_PEND 5
#define SI446X_SYNC_DETECT_PEND 0
#define SI446X_LOW_BATT_PEND 1
#define SI446X_WUT_PEND 0

// MATCH 属性组 (0x30)
#define SI446X_MATCH_VALUE_1 MATCH_PROP(0x00)
#define SI446X_MATCH_EN 0x40

// PA 属性组 (0x22)
#define SI446X_PA_PWR_LVL PA_PROP(0x01)

// MODEM 属性组 (0x20)
#define SI446X_MODEM_MOD_TYPE ((SI446X_PROP_GROUP_MODEM << 8) | 0x00)
#define SI446X_MODEM_MAP_CONTROL ((SI446X_PROP_GROUP_MODEM << 8) | 0x01)
#define SI446X_MODEM_DATA_RATE_2 ((SI446X_PROP_GROUP_MODEM << 8) | 0x03)
#define SI446X_MODEM_FREQ_DEV_2 ((SI446X_PROP_GROUP_MODEM << 8) | 0x0A)
#define SI446X_MODEM_CLKGEN_BAND ((SI446X_PROP_GROUP_MODEM << 8) | 0x51)

// MODEM_MOD_TYPE 位掩码
#define SI446X_MODEM_MOD_TYPE_MASK_SYNC 0x7F  // 清 bit7 = 同步模式
#define SI446X_MODEM_MOD_TYPE_MASK_ASYNC 0x80 // 置 bit7 = 异步模式

// FREQ_CONTROL 属性组 (0x40)
#define SI446X_FREQ_CONTROL_INTE ((SI446X_PROP_GROUP_FREQ_CONTROL << 8) | 0x00)
#define SI446X_FREQ_CONTROL_CHANNEL_STEP_SIZE                                  \
  ((SI446X_PROP_GROUP_FREQ_CONTROL << 8) | 0x04)

// PKT 属性组 (0x12)
#define SI446X_PKT_FIELD_1_LENGTH PKT_PROP(0x0D)
#define SI446X_PKT_FIELD_2_LENGTH PKT_PROP(0x11)
#define SI446X_PKT_FIELD_2_LENGTH_LOW PKT_PROP(0x12)

/* =================================================================
 * §4 枚举与结构体
 * =================================================================
 */

/**
 * GPIO 模式 (GPIO_PIN_CFG 命令).
 *
 * ⚠ 数值以 Si446x 数据手册为准:
 *   0x20 = RX_STATE (RX 时拉高), 0x21 = TX_STATE (TX 时拉高)。
 * 原库 Si446x.h 枚举中 TX/RX 两值颠倒 (TX=0x20, RX=0x21), 合并时已修正 —
 * 实际生效值一直是 Si446x_defs.h 宏 (0x21/0x20), 本工程 RF 天线开关
 * GPIO2=TX_STATE(0x21), GPIO3=RX_STATE(0x20)。
 */
typedef enum {
  SI446X_GPIO_MODE_DONOTHING = 0x00,
  SI446X_GPIO_MODE_TRISTATE = 0x01,
  SI446X_GPIO_MODE_DRIVE0 = 0x02,
  SI446X_GPIO_MODE_DRIVE1 = 0x03,
  SI446X_GPIO_MODE_INPUT = 0x04,
  SI446X_GPIO_MODE_32K_CLK = 0x05,
  SI446X_GPIO_MODE_BOOT_CLK = 0x06,
  SI446X_GPIO_MODE_DIV_CLK = 0x07,
  SI446X_GPIO_MODE_CTS = 0x08,
  SI446X_GPIO_MODE_INV_CTS = 0x09,
  SI446X_GPIO_MODE_CMD_OVERLAP = 0x0A,
  SI446X_GPIO_MODE_SDO = 0x0B,
  SI446X_GPIO_MODE_POR = 0x0C,
  SI446X_GPIO_MODE_CAL_WUT = 0x0D,
  SI446X_GPIO_MODE_WUT = 0x0E,
  SI446X_GPIO_MODE_EN_PA = 0x0F,
  SI446X_GPIO_MODE_TX_DATA_CLK = 0x10,
  SI446X_GPIO_MODE_RX_DATA_CLK = 0x11,
  SI446X_GPIO_MODE_EN_LNA = 0x12,
  SI446X_GPIO_MODE_TX_DATA = 0x13,
  SI446X_GPIO_MODE_RX_DATA = 0x14,
  SI446X_GPIO_MODE_RX_RAW_DATA = 0x15,
  SI446X_GPIO_MODE_ANTENNA_1_SW = 0x16,
  SI446X_GPIO_MODE_ANTENNA_2_SW = 0x17,
  SI446X_GPIO_MODE_VALID_PREAMBLE = 0x18,
  SI446X_GPIO_MODE_INVALID_PREAMBLE = 0x19,
  SI446X_GPIO_MODE_SYNC_WORD_DETECT = 0x1A,
  SI446X_GPIO_MODE_CCA = 0x1B,
  SI446X_GPIO_MODE_IN_SLEEP = 0x1C,
  SI446X_GPIO_MODE_PKT_TRACE = 0x1D,
  SI446X_GPIO_MODE_TX_RX_DATA_CLK = 0x1F,
  SI446X_GPIO_MODE_RX_STATE = 0x20, // RX 时拉高
  SI446X_GPIO_MODE_TX_STATE = 0x21, // TX 时拉高
  SI446X_GPIO_MODE_RX_FIFO_FULL = 0x22,
  SI446X_GPIO_MODE_TX_FIFO_EMPTY = 0x23,
  SI446X_GPIO_MODE_LOW_BATT = 0x24,
  SI446X_GPIO_MODE_CCA_LATCH = 0x25,
  SI446X_GPIO_MODE_HOPPED = 0x26,
  SI446X_GPIO_MODE_HOP_TABLE_WRAP = 0x27
} si446x_gpio_mode_t;

/** NIRQ 引脚模式 (GPIO_PIN_CFG 命令) */
typedef enum {
  SI446X_NIRQ_MODE_DONOTHING = 0x00,
  SI446X_NIRQ_MODE_TRISTATE = 0x01,
  SI446X_NIRQ_MODE_DRIVE0 = 0x02,
  SI446X_NIRQ_MODE_DRIVE1 = 0x03,
  SI446X_NIRQ_MODE_INPUT = 0x04,
  SI446X_NIRQ_MODE_DIV_CLK = 0x07,
  SI446X_NIRQ_MODE_CTS = 0x08,
  SI446X_NIRQ_MODE_SDO = 0x0B,
  SI446X_NIRQ_MODE_POR = 0x0C,
  SI446X_NIRQ_MODE_EN_PA = 0x0F,
  SI446X_NIRQ_MODE_TX_DATA_CLK = 0x10,
  SI446X_NIRQ_MODE_RX_DATA_CLK = 0x11,
  SI446X_NIRQ_MODE_EN_LNA = 0x12,
  SI446X_NIRQ_MODE_TX_DATA = 0x13,
  SI446X_NIRQ_MODE_RX_DATA = 0x14,
  SI446X_NIRQ_MODE_RX_RAW_DATA = 0x15,
  SI446X_NIRQ_MODE_ANTENNA_1_SW = 0x16,
  SI446X_NIRQ_MODE_ANTENNA_2_SW = 0x17,
  SI446X_NIRQ_MODE_VALID_PREAMBLE = 0x18,
  SI446X_NIRQ_MODE_INVALID_PREAMBLE = 0x19,
  SI446X_NIRQ_MODE_SYNC_WORD_DETECT = 0x1A,
  SI446X_NIRQ_MODE_CCA = 0x1B,
  SI446X_NIRQ_MODE_PKT_TRACE = 0x1D,
  SI446X_NIRQ_MODE_TX_RX_DATA_CLK = 0x1F,
  SI446X_NIRQ_MODE_NIRQ = 0x27
} si446x_nirq_mode_t;

/** SDO 引脚模式 (GPIO_PIN_CFG 命令) */
typedef enum {
  SI446X_SDO_MODE_DONOTHING = 0x00,
  SI446X_SDO_MODE_TRISTATE = 0x01,
  SI446X_SDO_MODE_DRIVE0 = 0x02,
  SI446X_SDO_MODE_DRIVE1 = 0x03,
  SI446X_SDO_MODE_INPUT = 0x04,
  SI446X_SDO_MODE_32K_CLK = 0x05,
  SI446X_SDO_MODE_DIV_CLK = 0x07,
  SI446X_SDO_MODE_CTS = 0x08,
  SI446X_SDO_MODE_SDO = 0x0B,
  SI446X_SDO_MODE_POR = 0x0C,
  SI446X_SDO_MODE_WUT = 0x0E,
  SI446X_SDO_MODE_EN_PA = 0x0F,
  SI446X_SDO_MODE_TX_DATA_CLK = 0x10,
  SI446X_SDO_MODE_RX_DATA_CLK = 0x11,
  SI446X_SDO_MODE_EN_LNA = 0x12,
  SI446X_SDO_MODE_TX_DATA = 0x13,
  SI446X_SDO_MODE_RX_DATA = 0x14,
  SI446X_SDO_MODE_RX_RAW_DATA = 0x15,
  SI446X_SDO_MODE_ANTENNA_1_SW = 0x16,
  SI446X_SDO_MODE_ANTENNA_2_SW = 0x17,
  SI446X_SDO_MODE_VALID_PREAMBLE = 0x18,
  SI446X_SDO_MODE_INVALID_PREAMBLE = 0x19,
  SI446X_SDO_MODE_SYNC_WORD_DETECT = 0x1A,
  SI446X_SDO_MODE_CCA = 0x1B,
  SI446X_SDO_MODE_PKT_TRACE = 0x1D,
  SI446X_SDO_MODE_TX_RX_DATA_CLK = 0x1F,
} si446x_sdo_mode_t;

/** ADC 转换目标 (GET_ADC_READING 命令) */
typedef enum {
  SI446X_ADC_CONV_TEMP = 16,
  SI446X_ADC_CONV_BATT = 8,
  SI446X_ADC_CONV_GPIO = 4
} si446x_adc_conv_t;

/** ADC 输入范围 */
typedef enum {
  SI446X_ADC_RANGE_0P8 = 0,
  SI446X_ADC_RANGE_1P6 = 4,
  SI446X_ADC_RANGE_3P2 = 5,
  SI446X_ADC_RANGE_2P4 = 8,
  SI446X_ADC_RANGE_3P6 = 9
} si446x_adc_range_t;

/** GPIO 引脚编号 (Si446x_gpio_t: GPIO_PIN_CFG 参数索引) */
typedef enum {
  SI446X_GPIO0 = 0,
  SI446X_GPIO1 = 1,
  SI446X_GPIO2 = 2,
  SI446X_GPIO3 = 3,
  SI446X_NIRQ = 4,
  SI446X_SDO = 5
} si446x_gpio_t;

/** 芯片状态机 (REQUEST_DEVICE_STATE 命令返回值) */
typedef enum {
  SI446X_STATE_NOCHANGE = 0x00,
  SI446X_STATE_SLEEP = 0x01,
  SI446X_STATE_SPI_ACTIVE = 0x02,
  SI446X_STATE_READY = 0x03,
  SI446X_STATE_READY2 = 0x04,
  SI446X_STATE_TX_TUNE = 0x05,
  SI446X_STATE_RX_TUNE = 0x06,
  SI446X_STATE_TX = 0x07,
  SI446X_STATE_RX = 0x08
} si446x_state_t;

/** PART_INFO 命令返回的芯片信息 */
typedef struct {
  uint8_t chipRev;
  uint16_t part;
  uint8_t partBuild;
  uint16_t id;
  uint8_t customer;
  uint8_t romId;   // 3 = B1B, 6 = C2A (硬件版本判断依据)
  uint8_t revExternal;
  uint8_t revBranch;
  uint8_t revInternal;
  uint16_t patch;
  uint8_t func;
} si446x_info_t;

/* =================================================================
 * §5 其他宏
 * =================================================================
 */

#define SI446X_MAX_PACKET_LEN 128 ///< 最大包长
#define SI446X_MAX_TX_POWER 127   ///< 最大 TX 功率 (+20dBm/100mW)

// 唤醒定时器 (WUT) 事件源 (Si446x_setupWUT 的 config 参数)
#define SI446X_WUT_RUN 1
#define SI446X_WUT_BATT 2
#define SI446X_WUT_RX 4

// FIFO 清除掩码 (FIFO_INFO 命令)
#define SI446X_FIFO_CLEAR_RX 0x02
#define SI446X_FIFO_CLEAR_TX 0x01

// GPIO 上拉使能位 (与模式值 OR)
#define SI446X_GPIO_PULL_EN 0x40

// 回调事件位
#define SI446X_CBS_SENT _BV(5 + 8)
#define SI446X_CBS_RXBEGIN _BV(0)

/* =================================================================
 * §6 回调弱符号 (可在自己的代码中重定义覆盖)
 * =================================================================
 */
void SI446X_CB_RXBEGIN(int16_t rssi);
void SI446X_CB_RXCOMPLETE(uint8_t length, int16_t rssi);
void SI446X_CB_RXINVALID(int16_t rssi);
void SI446X_CB_SENT(void);
void SI446X_CB_WUT(void);
void SI446X_CB_LOWBATT(void);
void SI446X_CB_CMDTIMEOUT(void);

/* =================================================================
 * §7 公共 API
 * ================================================================= */

/**
 * @brief 初始化射频芯片。必须先于其他一切调用!
 *
 * SPI 与 GPIO 引脚使用 §1 的编译期默认值。若需自定义引脚,
 * 请在此之前调用 Si446x_setPins()。
 */
void Si446x_init(void);

/**
 * @brief 指定 SPI/GPIO 引脚 (板级配置, 必须在 Si446x_init() 之前调用)。
 *
 * 库不再内置编译期默认引脚 — 引脚是板级硬件信息, 由应用方定义
 * (BeeperTX 中为 main.cpp 顶部的 PIN_* 宏)。
 *
 * 每个引脚传 -1 表示"保持当前值"; 初始值均为 -1 (未设置),
 * 因此除 IRQ 外的 6 个引脚必须在 Si446x_init() 前至少设置一次,
 * 否则 init 会打印错误并中止。
 *
 * @param csn     SPI 片选 (低有效)
 * @param sdn     关机引脚 (高=复位), 必填
 * @param irq     中断引脚, 或 -1 禁用 (轮询模式)
 * @param txData  Direct TX 数据引脚 (→ Si4463 GPIO0), 必填
 * @param sclk    SPI 时钟, 必填
 * @param mosi    SPI MOSI (→ SDI), 必填
 * @param miso    SPI MISO (← SDO), 必填
 */
void Si446x_setPins(int8_t csn, int8_t sdn, int8_t irq, int8_t txData,
                    int8_t sclk, int8_t mosi, int8_t miso);

/**
 * @brief 通过 GPIO_PIN_CFG 命令配置所有 GPIO/NIRQ/SDO 引脚。
 *
 * WDS 启动配置未正确设置 GPIO 时使用, 或运行时修改。
 * 模式常量见 §4 枚举 (SI446X_GPIO_MODE_* / SI446X_NIRQ_MODE_* /
 * SI446X_SDO_MODE_*)。与 SI446X_GPIO_PULL_EN (0x40) OR 以启用内部上拉。
 *
 * 示例 — RF 天线开关:
 *   Si446x_configGPIO(0x00, 0x00,
 *                     SI446X_GPIO_MODE_TX_STATE | SI446X_GPIO_PULL_EN,
 *                     SI446X_GPIO_MODE_RX_STATE | SI446X_GPIO_PULL_EN,
 *                     SI446X_NIRQ_MODE_DONOTHING,
 *                     SI446X_SDO_MODE_DONOTHING,
 *                     0x00);
 */
void Si446x_configGPIO(uint8_t gpio0, uint8_t gpio1, uint8_t gpio2,
                       uint8_t gpio3, uint8_t nirq, uint8_t sdo,
                       uint8_t genConfig);

/**
 * @brief 配置 GPIO2=TX_STATE, GPIO3=RX_STATE 用于 RF 天线开关。
 *
 * 便捷封装 — 以 RF 开关参数调用 Si446x_configGPIO()。
 * 仅当 WDS 配置未设置 GPIO 时才需要调用。
 */
void Si446x_configRFswitch(void);

/* =================================================================
 *  Direct TX API (需要 WDS 配置中将 MODEM_MOD_TYPE 设为同步或异步
 *  直接模式)
 * ================================================================= */

/**
 * @brief 进入 Direct 模式 TX 状态。
 *
 * 调用后 Si4463 开始发射载波, MCU 必须将数据位送入 TX 数据引脚
 * (默认 WDS Direct TX 配置中为 GPIO0)。
 *
 * async=0 (同步, 默认) 时芯片在 GPIO1 输出 TX 位时钟, GPIO0 上的数据
 * 在每个时钟下降沿被采样; async=1 (异步) 时 MCU 直接控制数据速率,
 * 无需位时钟, 芯片对 GPIO0 过采样。
 *
 * 调用 Si446x_txDirectWrite() 发送数据, 完成后 Si446x_stopTX()。
 *
 * @param channel  频率信道 (0 = 基频)
 * @param async    0 = 同步 (默认), 1 = 异步
 */
void Si446x_startDirectTX(uint8_t channel, uint8_t async);

/**
 * @brief 在 Direct TX 模式下基于延时位定时写入数据。
 *
 * 按 radio_config.h 中配置的数据速率发送 (RADIO_CONFIGURATION_DATA_RSYMB_SPS,
 * 默认 1200 sps → 833 µs/bit), 阻塞调用:
 *   1.2 kbps 时 ~6.7 ms/字节, 100 字节 ~670 ms。
 *
 * 必须先通过 Si446x_startDirectTX() 进入 Direct TX 模式。
 *
 * @param data  数据指针
 * @param len   字节数
 */
void Si446x_txDirectWrite(const uint8_t *data, uint8_t len);

/**
 * @brief 停止 TX 并回到 IDLE_STATE。
 *
 * FIFO 模式与 Direct 模式均适用。
 */
void Si446x_stopTX(void);

/* =================================================================
 *  频率控制 API
 * ================================================================= */

/**
 * @brief 用原始寄存器值设置载波频率。
 *
 * 数值取自 WDS 或按公式计算:
 *   fRF(Hz) = fXO × (FC_INTE + 1 + FC_FRAC/2^19) / OUTDIV
 *
 * 例 (156 MHz, fXO=30MHz, OUTDIV=12):
 *   N = 156e6 × 12 / 30e6 = 62.4
 *   FC_INTE = floor(62.4) - 1 = 61 (0x3D)
 *   FC_FRAC = (62.4 - 62 + 1) × 2^19 = 1.4 × 524288 = 734003 ≈ 0x0B3333
 *
 * 应用新频率后触发新频段的 VCO 校准 (IRCAL)。芯片须处于 IDLE/READY。
 *
 * @param inte    FREQ_CONTROL_INTE (寄存器 0x4000)
 * @param frac    FREQ_CONTROL_FRAC (寄存器 0x4001-0x4003, 20-bit)
 * @param step    CHANNEL_STEP_SIZE (寄存器 0x4004-0x4005, 16-bit)
 * @param band    MODEM_CLKGEN_BAND (寄存器 0x2051)
 */
void Si446x_setFrequency(uint8_t inte, uint32_t frac, uint16_t step,
                         uint8_t band);

/**
 * @brief 便捷 — 以 kHz 设置频率。
 *
 * 自动选择输出分频并计算寄存器值。晶振频率取自 radio_config.h
 * (RADIO_CONFIGURATION_DATA_RADIO_XO_FREQ, 默认 30 MHz), 支持频段:
 *
 *   范围 (MHz)   | OUTDIV | CLKGEN_BAND
 *   ---------------|--------|-------------
 *    119 – 220    |   12   |   0x0D
 *    220 – 295    |    8   |   0x0C
 *    295 – 390    |    6   |   0x0B
 *    390 – 520    |    4   |   0x0A
 *    520 – 700    |    3   |   0x09
 *    700 – 1050   |    2   |   0x08
 *
 * @param freqKHz  目标频率 kHz (119000 – 1050000)
 * @return 1 成功, 0 超出范围
 */
uint8_t Si446x_setFrequencyKHz(uint32_t freqKHz);

/**
 * @brief 读取芯片信息
 */
void Si446x_getInfo(si446x_info_t *info);

/**
 * @brief 读取当前 RSSI (芯片须处于 RX 模式)
 * @return RSSI dBm (典型 -130 到 0)
 */
int16_t Si446x_getRSSI(void);

/**
 * @brief 设置 TX 功率 (0-127, dBm 映射见数据手册)
 */
void Si446x_setTxPower(uint8_t pwr);
uint8_t Si446x_getTxPower(void);

/**
 * @brief 使能/禁用回调
 */
void Si446x_setupCallback(uint16_t callbacks, uint8_t state);

/**
 * @brief 从 RX FIFO 读取接收数据
 */
void Si446x_read(void *buff, uint8_t len);

/**
 * @brief 发送一个包
 * @return 0 忙, 1 成功
 */
uint8_t Si446x_TX(void *packet, uint8_t len, uint8_t channel,
                  si446x_state_t onTxFinish);

/**
 * @brief 进入接收模式
 */
void Si446x_RX(uint8_t channel);

/**
 * @brief 设置低电量阈值
 */
void Si446x_setLowBatt(uint16_t voltage);

/**
 * @brief 配置唤醒定时器 (WUT)
 */
void Si446x_setupWUT(uint8_t r, uint16_t m, uint8_t ldc, uint8_t config);

/**
 * @brief 禁用 WUT
 */
void Si446x_disableWUT(void);

/**
 * @brief 进入休眠模式
 * @return 0 忙 (正在发射), 1 成功
 */
uint8_t Si446x_sleep(void);

/**
 * @brief 获取当前射频状态
 */
si446x_state_t Si446x_getState(void);

/**
 * @brief 读取某个 GPIO 引脚的 ADC 值
 */
uint16_t Si446x_adc_gpio(uint8_t pin);

/**
 * @brief 读取供电电压 (mV)
 */
uint16_t Si446x_adc_battery(void);

/**
 * @brief 读取温度 (摄氏度)
 */
float Si446x_adc_temperature(void);

/**
 * @brief 配置单个 GPIO/NIRQ/SDO 引脚
 */
void Si446x_writeGPIO(si446x_gpio_t pin, uint8_t value);

/**
 * @brief 读取 GPIO 引脚状态
 */
uint8_t Si446x_readGPIO(void);

/**
 * @brief 转储属性组所有值
 */
uint8_t Si446x_dump(void *buff, uint8_t group);

/**
 * @brief 处理射频事件 (轮询模式下在 loop() 中频繁调用)
 */
void Si446x_SERVICE(void);

/**
 * @brief 禁用 Si446x 中断
 */
uint8_t Si446x_irq_off(void);

/**
 * @brief 重新启用 Si446x 中断
 */
void Si446x_irq_on(uint8_t origVal);

/**
 * @brief 回读当前载波频率 (kHz)。
 *
 * 由实时 FREQ_CONTROL_INTE/FRAC 和 CLKGEN_BAND 寄存器计算。
 *
 * @return 频率 kHz, 频段未知时返回 0
 */
uint32_t Si446x_getFrequencyKHz(void);

#endif /* SI446X_H_ */
