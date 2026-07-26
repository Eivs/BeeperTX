/*
 * Project: Si4463 Radio Library for ESP32
 * Original Author: Zak Kemble, contact@zakkemble.co.uk (AVR/Arduino version)
 * Ported for ESP32 + RF switch control
 * Copyright: (C) 2017 by Zak Kemble
 * License: GNU GPL v3 (see License.txt)
 *
 * Ported from the AVR/Arduino library with these adaptations:
 *   - Unified memory map (no PROGMEM needed on ESP32)
 *   - Arduino framework SPI, GPIO, and interrupt APIs
 *   - Automatic RF TX/RX antenna switch via GPIO2/GPIO3
 */

#include "Si446x.h"
#include "Si446x_config.h"
#include "Si446x_defs.h"
#include "radio_config.h"
#include <Arduino.h>
#include <SPI.h>
#include <stdint.h>
#include <string.h>

// ------------------------------------------------------------------
// Pin variables — initialised from config defaults, override at runtime
// ------------------------------------------------------------------
static int8_t _pin_csn = SI446X_CSN;
static int8_t _pin_sdn = SI446X_SDN;
static int8_t _pin_irq = SI446X_IRQ;
static int8_t _pin_txData = SI446X_TX_DATA_PIN;
static int8_t _pin_sclk = SI446X_SCLK;
static int8_t _pin_mosi = SI446X_MOSI;
static int8_t _pin_miso = SI446X_MISO;

// ------------------------------------------------------------------
// Runtime pin override — call before Si446x_init()
// ------------------------------------------------------------------
void Si446x_setPins(int8_t csn, int8_t sdn, int8_t irq, int8_t txData,
                    int8_t sclk, int8_t mosi, int8_t miso) {
  if (csn >= 0)
    _pin_csn = csn;
  if (sdn >= 0)
    _pin_sdn = sdn;
  if (irq >= 0)
    _pin_irq = irq;
  if (txData >= 0)
    _pin_txData = txData;
  if (sclk >= 0)
    _pin_sclk = sclk;
  if (mosi >= 0)
    _pin_mosi = mosi;
  if (miso >= 0)
    _pin_miso = miso;
}

// ------------------------------------------------------------------
// SPI abstraction — always read from _pin_csn
// ------------------------------------------------------------------
static inline void spiSelect(void) { digitalWrite(_pin_csn, LOW); }
static inline void spiDeselect(void) { digitalWrite(_pin_csn, HIGH); }
#define delay_ms(ms) delay(ms)
#define delay_us(us) delayMicroseconds(us)
#define spi_transfer_nr(data) (SPI.transfer(data))
#define spi_transfer(data) (SPI.transfer(data))

#define IDLE_STATE SI446X_IDLE_MODE

#define MAX_PACKET_LEN SI446X_MAX_PACKET_LEN

#define IRQ_PACKET 0
#define IRQ_MODEM 1
#define IRQ_CHIP 2

#define rssi_dBm(val) ((val / 2) - 134)

// ------------------------------------------------------------------
// Weak callbacks (user overrides by defining the same symbol)
// ------------------------------------------------------------------
void __attribute__((weak)) SI446X_CB_CMDTIMEOUT(void) {}
void __attribute__((weak)) SI446X_CB_RXBEGIN(int16_t rssi) { (void)(rssi); }
void __attribute__((weak)) SI446X_CB_RXCOMPLETE(uint8_t length, int16_t rssi) {
  (void)(length);
  (void)(rssi);
}
void __attribute__((weak)) SI446X_CB_RXINVALID(int16_t rssi) { (void)(rssi); }
void __attribute__((weak)) SI446X_CB_SENT(void) {}
void __attribute__((weak)) SI446X_CB_WUT(void) {}
void __attribute__((weak)) SI446X_CB_LOWBATT(void) {}

// ------------------------------------------------------------------
// Static configuration from WDS
// ------------------------------------------------------------------
static const uint8_t config[] = RADIO_CONFIGURATION_DATA_ARRAY;

static volatile uint8_t enabledInterrupts[3];

static volatile uint8_t isrState;
static volatile uint8_t isrBusy;

// ------------------------------------------------------------------
// Interrupt helpers for concurrent access protection
// ------------------------------------------------------------------
static inline uint8_t interrupt_off(void) {
  if (!isrBusy) {
    noInterrupts();
    isrState = isrState + 1;
  }
  return 1;
}

