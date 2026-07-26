/*
 * Project: Si4463 Radio Library for ESP32 (ported from Zak Kemble's AVR/Arduino
 * library) Author: Zak Kemble, contact@zakkemble.co.uk Ported for ESP32 + RF
 * switch control Copyright: (C) 2017 by Zak Kemble License: GNU GPL v3 (see
 * License.txt) Web:
 * http://blog.zakkemble.co.uk/si4463-radio-library-avr-arduino/
 *
 * Pin configuration — defaults used at compile time.
 * Override any pin at runtime with Si446x_setPins().
 */

#ifndef SI443X_CONFIG_H_
#define SI443X_CONFIG_H_

// If other libraries communicate with SPI devices while inside an interrupt
// then set this to 1
#define SI446X_INT_SPI_COMMS 0

// ADC Conversion speed (10-12 recommended)
#define SI446X_ADC_SPEED 10

// Idle mode (READY = fast response, 1.8mA; SPI_ACTIVE = lower power, 1.35mA)
#define SI446X_IDLE_MODE SI446X_STATE_READY

// Packet length mode: 0 = variable length, >0 = fixed length
#define SI446X_FIXED_LENGTH 0

// ============================================================
// Pin Assignments (defaults — override at runtime if needed)
// ============================================================
// Si4463   | ESP32-S3 | Function
// ---------|----------|-------------------------------
// SDN      | GPIO5    | Shutdown (high = reset)
// nSEL/CS  | GPIO10   | SPI chip-select (active low)
// SCLK     | GPIO12   | SPI clock
// SDI      | GPIO11   | SPI MOSI (master out)
// SDO      | GPIO13   | SPI MISO (master in)
// nIRQ     | GPIO9    | Interrupt output (active low)
// GPIO0    | GPIO21   | Direct TX data out (optional)
// ============================================================

#define SI446X_CSN 10
#define SI446X_SDN 5
#define SI446X_IRQ -1 // set to -1 to disable interrupts (ESP32: AVR ISR detach/attach inside SPI
     // ops crashes)
#define SI446X_TX_DATA_PIN 16

#define SI446X_SCLK 12
#define SI446X_MOSI 11
#define SI446X_MISO 13

#endif /* SI443X_CONFIG_H_ */
