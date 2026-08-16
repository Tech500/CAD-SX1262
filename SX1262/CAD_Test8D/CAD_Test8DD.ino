// ============================================================
// EoRa-S3-900TB
// SX1262 LOW-LEVEL REPEATED CAD TEST
//
// TEST 8D
//
// PURPOSE:
//   Detect a continuously transmitting 5000-symbol WOR
//   preamble using repeated one-shot CAD operations.
//
// SEQUENCE:
//
//   1. Transmitter WOR button pressed
//   2. 5000-symbol preamble begins
//   3. User presses ENTER
//   4. Receiver performs repeated CAD scans
//   5. Each CAD completion is checked
//   6. Stop immediately on CAD_DETECTED
//
// NO:
//   RadioLib
//   Deep sleep
//   EXT0
//   RX duty cycle
//
// SX1262 DIO1 -> ESP32-S3 GPIO16
// ============================================================

#define EoRa_PI_V1

#include <Arduino.h>
#include <SPI.h>
#include <boards.h>

#include "sx1262_commands_test8D.h"


// ============================================================
// TEST SETTINGS
// ============================================================

#define MAX_CAD_SCANS        1500

// Delay between completed CAD operations.
// Start at zero so we don't artificially reduce detection
// opportunities.
#define CAD_RESTART_DELAY_MS  0


// ============================================================
// PRINT HEADER
// ============================================================

void printHeader()
{
  Serial.println();
  Serial.println("========================================");
  Serial.println(" EoRa-S3-900TB");
  Serial.println(" SX1262 LOW-LEVEL REPEATED CAD TEST");
  Serial.println(" TEST 8D");
  Serial.println("========================================");
}


// ============================================================
// RADIO INITIALIZATION
// ============================================================

void initRadio()
{
  Serial.println("SX1262 reset...");

  sxReset();

  // ----------------------------------------------------------
  // Basic LoRa configuration
  // ----------------------------------------------------------

  sxStandby();

  sxSetPacketTypeLoRa();

  sxSetFrequency(LORA_FREQ_HZ);

  sxSetLoRaModulation();

  sxSetPacketParams();

  // ----------------------------------------------------------
  // CAD configuration
  // ----------------------------------------------------------

  sxSetCadParams();

  // ----------------------------------------------------------
  // IRQ configuration
  //
  // Global:
  //   CAD_DONE
  //   CAD_DETECTED
  //
  // DIO1:
  //   CAD_DETECTED only
  // ----------------------------------------------------------

  sxConfigureCadIrq();

  // ----------------------------------------------------------
  // Start with clean IRQ state
  // ----------------------------------------------------------

  sxClearIrq();

  Serial.println();
  Serial.println("=== CAD IRQ CONFIGURED ===");

  Serial.println("Global IRQ : CAD_DONE + CAD_DETECTED");
  Serial.println("DIO1       : CAD_DETECTED");
  Serial.println("DIO2       : NONE");
  Serial.println("DIO3       : NONE");

  Serial.printf(
      "GPIO16 before CAD = %d\n",
      digitalRead(16));
}

uint32_t scanCount = 0;

// ============================================================
// WAIT FOR ENTER
// ============================================================

void waitForEnter()
{
  Serial.println();
  Serial.println("----------------------------------------");
  Serial.println(" READY FOR WOR");
  Serial.println("----------------------------------------");
  Serial.println();
  Serial.println("1. Press the WOR button on the transmitter.");
  Serial.println("2. Confirm the 5000-symbol preamble is transmitting.");
  Serial.println("3. THEN press ENTER here.");
  Serial.println();
  Serial.println("Waiting for ENTER...");
  Serial.println("----------------------------------------");

  // Wait here until ENTER is actually pressed
  while (true)
  {
    if (Serial.available())
    {
      char c = Serial.read();

      if (c == '\n' || c == '\r')
      {
        // Clear the EXISTING global scan counter
        scanCount = 0;

        Serial.println();
        Serial.println("ENTER RECEIVED.");
        Serial.println("----------------------------------------");
        Serial.println("CAD scan counter RESET = 0");
        Serial.println("Starting repeated CAD scans...");
        Serial.println("----------------------------------------");

        // Remove any remaining CR/LF characters
        while (Serial.available())
        {
          Serial.read();
        }

        return;
      }
    }

    yield();
  }
}

