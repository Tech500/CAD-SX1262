// ============================================================
// SX1262 COMMANDS
// ============================================================

#define SX126X_CMD_SET_STANDBY        0x80
#define SX126X_CMD_SET_PACKET_TYPE    0x8A
#define SX126X_CMD_SET_RF_FREQUENCY   0x86
#define SX126X_CMD_SET_MOD_PARAMS     0x8B
#define SX126X_CMD_SET_PACKET_PARAMS  0x8C
#define SX126X_CMD_SET_DIO_IRQ        0x08
#define SX126X_CMD_CLEAR_IRQ          0x02
#define SX126X_CMD_GET_IRQ            0x12
#define SX126X_CMD_SET_CAD_PARAMS     0x88
#define SX126X_CMD_SET_CAD            0xC5
#define SX126X_CMD_SET_RX_DUTY_CYCLE  0x94

SPIClass radioSPI(FSPI);

// ============================================================
// SX1262 IRQ MASKS
// ============================================================

#define IRQ_RX_DONE             0x0002
#define IRQ_PREAMBLE_DETECTED   0x0004

#define IRQ_CAD_DONE            0x0080
#define IRQ_CAD_DETECTED        0x0100

#define IRQ_CAD_ALL             (IRQ_CAD_DONE | IRQ_CAD_DETECTED)

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
// LOW LEVEL SPI
// ============================================================

void waitBusy()
{
  while (digitalRead(RADIO_BUSY_PIN) == HIGH)
  {
    delayMicroseconds(10);
  }
}

// ============================================================
// SX1262 COMMAND WRITE
// ============================================================

void sxCommand(uint8_t opcode,
               const uint8_t *data,
               size_t len)
{
  waitBusy();

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

  waitBusy();
}

// ============================================================
// SX1262 COMMAND WRITE - NO DATA
// ============================================================

void sxCommand(uint8_t opcode)
{
  sxCommand(opcode, nullptr, 0);
}

// ============================================================
// SX1262 COMMAND READ
// ============================================================

void sxReadCommand(uint8_t opcode,
                   uint8_t *data,
                   size_t len)
{
  waitBusy();

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

  waitBusy();

  Serial.println("SX1262 reset complete.");
}

// ============================================================
// STANDBY
// ============================================================

void sxStandby()
{
  uint8_t data[] = {0x00};       // STDBY_RC

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
  uint8_t data[] = {0x01};

  sxCommand(
      SX126X_CMD_SET_PACKET_TYPE,
      data,
      sizeof(data));
}

// ============================================================
// RF FREQUENCY
// ============================================================

void sxSetFrequency(uint32_t frequency)
{
  uint32_t frf =
      ((uint64_t)frequency << 25) /
      32000000ULL;

  uint8_t data[4];

  data[0] = (frf >> 24) & 0xFF;
  data[1] = (frf >> 16) & 0xFF;
  data[2] = (frf >> 8)  & 0xFF;
  data[3] = frf & 0xFF;

  sxCommand(
      SX126X_CMD_SET_RF_FREQUENCY,
      data,
      4);
}

// ============================================================
// LoRa MODULATION PARAMETERS
// ============================================================

void sxSetLoRaModulation()
{
  uint8_t data[4];

  data[0] = LORA_SF;
  data[1] = 0x70;       // 125 kHz
  data[2] = LORA_CR;
  data[3] = 0x00;       // LDRO off

  sxCommand(
      SX126X_CMD_SET_MOD_PARAMS,
      data,
      4);
}

// ============================================================
// LoRa PACKET PARAMETERS
// ============================================================

void sxSetLoRaPacket()
{
  uint8_t data[6];

  data[0] = (LORA_PREAMBLE >> 8) & 0xFF;
  data[1] = LORA_PREAMBLE & 0xFF;

  data[2] = 0x00;       // Explicit header
  data[3] = 0xFF;       // Max payload
  data[4] = 0x01;       // CRC ON
  data[5] = 0x00;       // Normal IQ

  sxCommand(
      SX126X_CMD_SET_PACKET_PARAMS,
      data,
      6);
}

// ============================================================
// CAD PARAMETERS
// ============================================================

void sxSetCadParams()
{
  uint8_t data[7];

  data[0] = 0x02;       // CAD_ON_2_SYMB
  data[1] = 22;         // Detection peak
  data[2] = 10;         // Detection minimum

  data[3] = 0x00;       // Timeout
  data[4] = 0x00;
  data[5] = 0x00;

  data[6] = 0x01;       // Exit CAD -> STDBY_RC

  sxCommand(
      SX126X_CMD_SET_CAD_PARAMS,
      data,
      7);
}

// ============================================================
// CAD IRQ ROUTING
//
// CAD_DETECTED -> DIO1
// ============================================================

void sxConfigureCadIrq()
{
  uint8_t data[8];

  uint16_t irqMask = IRQ_CAD_DETECTED;

  // IRQ mask
  data[0] = irqMask >> 8;
  data[1] = irqMask & 0xFF;

  // DIO1 mask
  data[2] = irqMask >> 8;
  data[3] = irqMask & 0xFF;

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
// AUTONOMOUS WOR IRQ ROUTING
//
// PREAMBLE_DETECTED -> DIO1
// ============================================================

void sxConfigureWorIrq()
{
  uint8_t data[8];

  uint16_t irqMask =
      IRQ_PREAMBLE_DETECTED;

  // IRQ mask
  data[0] = irqMask >> 8;
  data[1] = irqMask & 0xFF;

  // DIO1 mask
  data[2] = irqMask >> 8;
  data[3] = irqMask & 0xFF;

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
// START CAD
// ============================================================

void sxStartCad()
{
  Serial.println("Starting SX1262 CAD...");

  waitBusy();

  digitalWrite(RADIO_CS_PIN, LOW);

  radioSPI.beginTransaction(
      SPISettings(8000000, MSBFIRST, SPI_MODE0));

  radioSPI.transfer(
      SX126X_CMD_SET_CAD);

  radioSPI.endTransaction();

  digitalWrite(RADIO_CS_PIN, HIGH);
}

// ============================================================
// SX1262 RX DUTY CYCLE / AUTONOMOUS WOR
//
// RX for rxPeriod
// SLEEP for sleepPeriod
// Automatically repeats.
//
// Timing unit = 15.625 us
//
// Both parameters are 24-bit values.
// ============================================================

void sxSetRxDutyCycle(uint32_t rxPeriod,
                      uint32_t sleepPeriod)
{
  uint8_t data[6];

  // RX period
  data[0] = (rxPeriod >> 16) & 0xFF;
  data[1] = (rxPeriod >> 8)  & 0xFF;
  data[2] = rxPeriod & 0xFF;

  // Sleep period
  data[3] = (sleepPeriod >> 16) & 0xFF;
  data[4] = (sleepPeriod >> 8)  & 0xFF;
  data[5] = sleepPeriod & 0xFF;

  sxCommand(
      SX126X_CMD_SET_RX_DUTY_CYCLE,
      data,
      6);
}