static inline uint8_t interrupt_on(void) {
  if (!isrBusy) {
    if (isrState > 0)
      isrState = isrState - 1;
    if (isrState == 0)
      interrupts();
  }
  return 0;
}

// ------------------------------------------------------------------
// SPI chip-select helpers
// ------------------------------------------------------------------
static inline uint8_t cselect(void) {
  spiSelect();
  return 1;
}

static inline uint8_t cdeselect(void) {
  spiDeselect();
  return 0;
}

#define CHIPSELECT() for (uint8_t _cs = cselect(); _cs; _cs = cdeselect())

#define SI446X_ATOMIC()                                                        \
  for (uint8_t _cs2 = interrupt_off(); _cs2; _cs2 = interrupt_on())

// ------------------------------------------------------------------
// Low-level API helpers
// ------------------------------------------------------------------

static uint8_t getResponse(void *buff, uint8_t len) {
  uint8_t cts = 0;
  SI446X_ATOMIC() {
    CHIPSELECT() {
      spi_transfer_nr(SI446X_CMD_READ_CMD_BUFF);
      cts = (spi_transfer(0xFF) == 0xFF);
      if (cts) {
        for (uint8_t i = 0; i < len; i++)
          ((uint8_t *)buff)[i] = spi_transfer(0xFF);
      }
    }
  }
  return cts;
}

static uint8_t waitForResponse(void *out, uint8_t outLen, uint8_t useTimeout) {
  uint16_t timeout = 40000;
  while (!getResponse(out, outLen)) {
    delayMicroseconds(10);
    if (useTimeout && !--timeout) {
      SI446X_CB_CMDTIMEOUT();
      return 0;
    }
  }
  return 1;
}

static void doAPI(void *data, uint8_t len, void *out, uint8_t outLen) {
  // Disable the Si446x interrupt while communicating
  uint8_t irq = Si446x_irq_off();

  if (waitForResponse(NULL, 0, 1)) {
    SI446X_ATOMIC() {
      CHIPSELECT() {
        for (uint8_t i = 0; i < len; i++)
          spi_transfer_nr(((uint8_t *)data)[i]);
      }
    }

    if (((uint8_t *)data)[0] == SI446X_CMD_IRCAL)
      waitForResponse(NULL, 0, 0);
    else if (out != NULL)
      waitForResponse(out, outLen, 1);
  }

  Si446x_irq_on(irq);
}

static void setProperties(uint16_t prop, void *values, uint8_t len) {
  uint8_t data[16] = {SI446X_CMD_SET_PROPERTY, (uint8_t)(prop >> 8), len,
                      (uint8_t)prop};
  memcpy(data + 4, values, len);
  doAPI(data, len + 4, NULL, 0);
}

static inline void setProperty(uint16_t prop, uint8_t value) {
  setProperties(prop, &value, 1);
}

static void getProperties(uint16_t prop, void *values, uint8_t len) {
  uint8_t data[] = {SI446X_CMD_GET_PROPERTY, (uint8_t)(prop >> 8), len,
                    (uint8_t)prop};
  doAPI(data, sizeof(data), values, len);
}

static inline uint8_t getProperty(uint16_t prop) {
  uint8_t val;
  getProperties(prop, &val, 1);
  return val;
}

static uint16_t getADC(uint8_t adc_en, uint8_t adc_cfg, uint8_t part) {
  uint8_t data[6] = {SI446X_CMD_GET_ADC_READING, adc_en, adc_cfg};
  doAPI(data, 3, data, 6);
  return (data[part] << 8 | data[part + 1]);
}

static uint8_t getFRR(uint8_t reg) {
  uint8_t frr = 0;
  SI446X_ATOMIC() {
    CHIPSELECT() {
      spi_transfer_nr(reg);
      frr = spi_transfer(0xFF);
    }
  }
  return frr;
}

static int16_t getLatchedRSSI(void) {
  uint8_t frr = getFRR(SI446X_CMD_READ_FRR_A);
  return rssi_dBm(frr);
}

