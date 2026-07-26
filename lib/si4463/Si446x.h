/*
 * Project: Si4463 Radio Library for ESP32
 * Original Author: Zak Kemble, contact@zakkemble.co.uk (AVR/Arduino version)
 * Ported for ESP32 + RF switch control by ASHINING RF design adaptation
 * Copyright: (C) 2017 by Zak Kemble
 * License: GNU GPL v3 (see License.txt)
 * Web: http://blog.zakkemble.co.uk/si4463-radio-library-avr-arduino/
 *
 * This port adds:
 *   - ESP32-S3 / Arduino framework compatibility
 *   - Automatic RF TX/RX antenna switch control via GPIO2/GPIO3
 *     (GPIO2 = TX_STATE, GPIO3 = RX_STATE, active-high)
 */

#ifndef SI443X_H_
#define SI443X_H_

#include <Arduino.h>
#include <stdint.h>

#include "Si446x_config.h"

#define SI446X_MAX_PACKET_LEN 128 ///< Maximum packet length
#define SI446X_MAX_TX_POWER 127   ///< Maximum TX power (+20dBm/100mW)

#define SI446X_WUT_RUN 1
#define SI446X_WUT_BATT 2
#define SI446X_WUT_RX 4

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
  SI446X_GPIO_MODE_TX_STATE = 0x20,
  SI446X_GPIO_MODE_RX_STATE = 0x21,
  SI446X_GPIO_MODE_RX_FIFO_FULL = 0x22,
  SI446X_GPIO_MODE_TX_FIFO_EMPTY = 0x23,
  SI446X_GPIO_MODE_LOW_BATT = 0x24,
  SI446X_GPIO_MODE_CCA_LATCH = 0x25,
  SI446X_GPIO_MODE_HOPPED = 0x26,
  SI446X_GPIO_MODE_HOP_TABLE_WRAP = 0x27
} si446x_gpio_mode_t;

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

typedef struct {
  uint8_t chipRev;
  uint16_t part;
  uint8_t partBuild;
  uint16_t id;
  uint8_t customer;
  uint8_t romId;
  uint8_t revExternal;
  uint8_t revBranch;
  uint8_t revInternal;
  uint16_t patch;
  uint8_t func;
} si446x_info_t;

typedef enum {
  SI446X_GPIO0 = 0,
  SI446X_GPIO1 = 1,
  SI446X_GPIO2 = 2,
  SI446X_GPIO3 = 3,
  SI446X_NIRQ = 4,
  SI446X_SDO = 5
} si446x_gpio_t;

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

#define SI446X_CBS_SENT _BV(5 + 8)
#define SI446X_CBS_RXBEGIN _BV(0)

/* ---- Callback weak symbols (override by defining in your code) ---- */
void SI446X_CB_RXBEGIN(int16_t rssi);
void SI446X_CB_RXCOMPLETE(uint8_t length, int16_t rssi);
void SI446X_CB_RXINVALID(int16_t rssi);
void SI446X_CB_SENT(void);
void SI446X_CB_WUT(void);
void SI446X_CB_LOWBATT(void);
void SI446X_CB_CMDTIMEOUT(void);

/* =================================================================
 *  Public API
 * ================================================================= */

/**
 * @brief Initialise the radio. Must be called before anything else!
 *
 * SPI and GPIO pins are read from the compile-time defaults in
 * Si446x_config.h.  Call Si446x_setPins() BEFORE this if you
 * want custom pins at runtime.
 */
void Si446x_init(void);

/**
 * @brief Override pin assignments at runtime.
 *
 * Call BEFORE Si446x_init().  Each pin-1 means "use the compile-time
 * default from Si446x_config.h".
 *
 * @param csn     SPI chip-select (SI446X_CSN)
 * @param sdn     Shutdown pin (SI446X_SDN)
 * @param irq     Interrupt pin, or -1 to disable (SI446X_IRQ)
 * @param txData  Direct TX data pin (SI446X_TX_DATA_PIN)
 * @param sclk    SPI clock (SI446X_SCLK)
 * @param mosi    SPI MOSI  (SI446X_MOSI)
 * @param miso    SPI MISO  (SI446X_MISO)
 */
