#pragma once

#include <Arduino.h>
#include <SPI.h>

// ============================================================
// SX1262 COMMAND OPCODES
// ============================================================
#define SX126X_CMD_SET_STANDBY                   0x80
#define SX126X_CMD_SET_PACKET_TYPE               0x8A
#define SX126X_CMD_SET_RF_FREQUENCY              0x86
#define SX126X_CMD_SET_MOD_PARAMS                0x8B
#define SX126X_CMD_SET_PACKET_PARAMS             0x8C
#define SX126X_CMD_SET_BUFFER_BASE_ADDRESS       0x8F
#define SX126X_CMD_CLEAR_DEVICE_ERRORS           0x07

#define SX126X_CMD_SET_DIO_IRQ                   0x08
#define SX126X_CMD_CLEAR_IRQ                     0x02
#define SX126X_CMD_GET_IRQ                       0x12

#define SX126X_CMD_SET_RX_DUTY_CYCLE             0x94
#define SX126X_CMD_SET_DIO2_AS_RF_SWITCH         0x9D
#define SX126X_CMD_SET_STOP_RX_TIMER_ON_PREAMBLE 0x9F
#define SX126X_CMD_SET_REGULATOR_MODE            0x96
#define SX126X_CMD_WRITE_REGISTER                0x0D

// Registers
#define REG_XTAL_TRIM_A                          0x0911
#define REG_XTAL_TRIM_B                          0x0912
#define REG_RX_GAIN                              0x08AC
#define REG_SYNC_WORD_MSB                        0x0740
#define REG_SYNC_WORD_LSB                        0x0741

// ============================================================
// SX1262 IRQ MASKS
// ============================================================

#define IRQ_TX_DONE              0x0001
#define IRQ_RX_DONE              0x0002
#define IRQ_PREAMBLE_DETECTED    0x0004
#define IRQ_SYNCWORD_VALID       0x0008
#define IRQ_HEADER_VALID         0x0010
#define IRQ_HEADER_ERROR         0x0020
#define IRQ_CRC_ERROR            0x0040
#define IRQ_CAD_DONE             0x0080
#define IRQ_CAD_DETECTED         0x0100
#define IRQ_TIMEOUT              0x0200

// ============================================================
// LoRa SETTINGS
// ============================================================
#define LORA_FREQ_HZ       915000000UL
#define LORA_SF            7
#define LORA_BW            4       // 125 kHz
#define LORA_CR            1       // 4/5
#define LORA_PREAMBLE      12

// ============================================================
// Extended RxDutyCycle Timing for Event-Driven WOR
// RTC tick = 15.625 us
//
// RX period    = 2048 ticks (~32.0 ms active listen)
// Sleep period = 5120 ticks (~80.0 ms deep sleep)
// Full cycle   ~= 112.0 ms
// ============================================================

#define RXDC_RX_TICKS       2048UL  
#define RXDC_SLEEP_TICKS    5120UL  

// ============================================================
// SPI & LOW-LEVEL BUS
// ============================================================

inline SPIClass radioSPI(FSPI); 

// --- SINGLE UNIFIED WAIT BUSY ---
inline bool sxWaitBusy(uint32_t timeoutMs = 500) {
  uint32_t start = millis();
  while (digitalRead(RADIO_BUSY_PIN) == HIGH) {
    if (millis() - start > timeoutMs) {
      return false; // Timed out
    }
    delayMicroseconds(10);
  }
  return true;
}

inline void sxWakeupSPI() {
  digitalWrite(RADIO_CS_PIN, LOW);
  delayMicroseconds(100);
  digitalWrite(RADIO_CS_PIN, HIGH);
  sxWaitBusy();
}

inline void sxCommand(uint8_t opcode, const uint8_t *data, size_t len) {
  sxWaitBusy();

  digitalWrite(RADIO_CS_PIN, LOW);
  radioSPI.beginTransaction(SPISettings(8000000, MSBFIRST, SPI_MODE0));

  radioSPI.transfer(opcode);
  for (size_t i = 0; i < len; i++) {
    radioSPI.transfer(data[i]);
  }

  radioSPI.endTransaction();
  digitalWrite(RADIO_CS_PIN, HIGH);

  sxWaitBusy();
}

inline void sxCommand(uint8_t opcode) {
  sxCommand(opcode, nullptr, 0);
}