static si446x_state_t getState(void) {
  uint8_t state = getFRR(SI446X_CMD_READ_FRR_B);
  if (state == SI446X_STATE_TX_TUNE)
    state = SI446X_STATE_TX;
  else if (state == SI446X_STATE_RX_TUNE)
    state = SI446X_STATE_RX;
  else if (state == SI446X_STATE_READY2)
    state = SI446X_STATE_READY;
  return (si446x_state_t)state;
}

static void setState(si446x_state_t newState) {
  uint8_t data[] = {SI446X_CMD_CHANGE_STATE, newState};
  doAPI(data, sizeof(data), NULL, 0);
}

static void clearFIFO(void) {
  static const uint8_t clearFifo[] = {
      SI446X_CMD_FIFO_INFO, SI446X_FIFO_CLEAR_RX | SI446X_FIFO_CLEAR_TX};
  doAPI((uint8_t *)clearFifo, sizeof(clearFifo), NULL, 0);
}

static void interrupt(void *buff) {
  uint8_t data = SI446X_CMD_GET_INT_STATUS;
  doAPI(&data, sizeof(data), buff, 8);
}

static void interrupt2(void *buff, uint8_t clearPH, uint8_t clearMODEM,
                       uint8_t clearCHIP) {
  uint8_t data[] = {SI446X_CMD_GET_INT_STATUS, clearPH, clearMODEM, clearCHIP};
  doAPI(data, sizeof(data), buff, 8);
}

// ------------------------------------------------------------------
// Hardware reset via SDN pin
// ------------------------------------------------------------------
static void resetDevice(void) {
  digitalWrite(_pin_sdn, HIGH);
  delay(50);
  digitalWrite(_pin_sdn, LOW);
  delay(50);
}

// ------------------------------------------------------------------
// Load WDS-generated configuration
// ------------------------------------------------------------------
static void applyStartupConfig(void) {
  uint8_t buff[17];
  for (uint16_t i = 0; i < sizeof(config); i++) {
    memcpy(buff, &config[i], sizeof(buff));
    doAPI(&buff[1], buff[0], NULL, 0);
    i += buff[0];
  }
}

// ==================================================================
// Public API
// ==================================================================

void Si446x_init() {
  spiDeselect();

  pinMode(_pin_csn, OUTPUT);
  digitalWrite(_pin_csn, HIGH); // deselect
  pinMode(_pin_sdn, OUTPUT);
  digitalWrite(_pin_sdn, LOW); // not in reset

  if (_pin_irq >= 0)
    pinMode(_pin_irq, INPUT_PULLUP);

  // Start SPI with explicit pins
  SPI.begin(_pin_sclk, _pin_miso, _pin_mosi, -1);

  resetDevice();
  applyStartupConfig();
  interrupt(NULL);
  Si446x_sleep();

  enabledInterrupts[IRQ_PACKET] =
      (1 << SI446X_PACKET_RX_PEND) | (1 << SI446X_CRC_ERROR_PEND);

  // Note: GPIO configuration is applied by the WDS config during
  // applyStartupConfig().  If your config doesn't set up the RF
  // antenna switch or Direct TX data pins, call Si446x_configGPIO()
  // or Si446x_configRFswitch() here.

  if (_pin_irq >= 0)
    Si446x_irq_on(1);
  else {
    (void)isrState;
    (void)isrBusy;
  }
}

/**
 * @brief Configure all GPIO/NIRQ/SDO pins via GPIO_PIN_CFG command.
 */
void Si446x_configGPIO(uint8_t gpio0, uint8_t gpio1, uint8_t gpio2,
                       uint8_t gpio3, uint8_t nirq, uint8_t sdo,
                       uint8_t genConfig) {
  uint8_t data[] = {SI446X_CMD_GPIO_PIN_CFG,
                    gpio0,
                    gpio1,
                    gpio2,
                    gpio3,
                    nirq,
                    sdo,
                    genConfig};
  doAPI(data, sizeof(data), NULL, 0);
}

/**
 * @brief Configure GPIO2=TX_STATE, GPIO3=RX_STATE for RF antenna switch.
 *
 * Convenience wrapper — matches the ASHINING drv_periph:
 *   SI446x_Config_Gpio(0, 0, 33|0x40, 32|0x40, 0, 0, 0)
 */
