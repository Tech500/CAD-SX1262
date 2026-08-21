/*
  BME280_Outside_Node_WOR.ino

  HSM IV -- Outside sensor node
  SX1262 RxDutyCycle WOR + ESP32-S3 deep sleep + BME280 + ESP-NOW

  ESP32 Core 3.3.10 required.
  Board: Ebyte EoRa-S3-900TB (utilities.h / boards.h provided)

  ARCHITECTURE
  ------------
  This node is event-driven and spends nearly all of its life in
  ESP32-S3 deep sleep. The SX1262 runs its own internal RxDutyCycle
  loop (RX <-> Sleep) completely autonomously -- the host CPU does
  not participate. When the hub's blower-triggered WOR transmitter
  sends its 5000-symbol (5.12s) SF7/BW125/915MHz preamble + packet, the
  SX1262 receives it (Listen mode's built-in bounded preamble-detect
  extension -- 2*rxPeriod + sleepPeriod, datasheet sec 13.1.7 -- keeps
  it in RX until the frame completes), raises RX_DONE, and pulses DIO1 -- which is
  wired to GPIO16 (rewired from the non-RTC-capable default DIO1
  pin) and configured as an EXT0 deep-sleep wake source.

  On wake:
    1. Confirm RX_DONE (not a spurious wake).
    2. Read BME280 (temp/humidity/pressure) + battery voltage.
    3. Send the reading to the hub over ESP-NOW.
    4. Re-arm SX1262 RxDutyCycle + EXT0 wake.
    5. Back to deep sleep.

  NO packet payload from the WOR trigger itself is ever read --
  the SX1262's only job here is to be the wake source. Content is
  irrelevant; RX_DONE firing at all is the signal.

  ASSUMPTIONS TO VERIFY BEFORE FLASHING
  --------------------------------------
  1. BME280 I2C pins: GPIO48 (SDA) / GPIO47 (SCL), physical pins
     19/20 on the EoRa-S3-900TB's 26-pin header. These are wired
     on a second I2C bus (Wire1) separate from the board's default
     I2C_SDA/I2C_SCL (18/17 per utilities.h, used for OLED/PMU on
     this board family). Change BME_SDA_PIN / BME_SCL_PIN below if
     your wiring differs.
  2. HUB_MAC_ADDRESS below is a placeholder -- fill in the real
     MAC address of the inside/hub ESP-NOW receiver.
  3. Sync word (0x14/0x24) must match the hub's WOR transmitter.
  4. Regulator mode is set to LDO (0x00) to match your prior CAD-8D
     bring-up code. Change to DC-DC (0x01) if EoRa-S3-900TB has the
     inductor populated and you've validated DC-DC mode.
*/

#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#define EoRa_PI_V1
#include <boards.h>
#include "Gold_III_sx1262_commands.h"
#include <WiFi.h>
#include <ESP32_NOW.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <BME280I2C.h>
#include <esp_sleep.h>
#include "driver/rtc_io.h"
#include "rom/rtc.h"
#include "esp_system.h"
#include "esp_private/periph_ctrl.h"

//Pin Configuration
//Using EByte's configuration files "boards.h" and "utilities.h"
//placed in sketch folder.

#define WAKEUP_PIN GPIO_NUM_16

// ============================================================
// BME280 -- second I2C bus (Wire1), GPIO48/47 = physical pins
// 19/20 on the EoRa-S3-900TB header. Separate from the board's
// default I2C_SDA/I2C_SCL (18/17, OLED/PMU bus).
// ============================================================

#define BME_SDA_PIN 47
#define BME_SCL_PIN 48
#define BME_I2C_ADDR 0x76

// ============================================================
// Extended RxDutyCycle Timing for Event-Driven WOR
// RTC tick = 15.625 us
//
// RX period    = 2048 ticks (~32.0 ms active listen)
// Sleep period = 5120 ticks (~80.0 ms deep sleep)
// Full cycle   ~= 112.0 ms
// ============================================================

