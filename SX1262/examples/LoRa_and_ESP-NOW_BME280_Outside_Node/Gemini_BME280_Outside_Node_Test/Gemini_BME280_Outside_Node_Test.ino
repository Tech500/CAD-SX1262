/*
  Gemini_BME280_Outsie_Node_Test.ino
  August 15,2026
  ESP32 Core 3.3.10 is required!
  CAD+LoRa+ESP-NOW+WOR+Deep-Sleep
  Project tested on Ebyte's, EoRa-S3-900TB
*/

#define EoRa_PI_V1
#include <boards.h>
#include "CAD.h"  // SX1262 driver includes
#include <SPI.h>
#include "driver/rtc_io.h"
#include "rom/rtc.h"
#include "esp_system.h"  // esp_reset_reason() -- for reset_log.txt
#include "esp_private/periph_ctrl.h"
#include <esp_sleep.h>

#define BENCH_TESTING true

#define WAKEUP_PIN GPIO_NUM_16

// Send OpCode + Parameter Payload
void sx1262_writeCommand(uint8_t opCode, const uint8_t* buffer, size_t size) {
  digitalWrite(RADIO_CS_PIN, LOW);
  SPI.transfer(opCode);
  for (size_t i = 0; i < size; i++) {
    SPI.transfer(buffer[i]);
  }
  digitalWrite(RADIO_CS_PIN, HIGH);
  delayMicroseconds(50);
}

// Read Response (OpCode -> Status NOP -> Read Data Bytes)
uint16_t sx1262_getIrqStatus() {
  digitalWrite(RADIO_CS_PIN, LOW);
  SPI.transfer(0x12); // OpCode 0x12: GetIrqStatus
  SPI.transfer(0x00); // Status NOP dummy byte
  uint8_t msb = SPI.transfer(0x00);
  uint8_t lsb = SPI.transfer(0x00);
  digitalWrite(RADIO_CS_PIN, HIGH);
  return ((uint16_t)msb << 8) | lsb;
}


#define LORA_FREQ_HZ 915000000UL

void releasePinHoldsOnWake() {
  // Disable holds on SPI Bus
  gpio_hold_dis((gpio_num_t)RADIO_CS_PIN);
  gpio_hold_dis((gpio_num_t)RADIO_SCLK_PIN);  // SCK (GPIO 5)
  gpio_hold_dis((gpio_num_t)RADIO_MISO_PIN);  // MISO (GPIO 3)
  gpio_hold_dis((gpio_num_t)RADIO_MOSI_PIN);  // MOSI (GPIO 6)

  // Explicitly release BUSY pin hold if it was set
  gpio_hold_dis((gpio_num_t)RADIO_BUSY_PIN);  // GPIO 34

  // Release global sleep hold flag
  gpio_deep_sleep_hold_dis();
}

void releasePins() {
  releasePinHoldsOnWake();


  radioSPI.end();  // Clear any zombie handles
  // Re-initialize hardware SPI bus explicitly
  radioSPI.begin(RADIO_SCLK_PIN, RADIO_MISO_PIN, RADIO_MOSI_PIN, RADIO_CS_PIN);
}

void initRadio(bool wokeFromSleep) {
  Serial.println("\n--- Initializing SX1262 ---");

  // Force CS HIGH and BUSY to INPUT before touching radio
  pinMode((gpio_num_t)RADIO_CS_PIN, OUTPUT);
  digitalWrite((gpio_num_t)RADIO_CS_PIN, HIGH);
  pinMode((gpio_num_t)RADIO_BUSY_PIN, INPUT);

  if (!wokeFromSleep) {
    Serial.println("SX1262 reset...");
    sxReset();  // Cold boot: hardware reset toggle
    Serial.println("SX1262 reset complete.");

    // CRITICAL: Give internal 32MHz crystal time to stabilize after NRESET release
    delay(10);
  } else {
    Serial.println("Woke from sleep: Preserving IRQ flags.");
  }

  // Wait for BUSY to go LOW after reset before issuing Standby
  sxWaitBusy();

  Serial.println("Entering sxStandby...");
  sxStandby();
  Serial.println("sxStandby complete.");

  // FIX 1: Set Regulator to DC-DC Converter (0x01) instead of LDO (0x00)
  // Drastically cuts internal SX1262 idle/CAD current draw
  //No inductor on pcb forDC-DC Converter.
  uint8_t regModeData[] = { 0x00 };
  sxCommand(SX126X_CMD_SET_REGULATOR_MODE, regModeData, 1);

  // Configure DIO2 to drive internal RF switch (E22 module)
  sxSetDio2AsRfSwitch();

  // Crystal Loading Trim
  sxWriteRegister(REG_XTAL_TRIM_A, 0x12);
  sxWriteRegister(REG_XTAL_TRIM_B, 0x12);

  // Set Rx Gain to High Sensitivity
  sxWriteRegister(REG_RX_GAIN, 0x96);

  // Set LoRa Mode & Frequency
  sxSetPacketTypeLoRa();
  // Set Private Sync Word (0x1424) to match RadioLib RADIOLIB_SX126X_SYNC_WORD_PRIVATE
  sxWriteRegister(0x0740, 0x14);
  sxWriteRegister(0x0741, 0x24);

  sxSetFrequency(LORA_FREQ_HZ);

  // Modulation Parameters
  uint8_t modParams[] = { LORA_SF, LORA_BW, LORA_CR, 0x00 };
  sxCommand(SX126X_CMD_SET_MOD_PARAMS, modParams, 4);

  // Packet Parameters
  uint8_t pktParams[6] = {
    (uint8_t)((LORA_PREAMBLE >> 8) & 0xFF),
    (uint8_t)(LORA_PREAMBLE & 0xFF),
    0x00,  // Explicit header
    0xFF,  // Max payload limit
    0x01,  // CRC On
    0x00   // Normal IQ
  };
  sxCommand(SX126X_CMD_SET_PACKET_PARAMS, pktParams, 6);

  // CAD and IRQ Config
  sxSetCadParams();
  sxConfigureWorIrq();
  sxClearIrq();

  Serial.println("SX1262 LoRa/CAD configuration complete.");
}