void Si446x_configRFswitch(void) {
  Si446x_configGPIO(0x00, 0x00,
                    SI446X_GPIO_MODE_TX_STATE | SI446X_GPIO_PULL_EN, // 0x61
                    SI446X_GPIO_MODE_RX_STATE | SI446X_GPIO_PULL_EN, // 0x60
                    SI446X_NIRQ_MODE_DONOTHING, SI446X_SDO_MODE_DONOTHING,
                    0x00);
}

// ==================================================================
// Direct TX API
// ==================================================================

void Si446x_startDirectTX(uint8_t channel, uint8_t async) {
  uint8_t irq = Si446x_irq_off();

  setState(IDLE_STATE);
  clearFIFO();
  interrupt2(NULL, 0, 0, 0xFF);

  // MODEM_MOD_TYPE[7] = TX_DIRECT_MODE_TYPE:
  //   0 = Synchronous (chip outputs TX bit clock on GPIO1)
  //   1 = Asynchronous (MCU controls data rate, no bit clock)
  uint8_t modType = getProperty(SI446X_MODEM_MOD_TYPE);
  if (async)
    modType |= SI446X_MODEM_MOD_TYPE_MASK_ASYNC;
  else
    modType &= SI446X_MODEM_MOD_TYPE_MASK_SYNC;
  setProperty(SI446X_MODEM_MOD_TYPE, modType);

  // START_TX — the FIFO/length fields are ignored by the chip when
  // MODEM_MOD_TYPE selects a direct data source.
  uint8_t data[] = {SI446X_CMD_START_TX,
                    channel,
                    0, // CONDITION — stay in TX
                    0,
                    0,
                    0,
                    0,
                    0};
  doAPI(data, sizeof(data), NULL, 0);

  Si446x_irq_on(irq);
}

void Si446x_txDirectWrite(const uint8_t *data, uint8_t len) {
  // Bit period for the configured data rate.
  // From radio_config.h: Rsymb(sps) = 1200, 2FSK → 1200 bps
  //   bitPeriod_us = 1_000_000 / 1200 ≈ 833 µs
  const uint32_t BIT_PERIOD_US = 833;

  // Si4463 reads GPIO0 on each bit clock.
  // Simple UART-style: start bit (low) + 8 data bits (LSB first)
  //                        + stop bit (high)
  for (uint8_t i = 0; i < len; i++) {
    uint8_t byte = data[i];

    // Start bit (low)
    digitalWrite(_pin_txData, LOW);
    delayMicroseconds(BIT_PERIOD_US);

    // 8 data bits, LSB first
    for (uint8_t b = 0; b < 8; b++) {
      digitalWrite(_pin_txData, (byte >> b) & 1);
      delayMicroseconds(BIT_PERIOD_US);
    }

    // Stop bit (high) — for receiver synchronisation
    digitalWrite(_pin_txData, HIGH);
    delayMicroseconds(BIT_PERIOD_US);
  }
}

void Si446x_stopTX(void) { setState(IDLE_STATE); }

// ==================================================================
// Frequency Control
// ==================================================================

/**
 * Band lookup table (30 MHz XTAL, Si4463 Rev B1).
 *
 * OUTDIV is chosen to keep the VCO-derived frequency within the
 * valid range.  The CLKGEN_BAND values are from WDS-generated configs.
 */
typedef struct {
  uint32_t minKHz;
  uint32_t maxKHz;
  uint8_t outdiv;
  uint8_t clkgenBand;
} freqBand_t;

static const freqBand_t FREQ_BANDS[] = {
    {0, 220000, 12, 0x0D}, // e.g. 156 MHz
    {220000, 295000, 8, 0x0C}, {295000, 390000, 6, 0x0B},
    {390000, 520000, 4, 0x0A}, // e.g. 425 MHz
    {520000, 700000, 3, 0x09}, {700000, 1050001, 2, 0x08},
};

