#define EoRa_PI_V1
#include <boards.h> 
#include "CAD.h"  // SX1262 driver includes
#include <SPI.h>
#include "driver/rtc_io.h"
#include <esp_sleep.h>

#define WAKEUP_PIN GPIO_NUM_16

SPIClass radioSPI(FSPI);

void releasePinHoldsOnWake() {
  gpio_hold_dis((gpio_num_t)RADIO_CS_PIN);
  gpio_hold_dis(GPIO_NUM_5);  // SCK
  gpio_hold_dis(GPIO_NUM_3);  // MISO
  gpio_hold_dis(GPIO_NUM_6);  // MOSI
  gpio_hold_dis(GPIO_NUM_1);  // Batt ADC
}

void releasePins() {
  // 1. Unhold pins held during deep sleep
  releasePinHoldsOnWake();

  // 2. Serial interface
  Serial.begin(115200);
  delay(1000);

  // 3. Re-initialize hardware SPI bus explicitly
  radioSPI.begin(RADIO_SCLK_PIN, RADIO_MISO_PIN, RADIO_MOSI_PIN, RADIO_CS_PIN);
}

void initRadio(bool wokeFromSleep) {
  Serial.println("\n--- Initializing SX1262 ---");

  if (!wokeFromSleep) { 
    sxReset(); // Cold boot / non-EXT1 reboot: hardware reset
  } else {
    Serial.println("Woke from sleep: Preserving IRQ flags.");
  }
  
  sxStandby();

  // LDO configuration (No DCDC inductor)
  uint8_t regModeData[] = { 0x00 }; 
  sxCommand(SX126X_CMD_SET_REGULATOR_MODE, regModeData, 1);

  // Crystal Loading Trim (0x12)
  sxWriteRegister(REG_XTAL_TRIM_A, 0x12);
  sxWriteRegister(REG_XTAL_TRIM_B, 0x12);

  // Set Rx Gain to High Sensitivity
  sxWriteRegister(REG_RX_GAIN, 0x96);

  // Set LoRa Mode & Frequency
  sxSetPacketTypeLoRa();
  sxSetFrequency(LORA_FREQ_HZ);

  // Modulation Parameters
  uint8_t modParams[] = { LORA_SF, LORA_BW, LORA_CR, 0x00 };
  sxCommand(SX126X_CMD_SET_MOD_PARAMS, modParams, 4);

  // Packet Parameters
  uint8_t pktParams[6] = {
    (uint8_t)((LORA_PREAMBLE >> 8) & 0xFF),
    (uint8_t)(LORA_PREAMBLE & 0xFF),
    0x00, // Explicit header
    0xFF, // Max payload limit
    0x01, // CRC On
    0x00  // Normal IQ
  };
  sxCommand(SX126X_CMD_SET_PACKET_PARAMS, pktParams, 6);

  // CAD and IRQ Config
  sxSetCadParams();
  sxConfigureCadIrq();
  sxClearIrq();

  Serial.println("SX1262 LoRa/CAD configuration complete.");
}

void inspectWake() {
  Serial.println("\n======================================");
  Serial.println(" EXT1 WAKE DIAGNOSTIC (GPIO 16)");
  Serial.println("======================================");

  Serial.printf("Wake cause = %d\n", esp_sleep_get_wakeup_cause());
  Serial.printf("GPIO16 / DIO1 = %d\n", digitalRead(WAKEUP_PIN));

  uint16_t irq = sxGetIrq();
  Serial.printf("SX1262 IRQ BEFORE RESET = 0x%04X\n", irq);

  if (irq & IRQ_CAD_DETECTED)      Serial.println("*** CAD_DETECTED = YES ***");
  if (irq & IRQ_CAD_DONE)          Serial.println("*** CAD_DONE = YES ***");
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

void enterLowPowerWOR() {
  Serial.println(F("\n--- Arming Low Power WOR ---"));

  // 1. Force SX1262 into STDBY_RC & Configure CAD
  sxStandby();
  delay(2);
  sxConfigureCadIrq();
  sxClearIrq();
  delay(2);
  sxStartContinuousCadDutyCycle();

  // 2. Hardware Line Isolation (GPIO 33 OEM trace)
  pinMode(GPIO_NUM_33, INPUT);
  gpio_hold_dis(GPIO_NUM_33);

  // 3. Wakeup Pin Configuration (GPIO 16)
  pinMode(WAKEUP_PIN, INPUT_PULLDOWN);
  rtc_gpio_pullup_dis(WAKEUP_PIN);
  rtc_gpio_pulldown_en(WAKEUP_PIN);

  // 4. Keep RTC Peripherals Domain ON
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);

  // 5. Configure EXT1 Wakeup
  const uint64_t bitmask = (1ULL << WAKEUP_PIN);
  esp_sleep_enable_ext1_wakeup_io(bitmask, ESP_EXT1_WAKEUP_ANY_HIGH);

  // 6. Isolate Battery ADC Pin (GPIO 1)
  disableBatteryADC();

  Serial.println(F("=== WOR Armed -> Deep Sleep ==="));
  Serial.flush();

  // 7. SPI Bus Isolation & Pin Holds
  SPI.end();

  pinMode((gpio_num_t)RADIO_CS_PIN, INPUT_PULLUP);
  gpio_hold_en((gpio_num_t)RADIO_CS_PIN);

  pinMode(GPIO_NUM_5, INPUT_PULLDOWN);  // SCK
  pinMode(GPIO_NUM_3, INPUT_PULLDOWN);  // MISO
  pinMode(GPIO_NUM_6, INPUT_PULLDOWN);  // MOSI

  gpio_hold_en(GPIO_NUM_5);
  gpio_hold_en(GPIO_NUM_3);
  gpio_hold_en(GPIO_NUM_6);

  gpio_deep_sleep_hold_en();

  // 8. Sleep
  esp_deep_sleep_start();
}

void setup() {
  releasePins();

  // Step 2: Determine wake source
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  bool wokeFromEXT1 = (wakeup_reason == ESP_SLEEP_WAKEUP_EXT1); // Code 3

  if(wakeup_reason == ESP_SLEEP_WAKEUP_EXT1){

    // Step 1: Initialize Board hardware & SPI pins
    initBoard();

    // Step 3: Initialize Radio (Preserves registers if EXT1, resets if non-EXT1 reboot)
    initRadio(wokeFromEXT1);

    // Step 4: Branch execution
    if (wokeFromEXT1) {
      Serial.println(F("\n*** WAKE: EXT1 (GPIO 16 / DIO1) ***"));

      inspectWake();

      // Do work...

      // Step 5: Arm continuous CAD, lock pins, enter deep sleep
      enterLowPowerWOR();
    }
  } 
  
  if(wakeup_reason != ESP_SLEEP_WAKEUP_EXT1){
    // Catches ALL reboots (Power-On, Reset button, Watchdog, Brownout, etc.)
    Serial.printf("\n*** REBOOT DETECTED (Cause Code: %d) ***\n", wakeup_reason);
  
    initRadio(wokeFromEXT1);

    // Step 5: Arm continuous CAD, lock pins, enter deep sleep
    enterLowPowerWOR();
  }
}

void loop() {
  // Unused
}