// ============================================================
// RUN REPEATED CAD
// ============================================================

// ============================================================
// RUN REPEATED CAD
//
// TEST 8D PATCH
//
// IMPORTANT:
//   BUSY is NOT used to determine when CAD is complete.
//
//   BUSY only tells us whether the SX1262 command interface
//   is available.
//
//   CAD completion is determined by:
//       CAD_DONE
//       CAD_DETECTED
//
// Sequence:
//
//   CLEAR_IRQ
//       |
//       v
//   SET_CAD
//       |
//       v
//   wait for CAD_DONE / CAD_DETECTED
//       |
//       +---- CAD_DETECTED -> PASS
//       |
//       +---- CAD_DONE ----> next CAD
//
// ============================================================

bool runCadScanner(uint32_t &scanCount)
{
  scanCount = 0;

  Serial.println();
  Serial.println("========================================");
  Serial.println(" STARTING REPEATED CAD");
  Serial.println("========================================");

  Serial.println();
  Serial.println("=== CAD ACTIVE ===");
  Serial.println("Watching for CAD_DONE / CAD_DETECTED...");
  Serial.println();

  while (scanCount < MAX_CAD_SCANS)
  {
    scanCount++;

    // --------------------------------------------------------
    // Make absolutely certain the SX1262 command interface
    // is ready before beginning the next CAD operation.
    // --------------------------------------------------------

    uint32_t busyStart = millis();

    while (digitalRead(RADIO_BUSY_PIN) == HIGH)
    {
      yield();

      if ((millis() - busyStart) > 500)
      {
        Serial.println();
        Serial.println("[ERROR] SX1262 BUSY before CAD timeout!");

        Serial.printf(
            "CAD scans = %lu\n",
            (unsigned long)scanCount);

        return false;
      }
    }

    // --------------------------------------------------------
    // Clear ALL previous IRQs.
    //
    // This is done immediately before EVERY SET_CAD.
    // --------------------------------------------------------

    sxClearIrq();

    // --------------------------------------------------------
    // Confirm IRQ really is clear.
    // --------------------------------------------------------

    uint16_t irqBefore = sxGetIrq();

    if (irqBefore != 0x0000)
    {
      Serial.printf(
          "CAD scan %lu: IRQ not clear = 0x%04X\n",
          (unsigned long)scanCount,
          irqBefore);

      sxClearIrq();
    }

    // --------------------------------------------------------
    // START CAD
    // --------------------------------------------------------

    sxStartCad();

    // --------------------------------------------------------
    // IMPORTANT:
    //
    // DO NOT use BUSY to determine CAD completion.
    //
    // Wait for the actual CAD IRQ.
    // --------------------------------------------------------

    uint32_t cadStart = millis();

    bool cadFinished = false;
    uint16_t irq = 0;

    while (!cadFinished)
    {
      irq = sxGetIrq();

      // ------------------------------------------------------
      // CAD DETECTED
      // ------------------------------------------------------

      if (irq & IRQ_CAD_DETECTED)
      {
        Serial.println();
        Serial.println("========================================");
        Serial.println(" *** CAD DETECTED ***");
        Serial.println("========================================");

        Serial.printf(
            "CAD scans = %lu\n",
            (unsigned long)scanCount);

        Serial.printf(
            "IRQ = 0x%04X\n",
            irq);

        Serial.printf(
            "DIO1/GPIO16 = %d\n",
            digitalRead(16));

        Serial.printf(
            "CAD time = %lu ms\n",
            (unsigned long)(millis() - cadStart));

        // Clear after recording detection.
        sxClearIrq();

        return true;
      }

      // ------------------------------------------------------
      // CAD DONE
      // ------------------------------------------------------

      if (irq & IRQ_CAD_DONE)
      {
        cadFinished = true;
        break;
      }

      // ------------------------------------------------------
      // CAD should not take this long.
      //
      // This prevents a failed radio state from hanging
      // the test forever.
      // ------------------------------------------------------

      if ((millis() - cadStart) > 1000)
      {
        Serial.println();
        Serial.printf(
            "CAD scan %lu: CAD IRQ timeout, IRQ = 0x%04X\n",
            (unsigned long)scanCount,
            irq);

        sxClearIrq();

        return false;
      }

      yield();
    }

    // --------------------------------------------------------
    // CAD completed normally without detection.
    // --------------------------------------------------------

    irq = sxGetIrq();

    if (irq & IRQ_CAD_DETECTED)
    {
      Serial.println();
      Serial.println("========================================");
      Serial.println(" *** CAD DETECTED ***");
      Serial.println("========================================");

      Serial.printf(
          "CAD scans = %lu\n",
          (unsigned long)scanCount);

      Serial.printf(
          "IRQ = 0x%04X\n",
          irq);

      Serial.printf(
          "DIO1/GPIO16 = %d\n",
          digitalRead(16));

      sxClearIrq();

      return true;
    }

    if (irq & IRQ_CAD_DONE)
    {
      // ------------------------------------------------------
      // Normal CAD completion.
      // ------------------------------------------------------

      sxClearIrq();
    }
    else
    {
      Serial.printf(
          "CAD scan %lu: unexpected final IRQ = 0x%04X\n",
          (unsigned long)scanCount,
          irq);

      sxClearIrq();
    }

    // --------------------------------------------------------
    // Progress display.
    // --------------------------------------------------------

    if ((scanCount % 10) == 0)
    {
      Serial.printf(
          "CAD scans = %lu\n",
          (unsigned long)scanCount);
    }

    // --------------------------------------------------------
    // IMPORTANT:
    //
    // Before the next SET_CAD, the top of the loop will again
    // wait for BUSY = LOW.
    //
    // Thus:
    //
    //   CAD IRQ controls CAD completion
    //   BUSY controls command readiness
    //
    // --------------------------------------------------------

    if (CAD_RESTART_DELAY_MS > 0)
    {
      delay(CAD_RESTART_DELAY_MS);
    }
  }

  return false;
}