void Si446x_setFrequency(uint8_t inte, uint32_t frac, uint16_t step,
                         uint8_t band) {
  // Stay in a safe state while changing frequency
  uint8_t irq = Si446x_irq_off();
  setState(SI446X_STATE_READY);

  setProperty(SI446X_MODEM_CLKGEN_BAND, band);

  uint8_t data[] = {
      (uint8_t)(step >> 8), // CHANNEL_STEP_SIZE_1 (0x4004)
      (uint8_t)(step),      // CHANNEL_STEP_SIZE_0 (0x4005)
  };
  setProperties(SI446X_FREQ_CONTROL_CHANNEL_STEP_SIZE, data, 2);

  data[0] = inte;                  // FREQ_CONTROL_INTE (0x4000)
  data[1] = (uint8_t)(frac >> 16); // FREQ_CONTROL_FRAC_2 (0x4001)
  data[2] = (uint8_t)(frac >> 8);  // FREQ_CONTROL_FRAC_1 (0x4002)
  data[3] = (uint8_t)(frac);       // FREQ_CONTROL_FRAC_0 (0x4003)
  setProperties(SI446X_FREQ_CONTROL_INTE, data, 4);

  // Perform image-rejection calibration for the new frequency
  uint8_t ircal = SI446X_CMD_IRCAL;
  doAPI(&ircal, 1, NULL, 0);

  Si446x_irq_on(irq);
}

uint8_t Si446x_setFrequencyKHz(uint32_t freqKHz) {
  // Find the band
  const freqBand_t *band = NULL;
  for (uint8_t i = 0; i < sizeof(FREQ_BANDS) / sizeof(FREQ_BANDS[0]); i++) {
    if (freqKHz > FREQ_BANDS[i].minKHz && freqKHz <= FREQ_BANDS[i].maxKHz) {
      band = &FREQ_BANDS[i];
      break;
    }
  }
  if (!band)
    return 0;

  // N = fRF × OUTDIV / fXO
  // fXO = 30 MHz, work in kHz
  // N × 2^19 in fixed-point for the fractional calculation
  const uint32_t fXO = 30000; // kHz
  const uint64_t N_fxp = ((uint64_t)freqKHz * band->outdiv << 19) / fXO;
  // N_fxp = N × 2^19  (in integer)
  // FC_FRAC should have bit 19 always set, so effective = (N - floor(N) + 1) ×
  // 2^19 FC_INTE = floor(N) - 1

  const uint32_t N_int = (uint32_t)(N_fxp >> 19); // floor(N)
  const uint32_t N_frac =
      (uint32_t)(N_fxp & ((1 << 19) - 1)); // fractional part × 2^19
  const uint8_t inte = (uint8_t)(N_int - 1);
  const uint32_t frac = N_frac | (1 << 19); // re-center so MSB = 1

  // Channel step size: default 2.5 kHz spacing
  // step_fxp = step_kHz × OUTDIV × 2^19 / fXO
  const uint32_t stepKHz = 2; // 2 kHz steps (adjustable)
  const uint16_t step =
      (uint16_t)(((uint64_t)stepKHz * band->outdiv << 19) / fXO);

  Si446x_setFrequency(inte, frac, step, band->clkgenBand);
  return 1;
}

void Si446x_getInfo(si446x_info_t *info) {
  uint8_t data[8] = {SI446X_CMD_PART_INFO};
  doAPI(data, 1, data, 8);

  info->chipRev = data[0];
  info->part = (data[1] << 8) | data[2];
  info->partBuild = data[3];
  info->id = (data[4] << 8) | data[5];
  info->customer = data[6];
  info->romId = data[7];

  data[0] = SI446X_CMD_FUNC_INFO;
  doAPI(data, 1, data, 6);

  info->revExternal = data[0];
  info->revBranch = data[1];
  info->revInternal = data[2];
  info->patch = (data[3] << 8) | data[4];
  info->func = data[5];
}

int16_t Si446x_getRSSI() {
  uint8_t data[3] = {SI446X_CMD_GET_MODEM_STATUS, 0xFF};
  doAPI(data, 2, data, 3);
  return rssi_dBm(data[2]);
}

si446x_state_t Si446x_getState() { return getState(); }

// ------------------------------------------------------------------
// Helper: derive OUTDIV from CLKGEN_BAND register value
// ------------------------------------------------------------------
static uint8_t outdivFromBand(uint8_t band) {
  for (uint8_t i = 0; i < sizeof(FREQ_BANDS) / sizeof(FREQ_BANDS[0]); i++) {
    if (FREQ_BANDS[i].clkgenBand == band)
      return FREQ_BANDS[i].outdiv;
  }
  return 0; // unknown
}