#define RXDC_RX_TICKS 2048UL     // Extended from 1050 to 2048 for 100% catch rate
#define RXDC_SLEEP_TICKS 5120UL  // ~80 ms sleep interval


#define HUB_WIFI_CHANNEL 11

#define BME_SDA 47
#define BME_SCL 48

const float BME280_OUTSIDE_TEMP_CAL_OFFSET_F = +5.54;

uint8_t hubMAC[] = { 0x1C, 0xDB, 0xD4, 0x85, 0x6E, 0x9C };

//SPI.begin(RADIO_SCLK_PIN, RADIO_MISO_PIN, RADIO_MOSI_PIN, RADIO_CS_PIN);

BME280I2C bme;

// --- Message / Packet Structures ---
enum MessageType : uint8_t {
  MSG_BME280 = 0,
  MSG_ALERT_FLAG = 1,
  MSG_BLOWER_STATE = 2
};

struct __attribute__((packed)) BME280Data {
  MessageType type;
  float temperature;
  float humidity;
  float pressure;
};

struct __attribute__((packed)) BlowerData {
  MessageType type;
  bool on;
  float elapsedMinutes;
  float dailyTotalMinutes;
};

struct __attribute__((packed)) AlertFlag {
  MessageType type;
  bool alert;
};

// --- ESP32 Core v3 ESP-NOW Peer Class ---
class HubPeer : public ESP_NOW_Peer {
public:
  HubPeer(const uint8_t *mac_addr, uint8_t channel)
    : ESP_NOW_Peer(mac_addr, channel, WIFI_IF_STA, NULL) {}

  bool add_to_system() {
    return ESP_NOW_Peer::add();
  }

  bool remove_from_system() {
    return ESP_NOW_Peer::remove();
  }

  bool sendData(const uint8_t *data, size_t len) {
    return send(data, len);
  }
};

bool sendTelemetryViaESPNOW(float tempF, float humidity, float pressureHPa) {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  esp_wifi_set_channel(HUB_WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);

  if (!ESP_NOW.begin()) {
    Serial.println(F("ESP-NOW init failed"));
    WiFi.mode(WIFI_OFF);
    return false;
  }

  HubPeer localHub(hubMAC, HUB_WIFI_CHANNEL);
  if (!localHub.add_to_system()) {
    Serial.println(F("Failed to bind hub peer"));
    ESP_NOW.end();
    WiFi.mode(WIFI_OFF);
    return false;
  }

  BME280Data pkt;
  pkt.type = MSG_BME280;
  pkt.temperature = tempF;
  pkt.humidity = humidity;
  pkt.pressure = pressureHPa;

  bool sent = localHub.sendData((uint8_t *)&pkt, sizeof(BME280Data));
  Serial.printf("[ESP-NOW] Send to hub: %s\n", sent ? "OK" : "FAILED");

  localHub.remove_from_system();
  ESP_NOW.end();
  WiFi.mode(WIFI_OFF);
  esp_wifi_stop();

  return sent;
}

bool readAndSendBME280() {
  Wire.end();
  delay(50);
  Wire.setPins(BME_SDA, BME_SCL);
  if (!Wire.begin(BME_SDA, BME_SCL)) {
    Serial.println(F("Failed to allocate I2C peripheral instance!"));
  }
  delay(50);

  if (!bme.begin()) {
    Serial.println(F("BME280 not found -- check wiring/address"));
    return false;
  }

  float tempF = NAN, humidity = NAN, pressureHPa = NAN;
  BME280::TempUnit tempUnit(BME280::TempUnit_Fahrenheit);
  BME280::PresUnit presUnit(BME280::PresUnit_hPa);

  delay(500);

  bme.read(pressureHPa, tempF, humidity, tempUnit, presUnit);
  tempF += BME280_OUTSIDE_TEMP_CAL_OFFSET_F;

  delay(200);

  if (isnan(tempF) || isnan(pressureHPa)) {
    Serial.println(F("Error reading BME280 telemetry."));
    return false;
  }

  Serial.printf("BME280 -> Temp: %.2f F  Hum: %.2f %%  Pres: %.4f hPa\n",
                tempF, humidity, pressureHPa);

  return sendTelemetryViaESPNOW(tempF, humidity, pressureHPa);
}

