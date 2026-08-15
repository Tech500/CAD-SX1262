/*
  Gemini_BME280_Outsie_Node_Test_Almost.ino
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

  // 1. Assert CS HIGH and set BUSY to INPUT before SPI setup
  pinMode((gpio_num_t)RADIO_CS_PIN, OUTPUT);
  digitalWrite((gpio_num_t)RADIO_CS_PIN, HIGH);
  pinMode((gpio_num_t)RADIO_BUSY_PIN, INPUT);

  if (!wokeFromSleep) {
    Serial.println("SX1262 reset...");
    sxReset();  // Cold boot: hardware reset toggle
    Serial.println("SX1262 reset complete.");
    delay(10);
  } 
  
  sxWaitBusy();

  Serial.println("Entering sxStandby...");
  sxStandby();
  Serial.println("sxStandby complete.");

  // LDO Mode (0x00) - Correct for boards without external DC-DC inductor
  uint8_t regModeData[] = { 0x00 };
  sxCommand(SX126X_CMD_SET_REGULATOR_MODE, regModeData, 1);

  // Configure DIO2 as RF Switch Control
  sxSetDio2AsRfSwitch();

  // Crystal Loading Trim & RX Gain
  sxWriteRegister(REG_XTAL_TRIM_A, 0x12);
  sxWriteRegister(REG_XTAL_TRIM_B, 0x12);
  sxWriteRegister(REG_RX_GAIN, 0x96);

  // Set LoRa Mode & Sync Word (Private Network: 0x1424)
  sxSetPacketTypeLoRa();
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
    0xFF,  // Max payload
    0x01,  // CRC On
    0x00   // Standard IQ
  };
  sxCommand(SX126X_CMD_SET_PACKET_PARAMS, pktParams, 6);

  // Configure CAD Parameters
  sxSetCadParams();

  // ONLY clear IRQs on a fresh cold boot. 
  // Do NOT clear here when wokeFromSleep, or you lose the raw wake status!
  if (!wokeFromSleep) {
    sxClearIrq();
  }

  // Route CAD / Preamble interrupts to DIO1 last
  sxConfigureWorIrq();

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

  //sxStandby(); 
  sxClearIrq();

  // Configure 8-symbol CAD and map wake flags to DIO1
  sxSetCadParams();
  sxConfigureWorIrq();

  // Issue 7-byte SetRxDutyCycle (~32ms listen, ~1s sleep, CAD mode)
  sxRXDutyCycle();

  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, option)

  // Arm S3 EXT0 wake on GPIO 16 (74HC04 output)
  // 1. Core 3.x EXT0 wake setup: GPIO 16, High level (1 = HIGH)
  esp_sleep_enable_ext0_wakeup(WAKEUP_PIN, 1);

  // 2. Keep line stable during sleep (pull down so resting level is 0V)
  rtc_gpio_pullup_dis(WAKEUP_PIN);
  rtc_gpio_pulldown_en(WAKEUP_PIN);

  Serial.println("Entering Deep Sleep... Waiting for WOR pulse on GPIO 16");
  Serial.flush();

  sxClearIrq();

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
  // 1. Unhold pin and disable sleep triggers
  rtc_gpio_hold_dis(GPIO_NUM_16);
  //esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);

  Serial.begin(115200);
  delay(1500);

  // 2. Initialize SPI hardware pins BEFORE reading registers
  pinMode((gpio_num_t)RADIO_CS_PIN, OUTPUT);
  digitalWrite((gpio_num_t)RADIO_CS_PIN, HIGH);
  pinMode((gpio_num_t)RADIO_BUSY_PIN, INPUT);
  
  

  //SPI.begin(RADIO_SCLK_PIN, RADIO_MISO_PIN, RADIO_MOSI_PIN);

  // 3. Determine Wake Reason
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  bool wokeFromSleep = (ESP_SLEEP_WAKEUP_EXT0);

    if(wakeup_reason == ESP_SLEEP_WAKEUP_UNDEFINED){
    Serial.println("[POWER UP / COLD BOOT] Initializing hardware...");
    
    // Pass 'false' to allow cold-boot sxReset()
    initRadio(false); 

    enterLowPowerWOR();
  }

  if (wokeFromSleep) {
    wokeFromSleep = false;

    Serial.println("[WOR WAKE] Processing RF event...");
    
    // SPI is active now — fetch raw IRQ flags before touching radio state
    uint16_t irqStatus = sxGetIrq();
    Serial.printf("--> SX1262 IRQ status on wake: 0x%04X\n", irqStatus);
    
    // Pass 'true' so sxReset() IS SKIPPED
    initRadio(true); 

    // 4. Application Work / Receive Logic
    Serial.println("Do work");

    enterLowPowerWOR();
  } 
}

void loop() {
  // Unused
}