// ------------------------------------------------------------------
// Read back frequency from live registers
// ------------------------------------------------------------------
uint32_t Si446x_getFrequencyKHz(void) {
  uint8_t inte = getProperty(SI446X_FREQ_CONTROL_INTE);
  uint8_t fracBuf[3];
  getProperties(SI446X_FREQ_CONTROL_INTE + 1, fracBuf, 3);
  uint32_t frac =
      ((uint32_t)fracBuf[0] << 16) | ((uint32_t)fracBuf[1] << 8) | fracBuf[2];
  uint8_t band = getProperty(SI446X_MODEM_CLKGEN_BAND);

  uint8_t od = outdivFromBand(band);
  if (!od)
    return 0;

  // fRF = 30M × (FC_INTE + 1 + (FC_FRAC&0x7FFFF)/2^19) / OUTDIV
  // Keep all arithmetic in kHz / 64-bit to avoid float
  uint32_t fKHz = (uint32_t)((30000ULL * ((inte + 1ULL) * 524288ULL +
                                          (frac & 0x7FFFFULL))) /
                             (od * 524288ULL));
  return fKHz;
}

void Si446x_setTxPower(uint8_t pwr) { setProperty(SI446X_PA_PWR_LVL, pwr); }

uint8_t Si446x_getTxPower(void) { return getProperty(SI446X_PA_PWR_LVL); }

void Si446x_setLowBatt(uint16_t voltage) {
  uint8_t batt = (voltage / 50) - 30;
  setProperty(SI446X_GLOBAL_LOW_BATT_THRESH, batt);
}

void Si446x_setupWUT(uint8_t r, uint16_t m, uint8_t ldc, uint8_t config_flags) {
  if (!(config_flags & (SI446X_WUT_RUN | SI446X_WUT_BATT | SI446X_WUT_RX)))
    return;

  uint8_t irq = Si446x_irq_off();

  setProperty(SI446X_GLOBAL_WUT_CONFIG, 0);

  uint8_t doRun = !!(config_flags & SI446X_WUT_RUN);
  uint8_t doBatt = !!(config_flags & SI446X_WUT_BATT);
  uint8_t doRx = (config_flags & SI446X_WUT_RX);

  uint8_t intChip = 0;
  intChip |= doBatt << SI446X_INT_CTL_CHIP_LOW_BATT_EN;
  intChip |= doRun << SI446X_INT_CTL_CHIP_WUT_EN;
  enabledInterrupts[IRQ_CHIP] = intChip;
  setProperty(SI446X_INT_CTL_CHIP_ENABLE, intChip);

  if (getProperty(SI446X_GLOBAL_CLK_CFG) != SI446X_DIVIDED_CLK_32K_SEL_RC) {
    setProperty(SI446X_GLOBAL_CLK_CFG, SI446X_DIVIDED_CLK_32K_SEL_RC);
    delayMicroseconds(300);
  }

  uint8_t properties[5];
  properties[0] = doRx ? SI446X_GLOBAL_WUT_CONFIG_WUT_LDC_EN_RX : 0;
  properties[0] |= doBatt << SI446X_GLOBAL_WUT_CONFIG_WUT_LBD_EN;
  properties[0] |= (1 << SI446X_GLOBAL_WUT_CONFIG_WUT_EN);
  properties[1] = m >> 8;
  properties[2] = m;
  properties[3] = r | SI446X_LDC_MAX_PERIODS_TWO | (1 << SI446X_WUT_SLEEP);
  properties[4] = ldc;
  setProperties(SI446X_GLOBAL_WUT_CONFIG, properties, sizeof(properties));

  Si446x_irq_on(irq);
}

void Si446x_disableWUT() {
  uint8_t irq = Si446x_irq_off();
  setProperty(SI446X_GLOBAL_WUT_CONFIG, 0);
  setProperty(SI446X_GLOBAL_CLK_CFG, 0);
  Si446x_irq_on(irq);
}

void Si446x_setupCallback(uint16_t callbacks, uint8_t state) {
  uint8_t irq = Si446x_irq_off();
  uint8_t data[2];
  getProperties(SI446X_INT_CTL_PH_ENABLE, data, sizeof(data));

  if (state) {
    data[0] |= callbacks >> 8;
    data[1] |= callbacks;
  } else {
    data[0] &= ~(callbacks >> 8);
    data[1] &= ~callbacks;
  }

  enabledInterrupts[IRQ_PACKET] = data[0];
  enabledInterrupts[IRQ_MODEM] = data[1];
  setProperties(SI446X_INT_CTL_PH_ENABLE, data, sizeof(data));
  Si446x_irq_on(irq);
}

