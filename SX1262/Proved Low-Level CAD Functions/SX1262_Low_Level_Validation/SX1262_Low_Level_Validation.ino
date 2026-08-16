/*
  EoRa-S3-900TB
  SX1262 LOW-LEVEL COMMAND VALIDATION

  Purpose:
    Validate the corrected sx1262_commands.h command layer
    BEFORE adding CAD + EXT0 + deep sleep.

  Test sequence:
    1. Initialize SPI / control pins
    2. Reset SX1262
    3. Standby RC
    4. Packet type = LoRa
    5. Frequency = 915 MHz
    6. LoRa modulation parameters
    7. LoRa packet parameters
    8. DIO2 RF switch
    9. Configure CAD IRQ -> DIO1
   10. Clear IRQ
   11. Start CAD
   12. Read IRQ repeatedly
   13. Repeat CAD

  IMPORTANT:
    No ESP32 deep sleep.
    No EXT0.
    No BME280.
    No RadioLib.
    No WOR transmitter is required for the first test.

  Hardware pins from the EoRa-S3-900TB configuration used in
  the previous CAD work:
    SCLK = GPIO 5
    MISO = GPIO 3
    MOSI = GPIO 6
    CS   = GPIO 7
    DIO1 = GPIO 16
    BUSY = GPIO 34
    RST  = GPIO 8
*/

#include <Arduino.h>
#include <SPI.h>

// ------------------------------------------------------------
// Pin definitions
// ------------------------------------------------------------
#define RADIO_SCLK_PIN  5
#define RADIO_MISO_PIN  3
#define RADIO_MOSI_PIN  6
#define RADIO_CS_PIN    7
#define RADIO_DIO1_PIN  16
#define RADIO_BUSY_PIN  34
#define RADIO_RST_PIN   8

// The uploaded corrected header defines radioSPI(FSPI).
// Include it after the pin definitions.
#include "sx1262_commands.h"

// ------------------------------------------------------------
// Diagnostic helpers
// ------------------------------------------------------------

void printIrq(uint16_t irq)
{
  Serial.printf("IRQ = 0x%04X", irq);

  if (irq == 0) {
    Serial.println("  [NONE]");
    return;
  }

  Serial.print("  [");

  bool first = true;

  if (irq & IRQ_RX_DONE) {
    Serial.print("RX_DONE");
    first = false;
  }

  if (irq & IRQ_PREAMBLE_DETECTED) {
    if (!first) Serial.print(" | ");
    Serial.print("PREAMBLE_DETECTED");
    first = false;
  }

  if (irq & IRQ_CAD_DONE) {
    if (!first) Serial.print(" | ");
    Serial.print("CAD_DONE");
    first = false;
  }

  if (irq & IRQ_CAD_DETECTED) {
    if (!first) Serial.print(" | ");
    Serial.print("CAD_DETECTED");
    first = false;
  }

  Serial.println("]");
}

bool checkIrqClear()
{
  uint16_t irq = sxGetIrq();

  Serial.print("After CLEAR_IRQ: ");
  printIrq(irq);

  return irq == 0;
}

// ------------------------------------------------------------
// Initialization
// ------------------------------------------------------------

void initRadioPins()
{
  pinMode(RADIO_CS_PIN, OUTPUT);
  digitalWrite(RADIO_CS_PIN, HIGH);

  pinMode(RADIO_RST_PIN, OUTPUT);
  digitalWrite(RADIO_RST_PIN, HIGH);

  pinMode(RADIO_BUSY_PIN, INPUT);

  // External 10K pulldown is preferred for this test.
  pinMode(RADIO_DIO1_PIN, INPUT);

  radioSPI.begin(
    RADIO_SCLK_PIN,
    RADIO_MISO_PIN,
    RADIO_MOSI_PIN,
    RADIO_CS_PIN
  );
}

// ------------------------------------------------------------
// Radio configuration
// ------------------------------------------------------------

