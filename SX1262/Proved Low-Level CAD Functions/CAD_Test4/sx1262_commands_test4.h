// ============================================================
// sx1262_commands.h
//
// EoRa-S3-900TB
// SX1262 LOW-LEVEL CAD VALIDATION
//
// PURPOSE:
//   Prove the SX1262 hardware CAD path independently of:
//     - RadioLib
//     - ESP32 deep sleep
//     - EXT0
//     - WOR
//     - RX duty cycle
//
// TEST PATH:
//
//   SX1262
//      |
//      +-- CAD
//      |
//      +-- CAD_DONE
//      |
//      +-- DIO1
//      |
//      +-- ESP32 GPIO16
//
// For this test ONLY:
//
//   CAD_DONE -> DIO1
//
// CAD_DETECTED and PREAMBLE_DETECTED are intentionally NOT
// routed to DIO1 yet.
// ============================================================


// ============================================================
// SX1262 COMMANDS
// ============================================================

#define SX126X_CMD_SET_STANDBY          0x80
#define SX126X_CMD_SET_PACKET_TYPE      0x8A
#define SX126X_CMD_SET_RF_FREQUENCY     0x86
#define SX126X_CMD_SET_MOD_PARAMS       0x8B
#define SX126X_CMD_SET_PACKET_PARAMS    0x8C

#define SX126X_CMD_SET_DIO_IRQ          0x08
#define SX126X_CMD_CLEAR_IRQ             0x02
#define SX126X_CMD_GET_IRQ               0x12

#define SX126X_CMD_SET_CAD_PARAMS       0x88
#define SX126X_CMD_SET_CAD              0xC5

#define SX126X_CMD_SET_RX_DUTY_CYCLE    0x94

#define SX126X_CMD_WRITE_REGISTER        0x0D
#define SX126X_CMD_READ_REGISTER         0x1D

#define SX126X_CMD_SET_DIO2_AS_RF_SWITCH 0x9D

#define SX126X_CMD_SET_REGULATOR_MODE    0x96


// ============================================================
// REGISTERS
// ============================================================

#define REG_XTAL_TRIM_A                  0x0911
#define REG_XTAL_TRIM_B                  0x0912
#define REG_RX_GAIN                      0x08AC


// ============================================================
// SPI
// ============================================================

SPIClass radioSPI(FSPI);


// ============================================================
// SX1262 IRQ MASKS
//
// SX1262 IRQ STATUS:
//
// RX_DONE              = 0x0002
// PREAMBLE_DETECTED    = 0x0010
// CAD_DONE             = 0x0080
// CAD_DETECTED         = 0x0100
// ============================================================

#define IRQ_RX_DONE             0x0002
#define IRQ_PREAMBLE_DETECTED   0x0010

#define IRQ_CAD_DONE            0x0080
#define IRQ_CAD_DETECTED        0x0100

#define IRQ_CAD_ALL             \
    (IRQ_CAD_DONE | IRQ_CAD_DETECTED)


// ============================================================
// LoRa SETTINGS
// Must match known-good RadioLib transmitter.
// ============================================================

#define LORA_FREQ_HZ       915000000UL
#define LORA_SF            7
#define LORA_BW            7       // 125 kHz
#define LORA_CR            1       // 4/5
#define LORA_PREAMBLE      12


// ============================================================
// LOW-LEVEL SPI FUNCTIONS
// ============================================================