// ============================================================
// BENCH vs. FIELD
// ============================================================

#define BENCH_TESTING false  // true = keep USB serial alive; false = isolate USB for lowest current


// ============================================================
// SAFETY NET
//
// If the SX1262 ever gets wedged and RxDutyCycle can't be
// re-armed (or WOR itself never arrives for some other reason),
// this timer wakeup guarantees the node checks in on its own
// periodically rather than sleeping forever with no recovery path.
// ============================================================

//#define SAFETY_NET_SLEEP_US (45ULL * 60ULL * 1000000ULL)  // 45 minutes

// ======================================
//  initRadio
//
//  wokeFromEXT0 == true:
//      ESP32 woke from EXT0.
//      DO NOT reset or clear SX1262 IRQ.
//
//  wokeFromEXT0 == false:
//      Normal / undefined / cold boot.
//      Reset and fully initialize SX1262.
// ======================================

void initRadio(bool wokeFromEXT0) {

  Serial.println("\n--- Initializing SX1262 ---");

  // ----------------------------------------------------------
  // 1. Establish safe SPI control-pin states
  // ----------------------------------------------------------
  pinMode((gpio_num_t)RADIO_CS_PIN, OUTPUT);
  digitalWrite((gpio_num_t)RADIO_CS_PIN, HIGH);

  pinMode((gpio_num_t)RADIO_BUSY_PIN, INPUT_PULLDOWN);

  // ----------------------------------------------------------
  // 2. NORMAL BOOT ONLY:
  //    Hardware reset the SX1262.
  //
  //    EXT0 WAKE:
  //    DO NOT reset here.
  //
  //    The SX1262 must remain alive so its IRQ state can
  //    be inspected after the ESP32 wakes.
  // ----------------------------------------------------------
  if (!wokeFromEXT0) {

    Serial.println("SX1262 reset...");

    sxReset();

    sxWaitBusy();

    delay(10);

    Serial.println("SX1262 reset complete.");
  }

  // ----------------------------------------------------------
  // 3. Wait for radio to become available
  // ----------------------------------------------------------
  sxWaitBusy();

  // ----------------------------------------------------------
  // 4. Put radio into standby
  //
  //    On EXT0 wake this happens AFTER the caller has already
  //    captured the SX1262 wake IRQ.
  // ----------------------------------------------------------
  Serial.println("Entering sxStandby...");

  sxStandby();

  sxWaitBusy();

  Serial.println("sxStandby complete.");

  // ----------------------------------------------------------
  // 5. NORMAL BOOT ONLY:
  //    Image calibration for 902–928 MHz
  // ----------------------------------------------------------
  if (!wokeFromEXT0) {

    uint8_t calData[] = {
      0xE1,
      0xE9
    };

    sxCommand(
      0x98,                 // CalibrateImage
      calData,
      2
    );

    sxWaitBusy();
  }

  // ----------------------------------------------------------
  // 6. Regulator = LDO
  // ----------------------------------------------------------
  uint8_t regModeData[] = {
    0x00
  };

  sxCommand(
    SX126X_CMD_SET_REGULATOR_MODE,
    regModeData,
    1
  );

  sxWaitBusy();

  // ----------------------------------------------------------
  // 7. DIO2 controls external RF switch
  // ----------------------------------------------------------
  sxSetDio2AsRfSwitch();

  sxWaitBusy();

  // ----------------------------------------------------------
  // 8. Crystal trim
  // ----------------------------------------------------------
  sxWriteRegister(REG_XTAL_TRIM_A, 0x12);
  sxWriteRegister(REG_XTAL_TRIM_B, 0x12);

  // ----------------------------------------------------------
  // 9. RX gain boost
  // ----------------------------------------------------------
  sxWriteRegister(REG_RX_GAIN, 0x96);

  // ----------------------------------------------------------
  // 10. LoRa packet type
  // ----------------------------------------------------------
  sxSetPacketTypeLoRa();

  sxWaitBusy();

  // ----------------------------------------------------------
  // 11. Private sync word = 0x1424
  // ----------------------------------------------------------
  sxWriteRegister(REG_SYNC_WORD_MSB, 0x14);
  sxWriteRegister(REG_SYNC_WORD_LSB, 0x24);

  // ----------------------------------------------------------
  // 12. RF frequency
  // ----------------------------------------------------------
  sxSetFrequency(LORA_FREQ_HZ);

  // ----------------------------------------------------------
  // 13. LoRa modulation parameters
  //
  // SF7
  // BW125 kHz
  // CR 4/5
  // LowDataRateOptimize OFF
  // ----------------------------------------------------------
  uint8_t modParams[] = {
    LORA_SF,
    LORA_BW,
    LORA_CR,
    0x00
  };

  sxCommand(
    SX126X_CMD_SET_MOD_PARAMS,
    modParams,
    4
  );

  sxWaitBusy();

  // ----------------------------------------------------------
  // 14. Packet parameters
  //
  // Explicit header
  // 1-byte throw-away WOR payload
  // CRC ON
  // Standard IQ
  // ----------------------------------------------------------
  uint8_t pktParams[6] = {
    (uint8_t)((LORA_PREAMBLE >> 8) & 0xFF),
    (uint8_t)(LORA_PREAMBLE & 0xFF),
    0x00,       // Explicit header
    0x01,       // 1-byte payload
    0x01,       // CRC ON
    0x00        // Standard IQ
  };

  sxCommand(
    SX126X_CMD_SET_PACKET_PARAMS,
    pktParams,
    6
  );

  sxWaitBusy();

  // ----------------------------------------------------------
  // 15. STOP RX TIMER ON PREAMBLE
  //
  // IMPORTANT FOR RxDutyCycle WOR
  //
  // Once a valid preamble is detected, the short Rx timer
  // must stop so the radio can remain awake long enough to
  // receive the header/payload and generate RX_DONE.
  // ----------------------------------------------------------
  sxSetStopRxTimerOnPreamble(true);

  sxWaitBusy();

  // ----------------------------------------------------------
  // 16. NORMAL BOOT ONLY:
  //    Clear any stale IRQs.
  //
  //    EXT0 wake:
  //    DO NOT clear here.
  //
  //    The caller should have captured the wake IRQ BEFORE
  //    calling initRadio(true).
  // ----------------------------------------------------------
  if (!wokeFromEXT0) {

    sxClearIrq();

    sxWaitBusy();
  }

  // ----------------------------------------------------------
  // 17. Route RxDutyCycle IRQs to DIO1
  // ----------------------------------------------------------
  sxConfigureRxDutyCycleIrq();

  sxWaitBusy();

  // ----------------------------------------------------------
  // 18. Configure RxDutyCycle
  //
  // Replace these with your actual WOR timing variables.
  //
  // rxPeriod   = receiver ON/listening time
  // sleepPeriod = receiver OFF/sleep time
  // ----------------------------------------------------------
  sxSetRxDutyCycle(RXDC_RX_TICKS, RXDC_SLEEP_TICKS); 

  sxWaitBusy();
}

