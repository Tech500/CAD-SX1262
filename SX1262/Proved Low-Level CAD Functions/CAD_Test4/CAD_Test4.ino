#include <Arduino.h>
#include <SPI.h>

#define EoRa_PI_V1
#include "boards.h"
#include "sx1262_commands_test4.h"

// ============================================================
// TEST 4
// SX1262 LOW-LEVEL CAD VALIDATION
//
// Purpose:
//   Prove:
//     CAD -> CAD_DONE -> DIO1 -> GPIO16
//
// No:
//   RadioLib
//   Deep sleep
//   EXT0
//   WOR
//   RX duty cycle
// ============================================================

void setup()
{
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("========================================");
  Serial.println(" EoRa-S3-900TB");
  Serial.println(" SX1262 LOW-LEVEL CAD VALIDATION");
  Serial.println(" TEST 4");
  Serial.println("========================================");

  // ----------------------------------------------------------
  // SPI
  // ----------------------------------------------------------

  radioSPI.begin(
    RADIO_SCLK_PIN,
    RADIO_MISO_PIN,
    RADIO_MOSI_PIN,
    RADIO_CS_PIN
  );

  pinMode(RADIO_CS_PIN, OUTPUT);
  digitalWrite(RADIO_CS_PIN, HIGH);

  pinMode(RADIO_BUSY_PIN, INPUT);
  pinMode(RADIO_RST_PIN, OUTPUT);

  // DIO1 is connected to ESP32 GPIO16 on this hardware
  pinMode(RADIO_DIO1_PIN, INPUT);

  // ----------------------------------------------------------
  // RESET
  // ----------------------------------------------------------

  sxReset();

  // ----------------------------------------------------------
  // BASIC RADIO CONFIGURATION
  // ----------------------------------------------------------

  sxStandby();
  sxSetPacketTypeLoRa();
  sxSetFrequency(LORA_FREQ_HZ);
  sxSetLoRaModulation();
  sxSetLoRaPacket();
  sxSetDio2AsRfSwitch();

  // ----------------------------------------------------------
  // CAD CONFIGURATION
  // ----------------------------------------------------------

  sxSetCadParams();

  // ----------------------------------------------------------
  // IRQ CONFIGURATION
  //
  // ONLY:
  //   CAD_DONE -> DIO1
  // ----------------------------------------------------------

  sxClearIrq();
  sxConfigureCadIrq();

  Serial.println();
  Serial.println("=== CAD IRQ CONFIGURED ===");
  Serial.println("Global IRQ : CAD_DONE");
  Serial.println("DIO1       : CAD_DONE");
  Serial.println("DIO2       : NONE");
  Serial.println("DIO3       : NONE");

  // ----------------------------------------------------------
  // START CAD
  // ----------------------------------------------------------

  Serial.println();
  sxStartCad();

  Serial.println();
  Serial.printf(
    "DIO1 after CAD start = %d\n",
    digitalRead(RADIO_DIO1_PIN)
  );

  Serial.println();
  Serial.println("=== CAD RUNNING ===");
}

void loop()
{
  // ----------------------------------------------------------
  // Monitor CAD completion.
  //
  // NO restarting yet.
  // NO sleep.
  // NO WOR.
  // ----------------------------------------------------------

  static bool reported = false;

  int dio1 = digitalRead(RADIO_DIO1_PIN);

  if (dio1 && !reported)
  {
    uint16_t irq = sxGetIrq();

    Serial.println();
    Serial.println("========================================");
    Serial.println(" DIO1 HIGH");
    Serial.println(" CAD IRQ DETECTED");
    Serial.printf(" IRQ = 0x%04X\n", irq);
    Serial.println("========================================");

    reported = true;
  }
}