inline void sxReadCommand(uint8_t opcode, uint8_t *data, size_t len) {
  sxWaitBusy();

  digitalWrite(RADIO_CS_PIN, LOW);
  radioSPI.beginTransaction(SPISettings(8000000, MSBFIRST, SPI_MODE0));

  radioSPI.transfer(opcode);
  radioSPI.transfer(0x00);   // Status/NOP byte

  for (size_t i = 0; i < len; i++) {
    data[i] = radioSPI.transfer(0x00);
  }

  radioSPI.endTransaction();
  digitalWrite(RADIO_CS_PIN, HIGH);
}

inline void sxWriteRegister(uint16_t address, uint8_t value) {
  uint8_t data[3];
  data[0] = (address >> 8) & 0xFF;
  data[1] = address & 0xFF;
  data[2] = value;
  sxCommand(SX126X_CMD_WRITE_REGISTER, data, 3);
}

// ============================================================
// LOW-LEVEL HELPER FUNCTIONS
// ============================================================

inline void sxClearDeviceErrors() {
  uint8_t data[2] = { 0x00, 0x00 };
  sxCommand(SX126X_CMD_CLEAR_DEVICE_ERRORS, data, 2);
}

inline void sxClearIrq() {
  uint8_t data[2] = { 0xFF, 0xFF };
  sxCommand(SX126X_CMD_CLEAR_IRQ, data, 2);
}

inline void sxReset() {
  Serial.println("SX1262 reset...");
  digitalWrite(RADIO_RST_PIN, LOW);
  delay(10);
  digitalWrite(RADIO_RST_PIN, HIGH);
  delay(20);
  sxWaitBusy();
  Serial.println("SX1262 reset complete.");
}

inline void sxSetRegulatorModeLDO() {
  uint8_t data[1] = { 0x00 }; // 0x00 = USE_LDO
  sxCommand(SX126X_CMD_SET_REGULATOR_MODE, data, 1);
}

inline void sxSetXtalCapacitance(uint8_t trimA, uint8_t trimB) {
  sxWriteRegister(REG_XTAL_TRIM_A, trimA); 
  sxWriteRegister(REG_XTAL_TRIM_B, trimB);
}

inline void sxStandby() {
  uint8_t data[] = { 0x00 };       // STDBY_RC
  sxCommand(SX126X_CMD_SET_STANDBY, data, sizeof(data));
}

inline void sxSetPacketTypeLoRa() {
  uint8_t data[] = { 0x01 };
  sxCommand(SX126X_CMD_SET_PACKET_TYPE, data, sizeof(data));
}

inline void sxSetFrequency(uint32_t freqHz) {
  uint32_t steps = (uint32_t)((double)freqHz / (32000000.0 / 33554432.0));
  uint8_t data[4];
  data[0] = (uint8_t)(steps >> 24);
  data[1] = (uint8_t)(steps >> 16);
  data[2] = (uint8_t)(steps >> 8);
  data[3] = (uint8_t)(steps);
  sxCommand(SX126X_CMD_SET_RF_FREQUENCY, data, 4);
}

inline void sxSetLoRaModulation() {
  uint8_t data[4] = { LORA_SF, 0x04, LORA_CR, 0x00 };
  sxCommand(SX126X_CMD_SET_MOD_PARAMS, data, 4);
}

inline void sxSetPacketParams() {
  uint8_t data[6];
  data[0] = (LORA_PREAMBLE >> 8) & 0xFF;
  data[1] = LORA_PREAMBLE & 0xFF;
  data[2] = 0x00; // Explicit header
  data[3] = 0xFF; // Max payload
  data[4] = 0x01; // CRC ON
  data[5] = 0x00; // Normal IQ
  sxCommand(SX126X_CMD_SET_PACKET_PARAMS, data, 6);
}

inline void sxSetBufferBaseAddress(uint8_t txBase, uint8_t rxBase) {
  uint8_t data[2] = { txBase, rxBase };
  sxCommand(SX126X_CMD_SET_BUFFER_BASE_ADDRESS, data, 2);
}

inline void sxSetDio2AsRfSwitch() {
  uint8_t data[] = { 0x01 };
  sxCommand(SX126X_CMD_SET_DIO2_AS_RF_SWITCH, data, 1);
}

// ============================================================
// IRQ & RX DUTY CYCLE (WOR)
// ============================================================

