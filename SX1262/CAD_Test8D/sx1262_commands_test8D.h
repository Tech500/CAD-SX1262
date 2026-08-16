// ============================================================
// sx1262_commands_test8D.h
//
// EoRa-S3-900TB
// SX1262 LOW-LEVEL REPEATED CAD TEST - TEST 8D
//
// PURPOSE:
//   Validate repeated SX1262 CAD operation against a known-good
//   915 MHz LoRa WOR transmitter.
//
//   NO:
//     RadioLib
//     deep sleep
//     EXT0
//     RX duty cycle
//
// TEST PATH:
//
//   SX1262
//      |
//      +-- repeated CAD
//              |
//              +-- CAD_DONE
//              |
//              +-- CAD_DETECTED
//              |
//              +-- DIO1 / GPIO16
//
// ============================================================

#pragma once

#include <Arduino.h>
#include <SPI.h>


// ============================================================
// SX1262 COMMANDS
// ============================================================

#define SX126X_CMD_SET_STANDBY           0x80
#define SX126X_CMD_SET_PACKET_TYPE       0x8A
#define SX126X_CMD_SET_RF_FREQUENCY      0x86
#define SX126X_CMD_SET_MOD_PARAMS        0x8B
#define SX126X_CMD_SET_PACKET_PARAMS     0x8C

#define SX126X_CMD_SET_DIO_IRQ           0x08
#define SX126X_CMD_CLEAR_IRQ             0x02
#define SX126X_CMD_GET_IRQ               0x12

#define SX126X_CMD_SET_CAD_PARAMS        0x88
#define SX126X_CMD_SET_CAD              0xC5

#define SX126X_CMD_SET_DIO2_AS_RF_SWITCH 0x9D


// ============================================================
// IRQ MASKS
// ============================================================

#define IRQ_RX_DONE             0x0002
#define IRQ_PREAMBLE_DETECTED   0x0010
#define IRQ_CAD_DONE            0x0080
#define IRQ_CAD_DETECTED        0x0100

#define IRQ_CAD_ALL             (IRQ_CAD_DONE | IRQ_CAD_DETECTED)


// ============================================================
// LoRa SETTINGS
//
// MUST MATCH TRANSMITTER
// ============================================================

#define LORA_FREQ_HZ       915000000UL
#define LORA_SF            7
#define LORA_BW            7       // 125 kHz
#define LORA_CR            1       // 4/5

// This is the receiver packet configuration.
// It does NOT need to be 5000.
// The transmitter is producing the 5000-symbol WOR preamble.
#define LORA_PREAMBLE      12


// ============================================================
// SPI
// ============================================================

SPIClass radioSPI(FSPI);


// ============================================================
// BUSY WAIT
// ============================================================

void sxWaitBusy()
{
  uint32_t start = millis();

  while (digitalRead(RADIO_BUSY_PIN) == HIGH)
  {
    if ((millis() - start) > 500)
    {
      Serial.println("[ERROR] SX1262 BUSY Timeout!");
      break;
    }

    yield();
  }
}


// ============================================================
// WRITE COMMAND
// ============================================================

inline void sxCommand(
    uint8_t opcode,
    const uint8_t *data,
    size_t len)
{
  sxWaitBusy();

  digitalWrite(RADIO_CS_PIN, LOW);

  radioSPI.beginTransaction(
      SPISettings(8000000, MSBFIRST, SPI_MODE0));

  radioSPI.transfer(opcode);

  for (size_t i = 0; i < len; i++)
  {
    radioSPI.transfer(data[i]);
  }

  radioSPI.endTransaction();

  digitalWrite(RADIO_CS_PIN, HIGH);

  sxWaitBusy();
}


// ============================================================
// WRITE COMMAND - NO DATA
// ============================================================

inline void sxCommand(uint8_t opcode)
{
  sxCommand(opcode, nullptr, 0);
}


// ============================================================
// READ COMMAND
// ============================================================

inline void sxReadCommand(
    uint8_t opcode,
    uint8_t *data,
    size_t len)
{
  sxWaitBusy();

  digitalWrite(RADIO_CS_PIN, LOW);

  radioSPI.beginTransaction(
      SPISettings(8000000, MSBFIRST, SPI_MODE0));

  radioSPI.transfer(opcode);

  // Status byte
  radioSPI.transfer(0x00);

  for (size_t i = 0; i < len; i++)
  {
    data[i] = radioSPI.transfer(0x00);
  }

  radioSPI.endTransaction();

  digitalWrite(RADIO_CS_PIN, HIGH);
}


// ============================================================
// RESET
// ============================================================

void sxReset()
{
  Serial.println("SX1262 reset...");

  digitalWrite(RADIO_RST_PIN, LOW);
  delay(10);

  digitalWrite(RADIO_RST_PIN, HIGH);
  delay(20);

  sxWaitBusy();

  Serial.println("SX1262 reset complete.");
}


// ============================================================
// STANDBY
// ============================================================