uint8_t Si446x_sleep() {
  if (getState() == SI446X_STATE_TX)
    return 0;
  setState(SI446X_STATE_SLEEP);
  return 1;
}

void Si446x_read(void *buff, uint8_t len) {
  SI446X_ATOMIC() {
    CHIPSELECT() {
      spi_transfer_nr(SI446X_CMD_READ_RX_FIFO);
      for (uint8_t i = 0; i < len; i++)
        ((uint8_t *)buff)[i] = spi_transfer(0xFF);
    }
  }
}

uint8_t Si446x_TX(void *packet, uint8_t len, uint8_t channel,
                  si446x_state_t onTxFinish) {
  uint8_t irq = Si446x_irq_off();

  if (getState() == SI446X_STATE_TX) {
    Si446x_irq_on(irq);
    return 0;
  }

  setState(IDLE_STATE);
  clearFIFO();
  interrupt2(NULL, 0, 0, 0xFF);

  SI446X_ATOMIC() {
    CHIPSELECT() {
      spi_transfer_nr(SI446X_CMD_WRITE_TX_FIFO);
#if !SI446X_FIXED_LENGTH
      spi_transfer_nr(len);
      for (uint8_t i = 0; i < len; i++)
        spi_transfer_nr(((uint8_t *)packet)[i]);
#else
      for (uint8_t i = 0; i < SI446X_FIXED_LENGTH; i++)
        spi_transfer_nr(((uint8_t *)packet)[i]);
#endif
    }
  }

#if !SI446X_FIXED_LENGTH
  setProperty(SI446X_PKT_FIELD_2_LENGTH_LOW, len);
#endif

  uint8_t data[] = {SI446X_CMD_START_TX,
                    channel,
                    (uint8_t)(onTxFinish << 4),
                    0,
                    SI446X_FIXED_LENGTH,
                    0,
                    0};
  doAPI(data, sizeof(data), NULL, 0);

#if !SI446X_FIXED_LENGTH
  setProperty(SI446X_PKT_FIELD_2_LENGTH_LOW, MAX_PACKET_LEN);
#endif

  Si446x_irq_on(irq);
  return 1;
}

void Si446x_RX(uint8_t channel) {
  uint8_t irq = Si446x_irq_off();

  setState(IDLE_STATE);
  clearFIFO();
  interrupt2(NULL, 0, 0, 0xFF);

  uint8_t data[] = {
      SI446X_CMD_START_RX,
      channel,
      0,
      0,
      SI446X_FIXED_LENGTH,
      SI446X_STATE_NOCHANGE, // RX Timeout
      IDLE_STATE,            // RX Valid
      SI446X_STATE_SLEEP     // RX Invalid
  };
  doAPI(data, sizeof(data), NULL, 0);

  Si446x_irq_on(irq);
}

uint16_t Si446x_adc_gpio(uint8_t pin) {
  return getADC(SI446X_ADC_CONV_GPIO | pin,
                (SI446X_ADC_SPEED << 4) | SI446X_ADC_RANGE_3P6, 0);
}

uint16_t Si446x_adc_battery() {
  uint16_t result = getADC(SI446X_ADC_CONV_BATT, (SI446X_ADC_SPEED << 4), 2);
  result = ((uint32_t)result * 75) / 32;
  return result;
}

float Si446x_adc_temperature() {
  float result = getADC(SI446X_ADC_CONV_TEMP, (SI446X_ADC_SPEED << 4), 4);
  result = (899 / 4096.0) * result - 293;
  return result;
}

void Si446x_writeGPIO(si446x_gpio_t pin, uint8_t value) {
  uint8_t data[] = {SI446X_CMD_GPIO_PIN_CFG,    SI446X_GPIO_MODE_DONOTHING,
                    SI446X_GPIO_MODE_DONOTHING, SI446X_GPIO_MODE_DONOTHING,
                    SI446X_GPIO_MODE_DONOTHING, SI446X_NIRQ_MODE_DONOTHING,
                    SI446X_SDO_MODE_DONOTHING,  0x00};
  data[pin + 1] = value;
  doAPI(data, sizeof(data), NULL, 0);
}