// ============================================================
// SETUP
// ============================================================

void setup()
{
  Serial.begin(115200);

  delay(1000);



  printHeader();

  // ----------------------------------------------------------
  // SPI / GPIO
  // ----------------------------------------------------------

  pinMode(RADIO_CS_PIN, OUTPUT);
  digitalWrite(RADIO_CS_PIN, HIGH);

  pinMode(RADIO_RST_PIN, OUTPUT);
  digitalWrite(RADIO_RST_PIN, HIGH);

  pinMode(RADIO_BUSY_PIN, INPUT);

  pinMode(16, INPUT);

  radioSPI.begin(
      RADIO_SCLK_PIN,
      RADIO_MISO_PIN,
      RADIO_MOSI_PIN,
      RADIO_CS_PIN);

  // ----------------------------------------------------------
  // Initialize SX1262
  // ----------------------------------------------------------

  initRadio();

  // ----------------------------------------------------------
  // Wait for transmitter
  // ----------------------------------------------------------

  waitForEnter();

  // ----------------------------------------------------------
  // Run repeated CAD
  // ----------------------------------------------------------

  

  scanCount = 0;

  bool detected = runCadScanner(scanCount);

  // ----------------------------------------------------------
  // RESULT
  // ----------------------------------------------------------

  Serial.println();
  Serial.println("========================================");
  Serial.println(" *** TEST 8D RESULT ***");
  Serial.println("========================================");

  if (detected)
  {
    Serial.println("PASS:");
    Serial.println("CAD_DETECTED detected the active");
    Serial.println("WOR preamble.");

    Serial.printf(
        "Detection occurred at CAD scan %lu.\n",
        (unsigned long)scanCount);
  }
  else
  {
    Serial.println("FAIL:");
    Serial.println("No CAD_DETECTED after maximum scans.");

    Serial.printf(
        "CAD scans = %lu\n", (unsigned long)scanCount);
  }

  Serial.println();
  Serial.println("TEST COMPLETE.");
}


// ============================================================
// LOOP
// ============================================================

void loop()
{
  // Test runs once.
}