void sxWaitBusy()
{
  uint32_t start = millis();

  while (digitalRead(RADIO_BUSY_PIN) == HIGH)
  {
    if (millis() - start > 500)
    {
      Serial.println(F("[ERROR] SX1262 BUSY Timeout!"));
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
//
// SX1262 read-command format:
//
//   Opcode
//   Status dummy
//   Data...
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
// WRITE REGISTER
// ============================================================

inline void sxWriteRegister(
    uint16_t address,
    uint8_t value)
{
  uint8_t data[3];

  data[0] = (address >> 8) & 0xFF;
  data[1] = address & 0xFF;
  data[2] = value;

  sxCommand(
      SX126X_CMD_WRITE_REGISTER,
      data,
      3);
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
  uint8_t data[] = { 0x00 };     // STDBY_RC

  sxCommand(
      SX126X_CMD_SET_STANDBY,
      data,
      1);
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
// RF FREQUENCY
// ============================================================

inline void sxSetFrequency(uint32_t freqHz)
{
  uint32_t steps =
      (uint32_t)(
        (double)freqHz /
        (32000000.0 / 33554432.0)
      );

  uint8_t data[4] =
  {
    (uint8_t)((steps >> 24) & 0xFF),
    (uint8_t)((steps >> 16) & 0xFF),
    (uint8_t)((steps >> 8)  & 0xFF),
    (uint8_t)(steps & 0xFF)
  };

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
  data[1] = 0x70;       // 125 kHz
  data[2] = LORA_CR;
  data[3] = 0x00;       // LDRO OFF

  sxCommand(
      SX126X_CMD_SET_MOD_PARAMS,
      data,
      4);
}


// ============================================================
// DIO2 AS RF SWITCH
// ============================================================

inline void sxSetDio2AsRfSwitch()
{
  uint8_t data[] = { 0x01 };

  sxCommand(
      SX126X_CMD_SET_DIO2_AS_RF_SWITCH,
      data,
      1);
}


// ============================================================
// LORA PACKET PARAMETERS
// ============================================================

void sxSetLoRaPacket()
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
// CAD PARAMETERS
//
// CAD symbols       = 8
// Peak threshold    = 0x16
// Min threshold     = 0x0A
// Exit mode         = CAD_ONLY
// Timeout           = 0
//
// CAD_ONLY is intentional.
// We do NOT enter RX after CAD_DETECTED.
// ============================================================

inline void sxSetCadParams()
{
  uint8_t cadParams[7] =
  {
    0x03,       // 8 CAD symbols
    0x16,       // CAD peak threshold
    0x0A,       // CAD minimum threshold
    0x00,       // CAD_ONLY
    0x00,       // CAD timeout MSB
    0x00,
    0x00
  };

  sxCommand(
      SX126X_CMD_SET_CAD_PARAMS,
      cadParams,
      7);
}


// ============================================================
// CAD IRQ ROUTING
//
// FIRST HARDWARE VALIDATION:
//
// ONLY CAD_DONE is routed to DIO1.
//
// Global IRQ:
//      CAD_DONE = 0x0080
//
// DIO1:
//      CAD_DONE = 0x0080
//
// DIO2:
//      NONE
//
// DIO3:
//      NONE
//
// This deliberately excludes:
//
//      CAD_DETECTED
//      PREAMBLE_DETECTED
//      RX_DONE
// ============================================================

void sxConfigureCadIrq()
{
  uint8_t data[8];

  // ONLY CAD_DONE for this validation
  uint16_t irqMask = IRQ_CAD_DONE;

  // ----------------------------------------------------------
  // Global IRQ mask
  // ----------------------------------------------------------

  data[0] = (uint8_t)(irqMask >> 8);
  data[1] = (uint8_t)(irqMask & 0xFF);

  // ----------------------------------------------------------
  // DIO1 IRQ mask
  // ----------------------------------------------------------

  data[2] = (uint8_t)(irqMask >> 8);
  data[3] = (uint8_t)(irqMask & 0xFF);

  // ----------------------------------------------------------
  // DIO2
  // ----------------------------------------------------------

  data[4] = 0x00;
  data[5] = 0x00;

  // ----------------------------------------------------------
  // DIO3
  // ----------------------------------------------------------

  data[6] = 0x00;
  data[7] = 0x00;

  sxCommand(
      SX126X_CMD_SET_DIO_IRQ,
      data,
      8);
}

// ============================================================
// CLEAR ALL IRQs
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
// READ IRQ STATUS
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
// START CAD
//
// Diagnostic version.
//
// We inspect IRQ state immediately before and after the
// SetCAD command.
// ============================================================

void sxStartCad()
{
  Serial.println("Starting SX1262 CAD...");

  // ----------------------------------------------------------
  // BUSY before CAD
  // ----------------------------------------------------------

  Serial.printf(
      "BUSY before CAD = %d\n",
      digitalRead(RADIO_BUSY_PIN));

  // ----------------------------------------------------------
  // IRQ before CAD
  // ----------------------------------------------------------

  uint16_t irqBefore = sxGetIrq();

  Serial.printf(
      "IRQ before CAD = 0x%04X\n",
      irqBefore);

  // ----------------------------------------------------------
  // START CAD
  // ----------------------------------------------------------

  sxCommand(
      SX126X_CMD_SET_CAD);

  // ----------------------------------------------------------
  // BUSY immediately after command
  // ----------------------------------------------------------

  Serial.printf(
      "BUSY after CAD command = %d\n",
      digitalRead(RADIO_BUSY_PIN));

  // ----------------------------------------------------------
  // IRQ immediately after command
  // ----------------------------------------------------------

  uint16_t irqAfter = sxGetIrq();

  Serial.printf(
      "IRQ immediately after CAD = 0x%04X\n",
      irqAfter);
}


// ============================================================
// OPTIONAL RX DUTY CYCLE  CAD Test4                sx1262_commands_test3.h
//
// NOT USED BY THE CURRENT CAD VALIDATION TEST.
//
// Retained here only so existing sketches that reference the
// function will still compile.
// ============================================================

inline void sxRXDutyCycle()
{
  uint8_t dutyData[6] =
  {
    0x00, 0x08, 0x00,
    0x00, 0xFA, 0x00
  };

  sxCommand(
      SX126X_CMD_SET_RX_DUTY_CYCLE,
      dutyData,
      6);
}


// ============================================================
// END sx1262_commands.h
// ============================================================