void inspectWake() {
  Serial.println("\n======================================");
  Serial.println(" EXT0 WAKE DIAGNOSTIC (GPIO 16)");
  Serial.println("======================================");

  Serial.printf("Wake cause = %d\n", esp_sleep_get_wakeup_cause());
  Serial.printf("GPIO16 / DIO1 = %d\n", digitalRead(WAKEUP_PIN));

  uint16_t irq = sxGetIrq();
  Serial.printf("SX1262 IRQ BEFORE RESET = 0x%04X\n", irq);

  if (irq & IRQ_CAD_DETECTED) Serial.println("*** CAD_DETECTED = YES ***");
  if (irq & IRQ_CAD_DONE) Serial.println("*** CAD_DONE = YES ***");
  if (irq & IRQ_PREAMBLE_DETECTED) Serial.println("*** PREAMBLE_DETECTED = YES ***");

  sxClearIrq();
  delay(2);
  Serial.printf("GPIO16 / DIO1 after clear = %d\n", digitalRead(WAKEUP_PIN));
}

void disableBatteryADC() {
  analogSetPinAttenuation(GPIO_NUM_1, ADC_0db);
  pinMode(GPIO_NUM_1, INPUT);
  gpio_pullup_dis(GPIO_NUM_1);
  gpio_pulldown_dis(GPIO_NUM_1);
  gpio_hold_en(GPIO_NUM_1);
}

// Set to true for live USB serial debugging on the bench.
// Set to false for true battery / PPK2 low-power measurements.
//#define BENCH_TESTING true

void shutdownUSB() {
#if !BENCH_TESTING
  // 1. Flush serial buffer
  Serial.flush();

  // 2. Disable clock domain to native USB PHY
  periph_module_disable(PERIPH_USB_MODULE);

  // 3. Isolate floating D+/D- lines (GPIO 19 & 20)
  pinMode(GPIO_NUM_19, INPUT);
  pinMode(GPIO_NUM_20, INPUT);

  gpio_pullup_dis(GPIO_NUM_19);
  gpio_pulldown_dis(GPIO_NUM_19);
  gpio_pullup_dis(GPIO_NUM_20);
  gpio_pulldown_dis(GPIO_NUM_20);

  gpio_hold_en(GPIO_NUM_19);
  gpio_hold_en(GPIO_NUM_20);
#else
  Serial.println(F("[BENCH MODE] USB kept active for debugging."));
  Serial.flush();
#endif
}