void sxStandby()
{
  uint8_t data[] = { 0x00 };       // STDBY_RC

  sxCommand(
      SX126X_CMD_SET_STANDBY,
      data,
      sizeof(data));
}


// ============================================================
// PACKET TYPE = LORA
// ============================================================

void sxSetPacketTypeLoRa()
{
  uint8_t data[] = { 0x01 };

  sxCommand(
      SX126X_CMD_SET_PACKET_TYPE,
      data,
      sizeof(data));
}


// ============================================================
// FREQUENCY
// ============================================================

void sxSetFrequency(uint32_t freqHz)
{
  uint32_t steps =
      (uint32_t)(
        (double)freqHz /
        (32000000.0 / 33554432.0)
      );

  uint8_t data[4];

  data[0] = (uint8_t)(steps >> 24);
  data[1] = (uint8_t)(steps >> 16);
  data[2] = (uint8_t)(steps >> 8);
  data[3] = (uint8_t)(steps);

  sxCommand(
      SX126X_CMD_SET_RF_FREQUENCY,
      data,
      4);
}


// ============================================================
// LORA MODULATION PARAMETERS
// ============================================================

void sxSetLoRaModulation()
{
  uint8_t data[4];

  data[0] = LORA_SF;
  data[1] = 0x70;       // BW 125 kHz
  data[2] = LORA_CR;
  data[3] = 0x00;       // LDRO OFF

  sxCommand(
      SX126X_CMD_SET_MOD_PARAMS,
      data,
      4);
}


// ============================================================
// PACKET PARAMETERS
//
// NOTE:
// This MUST be named sxSetPacketParams() because the Test 8D
// sketch calls this function.
//
// ============================================================

void sxSetPacketParams()
{
  uint8_t data[6];

  // Preamble length
  data[0] = (LORA_PREAMBLE >> 8) & 0xFF;
  data[1] = LORA_PREAMBLE & 0xFF;

  // Explicit header
  data[2] = 0x00;

  // Maximum payload
  data[3] = 0xFF;

  // CRC ON
  data[4] = 0x01;

  // Normal IQ
  data[5] = 0x00;

  sxCommand(
      SX126X_CMD_SET_PACKET_PARAMS,
      data,
      6);
}


// ============================================================
// DIO2 AS RF SWITCH
// ============================================================

void sxSetDio2AsRfSwitch()
{
  uint8_t data[] = { 0x01 };

  sxCommand(
      SX126X_CMD_SET_DIO2_AS_RF_SWITCH,
      data,
      1);
}


// ============================================================
// CAD PARAMETERS
//
// 8 CAD symbols
// Peak threshold = 0x16
// Minimum threshold = 0x0A
// Exit mode = CAD_ONLY
//
// CAD_ONLY is intentional.
// We want repeated independent CAD scans.
//
// ============================================================

void sxSetCadParams()
{
  uint8_t data[7] =
  {
    0x03,       // 8 CAD symbols
    0x16,       // CAD peak threshold
    0x0A,       // CAD minimum threshold
    0x00,       // CAD_ONLY
    0x00,       // timeout MSB
    0x00,
    0x00
  };

  sxCommand(
      SX126X_CMD_SET_CAD_PARAMS,
      data,
      7);
}


// ============================================================
// CAD IRQ ROUTING
//
// GLOBAL:
//   CAD_DONE
//   CAD_DETECTED
//
// DIO1:
//   CAD_DETECTED ONLY
//
// Thus:
//
//   CAD_DONE       -> IRQ STATUS only
//   CAD_DETECTED   -> IRQ STATUS + DIO1/GPIO16
//
// ============================================================

void sxConfigureCadIrq()
{
  uint8_t data[8];

  uint16_t globalMask =
      IRQ_CAD_DONE |
      IRQ_CAD_DETECTED;

  uint16_t dio1Mask =
      IRQ_CAD_DETECTED;

  // Global IRQ mask
  data[0] = globalMask >> 8;
  data[1] = globalMask & 0xFF;

  // DIO1
  data[2] = dio1Mask >> 8;
  data[3] = dio1Mask & 0xFF;

  // DIO2
  data[4] = 0x00;
  data[5] = 0x00;

  // DIO3
  data[6] = 0x00;
  data[7] = 0x00;

  sxCommand(
      SX126X_CMD_SET_DIO_IRQ,
      data,
      8);
}


// ============================================================
// CLEAR IRQ
// ============================================================

void sxClearIrq()
{
  uint8_t data[2] =
  {
    0xFF,
    0xFF
  };

  sxCommand(
      SX126X_CMD_CLEAR_IRQ,
      data,
      2);
}


// ============================================================
// READ IRQ
// ============================================================

uint16_t sxGetIrq()
{
  uint8_t data[2];

  sxReadCommand(
      SX126X_CMD_GET_IRQ,
      data,
      2);

  return
      ((uint16_t)data[0] << 8) |
      data[1];
}


// ============================================================
// START ONE CAD SCAN
// ============================================================

void sxStartCad()
{
  sxCommand(SX126X_CMD_SET_CAD);
}