uint8_t Si446x_readGPIO() {
  uint8_t data[4] = {SI446X_CMD_GPIO_PIN_CFG};
  doAPI(data, 1, data, sizeof(data));
  uint8_t states = data[0] >> 7 | (data[1] & 0x80) >> 6 |
                   (data[2] & 0x80) >> 5 | (data[3] & 0x80) >> 4;
  return states;
}

uint8_t Si446x_dump(void *buff, uint8_t group) {
  static const uint8_t groupSizes[] = {
      SI446X_PROP_GROUP_GLOBAL, 0x0A, SI446X_PROP_GROUP_INT,          0x04,
      SI446X_PROP_GROUP_FRR,    0x04, SI446X_PROP_GROUP_PREAMBLE,     0x0E,
      SI446X_PROP_GROUP_SYNC,   0x06, SI446X_PROP_GROUP_PKT,          0x40,
      SI446X_PROP_GROUP_MODEM,  0x60, SI446X_PROP_GROUP_MODEM_CHFLT,  0x24,
      SI446X_PROP_GROUP_PA,     0x07, SI446X_PROP_GROUP_SYNTH,        0x08,
      SI446X_PROP_GROUP_MATCH,  0x0C, SI446X_PROP_GROUP_FREQ_CONTROL, 0x08,
      SI446X_PROP_GROUP_RX_HOP, 0x42, SI446X_PROP_GROUP_PTI,          0x04};

  uint8_t length = 0;
  for (uint8_t i = 0; i < sizeof(groupSizes); i += 2) {
    if (groupSizes[i] == group) {
      length = groupSizes[i + 1];
      break;
    }
  }

  if (buff == NULL)
    return length;

  for (uint8_t i = 0; i < length; i += 16) {
    uint8_t count = length - i;
    if (count > 16)
      count = 16;
    getProperties((group << 8) | i, ((uint8_t *)buff) + i, count);
  }

  return length;
}

// ------------------------------------------------------------------
// Interrupt control
// ------------------------------------------------------------------
uint8_t Si446x_irq_off() {
  if (_pin_irq < 0)
    return 0;
  detachInterrupt(digitalPinToInterrupt(_pin_irq));
  isrState = isrState + 1;
  return 0;
}

void Si446x_irq_on(uint8_t origVal) {
  (void)(origVal);
  if (_pin_irq < 0)
    return;
  if (isrState > 0)
    isrState = isrState - 1;
  if (isrState == 0)
    attachInterrupt(digitalPinToInterrupt(_pin_irq), Si446x_SERVICE, FALLING);
}

// ------------------------------------------------------------------
// Service routine — handles radio events
// ------------------------------------------------------------------
void Si446x_SERVICE() {
  isrBusy = 1;

  uint8_t interrupts[8];
  interrupt(interrupts);

  interrupts[2] &= enabledInterrupts[IRQ_PACKET];
  interrupts[4] &= enabledInterrupts[IRQ_MODEM];
  interrupts[6] &= enabledInterrupts[IRQ_CHIP];

  if (interrupts[4] & (1 << SI446X_SYNC_DETECT_PEND)) {
    SI446X_CB_RXBEGIN(getLatchedRSSI());
  }

  if (interrupts[2] & (1 << SI446X_PACKET_RX_PEND)) {
#if !SI446X_FIXED_LENGTH
    uint8_t len = 0;
    Si446x_read(&len, 1);
#else
    uint8_t len = SI446X_FIXED_LENGTH;
#endif
    SI446X_CB_RXCOMPLETE(len, getLatchedRSSI());
  }

  if (interrupts[2] & (1 << SI446X_CRC_ERROR_PEND)) {
#if IDLE_STATE == SI446X_STATE_READY
    if (getState() == SI446X_STATE_SPI_ACTIVE)
      setState(IDLE_STATE);
#endif
    SI446X_CB_RXINVALID(getLatchedRSSI());
  }

  if (interrupts[2] & (1 << SI446X_PACKET_SENT_PEND))
    SI446X_CB_SENT();

  if (interrupts[6] & (1 << SI446X_LOW_BATT_PEND))
    SI446X_CB_LOWBATT();

  if (interrupts[6] & (1 << SI446X_WUT_PEND))
    SI446X_CB_WUT();

  isrBusy = 0;
}
