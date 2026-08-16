#include <Arduino.h>
#include <SPI.h>

#define EoRa_PI_V1
#include "boards.h"
#include "sx1262_commands_test7.h"

#define WAKEUP_PIN GPIO_NUM_16


// ============================================================
// TEST 6
// SX1262 LOW-LEVEL CAD_DETECTED VALIDATION
//
// Continuously re-arms CAD.
//
// Purpose:
//   Prove that a real LoRa signal causes:
//
//      CAD_DETECTED
//          |
//          v
//        DIO1
//          |
//          v
//       GPIO16
//
// NO deep sleep
// NO EXT0
// NO WOR receiver logic
// NO RX duty cycle
// NO RadioLib
// ============================================================

bool detected = false;
uint32_t cadCount = 0;

void setup()
{
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("========================================");
  Serial.println(" EoRa-S3-900TB");
  Serial.println(" SX1262 LOW-LEVEL CAD VALIDATION");
  Serial.println(" TEST 6");
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
  pinMode(WAKEUP_PIN, INPUT);

  // ----------------------------------------------------------
  // RESET
  // ----------------------------------------------------------

  sxReset();

  // ----------------------------------------------------------
  // RADIO CONFIGURATION
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
  // ONLY CAD_DETECTED -> DIO1
  // ----------------------------------------------------------

  sxClearIrq();
  sxConfigureCadIrq();

  Serial.println();
  Serial.println("=== CAD IRQ CONFIGURED ===");
  Serial.println("Global IRQ : CAD_DETECTED");
  Serial.println("DIO1       : CAD_DETECTED");
  Serial.println("DIO2       : NONE");
  Serial.println("DIO3       : NONE");

  // ----------------------------------------------------------
  // START FIRST CAD
  // ----------------------------------------------------------

  Serial.println();
  sxStartCad();

  Serial.printf("DIO1 after CAD start = %d\n", digitalRead(WAKEUP_PIN));

  Serial.println();
  Serial.println("=== CAD SCANNING ===");
}

void loop()
{
  // ----------------------------------------------------------
  // Check DIO1
  // ----------------------------------------------------------

  if (digitalRead(RADIO_DIO1_PIN))
  {
    uint16_t irq = sxGetIrq();

    Serial.println();
    Serial.println("========================================");
    Serial.println(" *** CAD DETECTED ***");
    Serial.printf(" IRQ = 0x%04X\n", irq);
    Serial.printf(" CAD scans = %lu\n", cadCount);
    Serial.println(" DIO1 = HIGH");
    Serial.println("========================================");

    detected = true;

    // --------------------------------------------------------
    // STOP HERE.
    //
    // We have proven CAD_DETECTED.
    // --------------------------------------------------------

    while (true)
    {
      delay(1000);
    }
  }

  // ----------------------------------------------------------
  // CAD_DONE isn't routed to DIO1 in Test 6.
  //
  // Therefore we use the IRQ register to determine when
  // the current CAD operation has completed.
  // ----------------------------------------------------------

  uint16_t irq = sxGetIrq();

  if (irq & IRQ_CAD_DONE)
  {
    cadCount++;

    // Clear the completed CAD IRQ.
    sxClearIrq();

    // Re-arm CAD.
    sxStartCad();
  }

  delay(1);
}