void Si446x_setPins(int8_t csn, int8_t sdn, int8_t irq, int8_t txData,
                    int8_t sclk, int8_t mosi, int8_t miso);

/**
 * @brief Configure all GPIO/NIRQ/SDO pins via GPIO_PIN_CFG command.
 *
 * Use this if the WDS startup config doesn't set the GPIOs the way you
 * need, or if you want to change them at runtime.
 *
 * Common mode constants are defined in Si446x_defs.h:
 *   SI446X_GPIO_MODE_DONOTHING, _INPUT, _TX_STATE, _RX_STATE,
 *   SI446X_GPIO_MODE_TX_DATA_CLK, etc.
 *
 * OR with SI446X_GPIO_PULL_EN (0x40) to enable the internal pull-up.
 *
 * Example — RF antenna switch (drv_periph compatible):
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
 * @brief Configure GPIO2=TX_STATE, GPIO3=RX_STATE for RF antenna switch.
 *
 * Convenience wrapper — calls Si446x_configGPIO() with RF switch values.
 * Only needed if your WDS config doesn't already set up the GPIOs.
 */
void Si446x_configRFswitch(void);

/* =================================================================
 *  Direct TX API  (requires a WDS config with MODEM_MOD_TYPE set for
 *  Synchronous or Asynchronous Direct mode)
 * ================================================================= */

/**
 * @brief Enter TX state for Direct mode.
 *
 * After this call the Si4463 begins transmitting its carrier.  The MCU
 * must then feed data bits to the TX-data GPIO pin (GPIO0 in the
 * default WDS Direct TX config).
 *
 * When async=0 (synchronous, default), the chip outputs a TX bit clock
 * on GPIO1; data on GPIO0 is sampled on the falling edge of each clock.
 * When async=1 (asynchronous), the MCU controls the data rate directly
 * — no bit clock is required, the chip oversamples GPIO0.
 *
 * Call Si446x_txDirectWrite() to send data, then Si446x_stopTX() to
 * finish.
 *
 * @param channel  Frequency channel (0 = base frequency)
 * @param async    0 = synchronous (default), 1 = asynchronous
 */
void Si446x_startDirectTX(uint8_t channel, uint8_t async);

/**
 * @brief Write data in Direct TX mode using delay-based bit timing.
 *
 * Sends bits at the data rate configured in radio_config.h.
 * This is a blocking call:
 *   @ 1.2 kbps → ~6.7 ms per byte, ~670 ms for 100 bytes.
 *
 * The Si4463 must have been placed in Direct TX mode first via
 * Si446x_startDirectTX().
 *
 * @param data  Pointer to data bytes
 * @param len   Number of bytes to send
 */
void Si446x_txDirectWrite(const uint8_t *data, uint8_t len);

/**
 * @brief Stop TX and return to IDLE_STATE.
 *
 * Works for both FIFO mode and Direct mode.
 */
void Si446x_stopTX(void);

/* =================================================================
 *  Frequency Control API
 * ================================================================= */

/**
 * @brief Set carrier frequency using raw register values.
 *
 * Use values from WDS or calculate with the formula:
 *   fRF(Hz) = fXO × (FC_INTE + 1 + FC_FRAC/2^19) / OUTDIV
 *
 * Example (156 MHz, fXO=30MHz, OUTDIV=12):
 *   N = 156e6 × 12 / 30e6 = 62.4
 *   FC_INTE = floor(62.4) - 1 = 61 (0x3D)
 *   FC_FRAC = (62.4 - 62 + 1) × 2^19 = 1.4 × 524288 = 734003 ≈ 0x0B3333
 *
 * After applying the new frequency the function triggers a VCO calibration
 * (IRCAL) for the new band.  The chip must be in IDLE/READY state.
 *
 * @param inte    FREQ_CONTROL_INTE (register 0x4000)
 * @param frac    FREQ_CONTROL_FRAC (registers 0x4001-0x4003, 20-bit)
 * @param step    CHANNEL_STEP_SIZE (registers 0x4004-0x4005, 16-bit)
 * @param band    MODEM_CLKGEN_BAND (register 0x2051)
 */