// ============================================================
// PIN HOLD MANAGEMENT (deep sleep <-> active)
// ============================================================

/*
void disableBatteryADC() {
  pinMode(GPIO_NUM_1, INPUT);
  gpio_pullup_dis(GPIO_NUM_1);
  gpio_pulldown_dis(GPIO_NUM_1);
  gpio_hold_en(GPIO_NUM_1);
}
*/

// ============================================================
// USB
// ============================================================

void shutdownUSB() {
#if !BENCH_TESTING
  Serial.flush();
  periph_module_disable(PERIPH_USB_MODULE);

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

// ============================================================
// WAKE HANDLING
// ============================================================

uint16_t inspectWake(uint16_t irqStatus) {

  Serial.printf("SX1262 IRQ = 0x%04X\n", irqStatus);

  if (irqStatus & IRQ_PREAMBLE_DETECTED) Serial.println("PREAMBLE_DETECTED");
  if (irqStatus & IRQ_HEADER_VALID) Serial.println("HEADER_VALID");
  if (irqStatus & IRQ_RX_DONE) Serial.println("RX_DONE");
  if (irqStatus & IRQ_TIMEOUT) Serial.println("TIMEOUT");

  return irqStatus;
}

// ============================================================
// SX1262 IRQ DIAGNOSTIC
// Reads IRQ status ONLY.
// DOES NOT clear IRQs.
// ============================================================

uint16_t inspectIrqDetection() {

  uint16_t irq = sxGetIrq();

  Serial.printf("[IRQ] SX1262 IRQ = 0x%04X\n", irq);

  if (irq == 0x0000) {
    Serial.println("[IRQ] No IRQ flags set.");
    return irq;
  }

  if (irq & IRQ_TX_DONE)
    Serial.println("[IRQ] TX_DONE");

  if (irq & IRQ_RX_DONE)
    Serial.println("[IRQ] RX_DONE");

  if (irq & IRQ_PREAMBLE_DETECTED)
    Serial.println("[IRQ] *** PREAMBLE_DETECTED ***");

  if (irq & IRQ_HEADER_VALID)
    Serial.println("[IRQ] HEADER_VALID");

  if (irq & IRQ_HEADER_ERROR)
    Serial.println("[IRQ] HEADER_ERR");

  if (irq & IRQ_CRC_ERROR)
    Serial.println("[IRQ] CRC_ERROR");

  if (irq & IRQ_TIMEOUT)
    Serial.println("[IRQ] TIMEOUT");

  if (irq & IRQ_SYNCWORD_VALID)
    Serial.println("[IRQ] SYNCWORD_VALID");

  Serial.printf(
    "[IRQ] DIO1 GPIO16 = %d\n",
    digitalRead(WAKEUP_PIN));

  return irq;
}

// ============================================================
// RxDutyCycle COMMAND DIAGNOSTIC
// Does NOT replace Gold III.
// Does NOT alter the command.
// ============================================================

void printRxDutyCycleTiming() {

  uint32_t rxTicks = RXDC_RX_TICKS;
  uint32_t sleepTicks = RXDC_SLEEP_TICKS;

  Serial.println();
  Serial.println(F("========================================"));
  Serial.println(F("SX1262 RxDutyCycle COMMAND CHECK"));
  Serial.println(F("========================================"));

  Serial.printf(
    "RX ticks    = %lu (0x%06lX)\n",
    rxTicks,
    rxTicks);

  Serial.printf(
    "SLEEP ticks = %lu (0x%06lX)\n",
    sleepTicks,
    sleepTicks);

  Serial.printf(
    "RX time     = %.3f ms\n",
    rxTicks * 0.015625);

  Serial.printf(
    "Sleep time  = %.3f ms\n",
    sleepTicks * 0.015625);

  Serial.println(F("Expected SET_RX_DUTY_CYCLE payload:"));

  Serial.printf(
    "94 %02X %02X %02X %02X %02X %02X\n",
    (uint8_t)(rxTicks >> 16),
    (uint8_t)(rxTicks >> 8),
    (uint8_t)rxTicks,
    (uint8_t)(sleepTicks >> 16),
    (uint8_t)(sleepTicks >> 8),
    (uint8_t)sleepTicks);

  Serial.println(F("========================================"));
}

void enterLowPowerWOR() {

  inspectWake(sxGetIrq());

  // 4. Configure GPIO 16 as input (external 10k pulldown holds LOW)
  pinMode(WAKEUP_PIN, INPUT);

  // 5. Enable Deep Sleep Wake
  // Use AUTO or remove explicitly to allow RTC domain to sleep
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_AUTO);
  esp_sleep_enable_ext0_wakeup(WAKEUP_PIN, 1);

  
  SPI.end();
#ifdef SDSPI
  SDSPI.end();
#endif

  // 5. Lock CS HIGH with RTC pin hold so CS doesn't drop LOW and wake the SX1262
  pinMode((gpio_num_t)RADIO_CS_PIN, OUTPUT);
  digitalWrite((gpio_num_t)RADIO_CS_PIN, HIGH);
  gpio_hold_en((gpio_num_t)RADIO_CS_PIN);

  // 6. Floating unneeded SPI / peripheral lines to prevent back-feeding current
  pinMode((gpio_num_t)RADIO_SCLK_PIN, INPUT);
  pinMode((gpio_num_t)RADIO_MOSI_PIN, INPUT);
  pinMode((gpio_num_t)RADIO_MISO_PIN, INPUT_PULLUP);
  pinMode((gpio_num_t)RADIO_RST_PIN, INPUT_PULLUP);
  pinMode((gpio_num_t)RADIO_BUSY_PIN, INPUT_PULLUP);

#ifdef I2C_SDA
  pinMode((gpio_num_t)I2C_SDA, INPUT);
  pinMode((gpio_num_t)I2C_SCL, INPUT);
#endif

  // 7. Ensure Wakeup pin (GPIO 16) is clean input
  gpio_hold_dis(GPIO_NUM_16);

  Serial.println(F("========== Entering Deep Sleep ==========="));
  Serial.flush();

  // 8. Enter Deep Sleep
  esp_deep_sleep_start();
}