bool configureRadio()
{
  Serial.println();
  Serial.println("=== CONFIGURING SX1262 ===");

  sxStandby();
  delay(5);

  Serial.println("Set packet type: LoRa");
  sxSetPacketTypeLoRa();
  delay(5);

  Serial.println("Set frequency: 915 MHz");
  sxSetFrequency(LORA_FREQ_HZ);
  delay(5);

  Serial.println("Set LoRa modulation: SF7 / BW125 / CR4/5");
  sxSetLoRaModulation();
  delay(5);

  Serial.println("Set LoRa packet parameters");
  sxSetLoRaPacket();
  delay(5);

  Serial.println("Set DIO2 as RF switch");
  sxSetDio2AsRfSwitch();
  delay(5);

  Serial.println("Set CAD parameters");
  sxSetCadParams();
  delay(5);

  Serial.println("Configure CAD/Preamble IRQ -> DIO1");
  sxConfigureCadIrq();
  delay(5);

  Serial.println("Clear all IRQs");
  sxClearIrq();
  delay(5);

  return checkIrqClear();
}

// ------------------------------------------------------------
// CAD test
// ------------------------------------------------------------

void runCadTest()
{
  Serial.println();
  Serial.println("========================================");
  Serial.println(" STARTING CAD");
  Serial.println("========================================");

  sxClearIrq();

  Serial.printf("DIO1 before CAD = %d\n",
                digitalRead(RADIO_DIO1_PIN));

  sxStartCad();

  Serial.printf("DIO1 immediately after CAD = %d\n",
                digitalRead(RADIO_DIO1_PIN));

  Serial.println("Waiting for CAD result...");

  const uint32_t timeoutMs = 3000;
  uint32_t start = millis();

  while (millis() - start < timeoutMs) {

    uint16_t irq = sxGetIrq();

    if (irq != 0) {
      Serial.printf("DIO1 = %d  ",
                    digitalRead(RADIO_DIO1_PIN));
      printIrq(irq);

      if (irq & IRQ_CAD_DETECTED) {
        Serial.println("*** CAD DETECTED ***");
      }

      if (irq & IRQ_CAD_DONE) {
        Serial.println("*** CAD DONE ***");
      }

      return;
    }

    delay(10);
  }

  Serial.println("*** CAD TEST TIMEOUT ***");
  Serial.printf("DIO1 at timeout = %d\n",
                digitalRead(RADIO_DIO1_PIN));

  Serial.print("Final ");
  printIrq(sxGetIrq());
}

// ------------------------------------------------------------
// Arduino setup
// ------------------------------------------------------------

void setup()
{
  Serial.begin(115200);
  delay(1500);

  Serial.println();
  Serial.println("========================================");
  Serial.println(" EoRa-S3-900TB");
  Serial.println(" SX1262 LOW-LEVEL VALIDATION");
  Serial.println("========================================");

  initRadioPins();

  Serial.println();
  Serial.println("Resetting SX1262...");
  sxReset();

  if (!configureRadio()) {
    Serial.println();
    Serial.println("*** IRQ CLEAR VALIDATION FAILED ***");
    Serial.println("*** STOPPING TEST ***");

    while (true) {
      delay(1000);
    }
  }

  Serial.println();
  Serial.println("*** BASIC CONFIGURATION PASSED ***");

  Serial.println();
  Serial.println("Initial IRQ state:");
  printIrq(sxGetIrq());

  Serial.printf("Initial DIO1 = %d\n",
                digitalRead(RADIO_DIO1_PIN));

  Serial.println();
  Serial.println("No RF required for first CAD test.");
  Serial.println("The radio should report CAD_DONE with");
  Serial.println("no CAD_DETECTED when the channel is quiet.");
}

// ------------------------------------------------------------
// Main loop
// ------------------------------------------------------------

void loop()
{
  runCadTest();

  Serial.println();
  Serial.println("Waiting 2 seconds before next CAD...");
  delay(2000);
}