bool verifySignalWithBusyLoop(int totalPasses = 10, int requiredHits = 3) {
  Serial.println(F("\n--- [WAKE DIAGNOSTIC & MULTI-SCAN START] ---"));
  
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  Serial.printf("  Wakeup Cause Code: %d ", wakeup_reason);
  if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT0) {
    Serial.println(F("(ESP_SLEEP_WAKEUP_EXT0 - DIO1 / Inverter High)"));
  } else {
    Serial.println(F("(Other Wakeup Reason / Cold Boot)"));
  }

  // Read raw pin states directly
  int dio1State = digitalRead(WAKEUP_PIN);
  int busyState = digitalRead(RADIO_BUSY_PIN);
  Serial.printf("  Pin Levels -> WAKEUP_PIN (GPIO %d): %d | BUSY (GPIO %d): %d\n", 
                WAKEUP_PIN, dio1State, RADIO_BUSY_PIN, busyState);

  // Direct SPI read of SX1262 IRQ register (0x12)
  uint16_t irqFlags = sx1262_getIrqStatus();
  Serial.printf("  SX1262 Bare-Metal IRQ Register: 0x%04X\n", irqFlags);

  // Bit 0x0002 = RxDone, 0x0004 = PreambleDetected, 0x0010 = HeaderValid, 0x0080 = CadDetected
  if (irqFlags & 0x0080) {
    Serial.println(F("  [IRQ CHECK] -> CAD_DETECTED (0x0080) is SET!"));
  }
  if (irqFlags & 0x0004) {
    Serial.println(F("  [IRQ CHECK] -> PREAMBLE_DETECTED (0x0004) is SET!"));
  }
  if (irqFlags & 0x0010) {
    Serial.println(F("  [IRQ CHECK] -> HEADER_VALID (0x0010) is SET!"));
  }

  if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT0) {
    Serial.println("Woke from hub WOR -- reading and sending BME280 data");
    digitalWrite(BOARD_LED, LED_ON);
  }  

  return (wakeup_reason == ESP_SLEEP_WAKEUP_EXT0);
}

void enterLowPowerWOR() {
  Serial.println(F("\n--- [SX1262 Direct SPI WOR Arming] ---"));

  sxStandby(); 
  sxClearIrq();

  // Configure 8-symbol CAD and map wake flags to DIO1
  sxSetCadParams();
  sxConfigureWorIrq();

  // Issue 7-byte SetRxDutyCycle (~32ms listen, ~1s sleep, CAD mode)
  sxRXDutyCycle();

  // Arm S3 EXT0 wake on GPIO 16 (74HC04 output)
  // 1. Core 3.x EXT0 wake setup: GPIO 16, High level (1 = HIGH)
  esp_sleep_enable_ext0_wakeup(WAKEUP_PIN, 0);

  // 2. Keep line stable during sleep (pull down so resting level is 0V)
  rtc_gpio_pullup_dis(WAKEUP_PIN);
  rtc_gpio_pulldown_en(WAKEUP_PIN);

  Serial.println("Entering Deep Sleep... Waiting for WOR pulse on GPIO 16");
  Serial.flush();

  sxClearIrq();

  esp_deep_sleep_start();

  Serial.printf("WAKEUP_PIN GPIO %d before sleep = %d\n", WAKEUP_PIN, digitalRead(WAKEUP_PIN));
  Serial.println(F("=== SX1262 WOR armed - ESP32-S3 entering deep sleep ==="));
  Serial.flush();

  // Tear down SPI & isolate pins for low leakage
  radioSPI.end();
  pinMode((gpio_num_t)RADIO_CS_PIN, OUTPUT);
  digitalWrite((gpio_num_t)RADIO_CS_PIN, HIGH);
  gpio_hold_en((gpio_num_t)RADIO_CS_PIN);

  pinMode((gpio_num_t)RADIO_SCLK_PIN, INPUT);
  pinMode((gpio_num_t)RADIO_MOSI_PIN, INPUT);
  pinMode((gpio_num_t)RADIO_MISO_PIN, INPUT_PULLUP);

  esp_deep_sleep_start();
}

void setup() {
  // 1. Release pin holds so SPI bus can talk
  releasePinHoldsOnWake();

  Serial.begin(115200);
  delay(1500);

  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  bool wakeFromSleep = (wakeup_reason == ESP_SLEEP_WAKEUP_EXT0);

  // In Core 3.x, single-pin RTC wakeups report as ESP_SLEEP_WAKEUP_GPIO or ESP_SLEEP_WAKEUP_EXT0
  if (wakeup_reason == ESP_SLEEP_WAKEUP_GPIO || wakeup_reason == ESP_SLEEP_WAKEUP_EXT0) {
    Serial.println(F("\n[CAD EVENT] First CAD detection triggered wake!"));

    initRadio(wakeFromSleep);

    // Bring SPI back online
    releasePins();
    
    // Check IRQ flags and inspect wake
    inspectWake();

    // CLEAR THE IRQ SO DIO1 DROPS LOW (GPIO 16 -> 0V)
    sxClearIrq();

    // Read BME280 sensor and transmit via ESP-NOW
    // readAndSendBME280();

    // Service completed; put radio and S3 back to sleep
    enterLowPowerWOR();

  } else {
    Serial.println(F("[POWER UP / COLD BOOT] Initializing hardware for first duty cycle..."));
    
    releasePins();

    initRadio(wakeFromSleep); // Cold reset on power up
    
    // Put radio into CAD Duty Cycle and enter Deep Sleep
    enterLowPowerWOR();
  }
}

void loop() {
  // Unused
}