// --- GET IRQ FUNCTION ---
// Semtech GetIrqStatus (0x12): opcode, 1 status byte, 2 IRQ bytes (MSB, LSB).
// MUST use radioSPI (the FSPI bus actually wired to the radio via
// radioSPI.begin(RADIO_SCLK_PIN, RADIO_MISO_PIN, RADIO_MOSI_PIN, RADIO_CS_PIN)
// in setup()) and the same beginTransaction/SPISettings framing as every
// other command in this file. The previous version called the bare
// default `SPI` object, which is never begin()'d anywhere in this sketch
// -- it clocks an unwired peripheral, so it always read back 0x0000
// regardless of BUSY/DIO1 timing.
inline uint16_t sxGetIrq() {
  uint8_t raw[2] = {0, 0};
  sxReadCommand(SX126X_CMD_GET_IRQ, raw, 2);
  sxWaitBusy();
  return ((uint16_t)raw[0] << 8) | raw[1];
}

inline void sxSetStopRxTimerOnPreamble(bool stopOnPreamble) {
  uint8_t param = stopOnPreamble ? 0x00 : 0x01; // 0x00 = Stop on Preamble Detect
  sxCommand(SX126X_CMD_SET_STOP_RX_TIMER_ON_PREAMBLE, &param, 1);
}

inline void sxConfigureRxDutyCycleIrq() {
  // Enable internal tracking for RxDone, HeaderValid, PreambleDetected, CRC, and Timeout
  uint16_t irqMask = IRQ_RX_DONE | IRQ_HEADER_VALID | IRQ_PREAMBLE_DETECTED | IRQ_CRC_ERROR | IRQ_TIMEOUT;
  
  // DIO1 Mask: Include PREAMBLE_DETECTED (0x0004) so DIO1 fires immediately on preamble!
  uint16_t dio1Mask = IRQ_RX_DONE | IRQ_HEADER_VALID | IRQ_PREAMBLE_DETECTED; 

  uint8_t buffer[8];
  buffer[0] = (uint8_t)(irqMask >> 8);
  buffer[1] = (uint8_t)(irqMask & 0xFF);
  buffer[2] = (uint8_t)(dio1Mask >> 8);
  buffer[3] = (uint8_t)(dio1Mask & 0xFF);
  buffer[4] = 0x00; buffer[5] = 0x00; // DIO2
  buffer[6] = 0x00; buffer[7] = 0x00; // DIO3

  sxCommand(SX126X_CMD_SET_DIO_IRQ, buffer, 8);
}
inline void sxSetSyncWordPrivate() {
  sxWriteRegister(REG_SYNC_WORD_MSB, 0x00);
  sxWriteRegister(REG_SYNC_WORD_LSB, 0x12);
}

inline void sxSetRxDutyCycle(uint32_t rxTicks, uint32_t sleepTicks) {
  uint8_t data[6];

  data[0] = (rxTicks >> 16) & 0xFF;
  data[1] = (rxTicks >> 8)  & 0xFF;
  data[2] =  rxTicks        & 0xFF;

  data[3] = (sleepTicks >> 16) & 0xFF;
  data[4] = (sleepTicks >> 8)  & 0xFF;
  data[5] =  sleepTicks        & 0xFF;

  sxCommand(SX126X_CMD_SET_RX_DUTY_CYCLE, data, 6);
}

// ============================================================
// TOP-LEVEL INITIALIZATION ROUTINE (MUST BE AT BOTTOM)
// ============================================================

inline void initRadio() {
  sxReset();
  
  // 1. Force LDO Mode
  uint8_t ldoData[1] = { 0x00 }; 
  sxCommand(SX126X_CMD_SET_REGULATOR_MODE, ldoData, 1);

  // 2. Crystal Trim
  sxWriteRegister(REG_XTAL_TRIM_A, 0x12);
  sxWriteRegister(REG_XTAL_TRIM_B, 0x12);

  // 3. Switch to STDBY_XOSC
  uint8_t xoscData[1] = { 0x01 }; 
  sxCommand(SX126X_CMD_SET_STANDBY, xoscData, 1);
  delay(10); // CRITICAL: Allow 32 MHz crystal oscillator time to stabilize!

  // 4. Calibrate RF Image for 915 MHz Band
  uint8_t calData[2] = { 0xE1, 0xE9 }; 
  sxCommand(0x98, calData, 2);

  // 5. Antenna Switch
  sxSetDio2AsRfSwitch();

  // 6. Configure LoRa Modem
  sxSetPacketTypeLoRa();
  sxSetFrequency(LORA_FREQ_HZ);
  sxSetLoRaModulation();
  sxSetPacketParams();
  sxSetSyncWordPrivate();

  // 7. Configure WOR IRQs
  sxSetStopRxTimerOnPreamble(true);
  sxConfigureRxDutyCycleIrq();
  
  // 8. Clear startup IRQs
  sxClearIrq();
}