void Si446x_setFrequency(uint8_t inte, uint32_t frac, uint16_t step,
                         uint8_t band);

/**
 * @brief Convenience — set frequency in kHz.
 *
 * Auto-selects the output divider and calculates register values.
 * Supported bands with the default 30 MHz XTAL:
 *
 *   Range (MHz)   | OUTDIV | CLKGEN_BAND
 *   ---------------|--------|-------------
 *    119 – 220    |   12   |   0x0D
 *    220 – 295    |    8   |   0x0C
 *    295 – 390    |    6   |   0x0B
 *    390 – 520    |    4   |   0x0A
 *    520 – 700    |    3   |   0x09
 *    700 – 1050   |    2   |   0x08
 *
 * @param freqKHz  Desired frequency in kHz (119000 – 1050000)
 * @return 1 on success, 0 if out of range
 */
uint8_t Si446x_setFrequencyKHz(uint32_t freqKHz);

/**
 * @brief Get chip info
 */
void Si446x_getInfo(si446x_info_t *info);

/**
 * @brief Get current RSSI (chip must be in RX mode)
 * @return RSSI in dBm (-130 to 0 typical)
 */
int16_t Si446x_getRSSI(void);

/**
 * @brief Set TX power (0-127, see datasheet for dBm mapping)
 */
void Si446x_setTxPower(uint8_t pwr);
uint8_t Si446x_getTxPower(void);

/**
 * @brief Enable/disable callbacks
 */
void Si446x_setupCallback(uint16_t callbacks, uint8_t state);

/**
 * @brief Read received data from RX FIFO
 */
void Si446x_read(void *buff, uint8_t len);

/**
 * @brief Transmit a packet
 * @return 0 if busy, 1 on success
 */
uint8_t Si446x_TX(void *packet, uint8_t len, uint8_t channel,
                  si446x_state_t onTxFinish);

/**
 * @brief Enter receive mode
 */
void Si446x_RX(uint8_t channel);

/**
 * @brief Set low battery voltage threshold
 */
void Si446x_setLowBatt(uint16_t voltage);

/**
 * @brief Configure the wake-up timer (WUT)
 */
void Si446x_setupWUT(uint8_t r, uint16_t m, uint8_t ldc, uint8_t config);

/**
 * @brief Disable WUT
 */
void Si446x_disableWUT(void);

/**
 * @brief Enter sleep mode
 * @return 0 if busy transmitting, 1 on success
 */
uint8_t Si446x_sleep(void);

/**
 * @brief Get current radio state
 */
si446x_state_t Si446x_getState(void);

/**
 * @brief Read ADC value from a GPIO pin
 */
uint16_t Si446x_adc_gpio(uint8_t pin);

/**
 * @brief Read supply voltage in millivolts
 */
uint16_t Si446x_adc_battery(void);

/**
 * @brief Read temperature in Celsius
 */
float Si446x_adc_temperature(void);

/**
 * @brief Configure a single GPIO/NIRQ/SDO pin
 */
void Si446x_writeGPIO(si446x_gpio_t pin, uint8_t value);

/**
 * @brief Read GPIO pin states
 */
uint8_t Si446x_readGPIO(void);

/**
 * @brief Dump all values of a property group
 */
uint8_t Si446x_dump(void *buff, uint8_t group);

/**
 * @brief Process radio events (call frequently in loop() when polling)
 */
void Si446x_SERVICE(void);

/**
 * @brief Disable Si446x interrupts
 */
uint8_t Si446x_irq_off(void);

/**
 * @brief Re-enable Si446x interrupts
 */
void Si446x_irq_on(uint8_t origVal);

/**
 * @brief Read back current carrier frequency in kHz.
 *
 * Calculated from live FREQ_CONTROL_INTE/FRAC and CLKGEN_BAND registers.
 *
 * @return Frequency in kHz, or 0 if band is unknown.
 */
uint32_t Si446x_getFrequencyKHz(void);

#endif /* SI443X_H_ */