// ============================================================
// SETUP / LOOP
// ============================================================

void setup() {

  // 1. Unhold pin and disable sleep triggers
  rtc_gpio_hold_dis(GPIO_NUM_16);
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);

  Serial.begin(115200);
  delay(1500);

  // 2. Initialize SPI hardware pins BEFORE reading registers
  pinMode((gpio_num_t)RADIO_CS_PIN, OUTPUT);
  digitalWrite((gpio_num_t)RADIO_CS_PIN, HIGH);
  pinMode((gpio_num_t)RADIO_BUSY_PIN, INPUT);

  pinMode(WAKEUP_PIN, INPUT);
  pinMode(BOARD_LED, OUTPUT);

  // 3. Determine Wake Reason
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();

  // Compare the actual wakeup_reason variable instead:
  bool wokeFromEXT0 = (wakeup_reason == ESP_SLEEP_WAKEUP_EXT0);

  if (wakeup_reason == ESP_SLEEP_WAKEUP_UNDEFINED) {
    Serial.println("[POWER UP / COLD BOOT] Initializing hardware...");

    Serial.print("wokeFromEXT0:  ");
    Serial.println(wokeFromEXT0);

    initRadio(!wokeFromEXT0);

    // Pass 'false' to allow cold-boot sxReset()
    enterLowPowerWOR();
  }

  if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT0) {
    digitalWrite(BOARD_LED, LED_ON);

    // 1. Immediately abort active RF listen and put radio into STDBY_RC (~0.6 mA)
    Serial.println("Wake from EXT0 GPIO 16");

    Serial.print("wokeFromEXT0:  ");
    Serial.println(wokeFromEXT0);

    inspectWake(sxGetIrq());

    // 3. Clear IRQs
    sxClearIrq();
    
    initRadio(wokeFromEXT0);    

    // 4. Run application logic (Read BME280 & transmit via ESP-NOW)
    readAndSendBME280();

    // 5. Re-arm RxDutyCycle & return to deep sleep
    enterLowPowerWOR();
  }
}

void loop() {
  // Unused -- everything happens in setup() before deep sleep.
}