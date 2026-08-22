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

#define BOARD_LED 37
#define LED_ON HIGH
#define LED_OFF LOW

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
// SAFETY NET
//
// If the SX1262 ever gets wedged and RxDutyCycle can't be
// re-armed (or WOR itself never arrives for some other reason),
// this timer wakeup guarantees the node checks in on its own
// periodically rather than sleeping forever with no recovery path.
// ============================================================

//#define SAFETY_NET_SLEEP_US (45ULL * 60ULL * 1000000ULL)  // 45 minutes

#define BENCH_TESTING 1  // Set to 0 for production mode

// ============================================================
// PIN HOLD MANAGEMENT
// ============================================================

void preparePinsForSleep() {
  // 1. Keep Chip Select HIGH to prevent floating SPI commands to SX1262
  digitalWrite(RADIO_CS_PIN, HIGH);
  gpio_hold_en((gpio_num_t)RADIO_CS_PIN);

  // 2. Put RST in input mode and hold it
  pinMode(RADIO_RST_PIN, INPUT);
  gpio_hold_en((gpio_num_t)RADIO_RST_PIN);

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
#endif

  // Enable global RTC pin hold for sleep
  gpio_deep_sleep_hold_en();
}

void releasePinHoldsOnWake() {
  gpio_deep_sleep_hold_dis();

  gpio_hold_dis((gpio_num_t)RADIO_CS_PIN);
  gpio_hold_dis((gpio_num_t)RADIO_RST_PIN);

#if !BENCH_TESTING
  gpio_hold_dis(GPIO_NUM_19);
  gpio_hold_dis(GPIO_NUM_20);
#endif

  // Restore RST pin output state
  pinMode(RADIO_RST_PIN, OUTPUT);
  digitalWrite(RADIO_RST_PIN, HIGH);
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

  // --- PRE-SLEEP WOR ARMING SEQUENCE ---

  // --- CORRECTED PRE-SLEEP WOR SEQUENCE ---

  // 1. Force Radio into Standby RC mode so registers accept configuration commands
  sxStandby();  // 0x00 = STDBY_RC
  sxWaitBusy();

  // 2. Configure IRQs (HeaderValid, RxDone, and PreambleDetected on DIO1)
  sxConfigureRxDutyCycleIrq();
  sxWaitBusy();

  // 3. Prevent radio from sleeping mid-preamble when long WOR signal arrives
  sxSetStopRxTimerOnPreamble(true);
  sxWaitBusy();

  // 4. Start Duty Cycle mode
  // 2000 ticks (~31ms RX) | 64000 ticks (~1000ms Sleep)
  sxSetRxDutyCycle(2000, 64000);

  // 5. Allow internal state machine to settle & clear transient IRQ
  delayMicroseconds(200);
  sxClearIrq();

  // 6. Force GPIO 16 into RTC domain & clear pin holds
  gpio_deep_sleep_hold_dis();
  gpio_hold_dis(GPIO_NUM_16);

  rtc_gpio_init(GPIO_NUM_16);
  rtc_gpio_set_direction(GPIO_NUM_16, RTC_GPIO_MODE_INPUT_ONLY);
  rtc_gpio_pulldown_en(GPIO_NUM_16);

  // 7. Arm EXT0 wake on HIGH (Level 1)
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_16, 1);

  Serial.println("Entering Deep Sleep with RxDutyCycle armed...");
  Serial.flush();

  // 8. Start ESP32-S3 Deep Sleep
  esp_deep_sleep_start();
}

// ============================================================
// SETUP / LOOP
// ============================================================


/*
void setup() {

  // 1. Unhold pin and disable sleep triggers
  rtc_gpio_hold_dis(GPIO_NUM_16);
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);

  Serial.begin(115200);
  initBoard();
  delay(1500);

  // 2. Initialize SPI hardware pins BEFORE reading registers
  pinMode((gpio_num_t)RADIO_CS_PIN, OUTPUT);
  digitalWrite((gpio_num_t)RADIO_CS_PIN, HIGH);
  pinMode((gpio_num_t)RADIO_BUSY_PIN, INPUT);

  //radioSPI.begin(RADIO_SCLK_PIN, RADIO_MISO_PIN, RADIO_MOSI_PIN, RADIO_CS_PIN);

  pinMode(WAKEUP_PIN, INPUT);
  pinMode(BOARD_LED, OUTPUT);

  // 3. Determine Wake Reason
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  uint16_t currentIrq = 0;

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

    currentIrq = sxGetIrq();  // Read IRQ status while SX1262 memory is intact
    inspectWake(currentIrq);
    sxClearIrq();

    // 1. Immediately abort active RF listen and put radio into STDBY_RC (~0.6 mA)
    Serial.println("Wake from EXT0 GPIO 16");

    Serial.print("wokeFromEXT0:  ");
    Serial.println(wokeFromEXT0);

    initRadio(wokeFromEXT0);

    // 4. Run application logic (Read BME280 & transmit via ESP-NOW)
    readAndSendBME280();

    // 5. Re-arm RxDutyCycle & return to deep sleep
    enterLowPowerWOR();
  }
}
*/

// ============================================================
// MAIN SETUP
// ============================================================

void setup() {

  // 1. IMMEDIATELY unhold pins before running SPI operations
  releasePinHoldsOnWake();

  Serial.begin(115200);
  delay(1500);

  // 2. Configure both jumpered pins to identical pulldown states
  pinMode(RADIO_DIO1_PIN, INPUT_PULLDOWN);  // GPIO 33
  pinMode(WAKEUP_PIN, INPUT_PULLDOWN);      // GPIO 16

  // Configure CS & BUSY
  pinMode(RADIO_CS_PIN, OUTPUT);
  digitalWrite(RADIO_CS_PIN, HIGH);
  pinMode(RADIO_BUSY_PIN, INPUT);

  radioSPI.begin(RADIO_SCLK_PIN, RADIO_MISO_PIN, RADIO_MOSI_PIN, RADIO_CS_PIN);
  delay(5);

  // 3. Evaluate wake reason
  esp_sleep_wakeup_cause_t wakeCause = esp_sleep_get_wakeup_cause();

  if (wakeCause == ESP_SLEEP_WAKEUP_EXT0) {
    // --- EXT0 WAKE PATH ---
    
    // Pulse CS and wait on BUSY to wake SX1262 SPI bus out of sleep
    sxWakeupSPI();
    bool isReady = sxWaitBusy();

    // Read IRQ Status
    uint16_t irqStatus = sxGetIrq();

    // Retry once if initial read was 0x0000 while DIO1 is HIGH
    if (irqStatus == 0x0000 && digitalRead(WAKEUP_PIN) == HIGH) {
      delayMicroseconds(200);
      sxWaitBusy();
      irqStatus = sxGetIrq();
    }

    // Single Gated Print: Only output if radio is ready and IRQ is valid
    if (isReady && (irqStatus != 0x0000)) {
      Serial.printf("Woke on WOR! Pin 16: %d | SX1262 IRQ: 0x%04X\n", 
                    digitalRead(WAKEUP_PIN), irqStatus);

      if (irqStatus & IRQ_RX_DONE) {
        Serial.println("[WOR] Valid LoRa Packet Received!");
      } else if (irqStatus & (IRQ_PREAMBLE_DETECTED | IRQ_HEADER_VALID)) {
        Serial.println("[WOR] Preamble / Header Detected.");
      } else {
        Serial.println("[WOR] Non-RX or Spurious wake event.");
      }
    } else {
      Serial.println("[WOR] Hardware wake triggered via DIO1.");
    }

    // Clear IRQ flags so DIO1 drops back LOW
    sxClearIrq();

    // Re-initialize state, transmit BME280 sensor data, and re-arm WOR
    Serial.println("Re-initializing SX1262 state for next cycle...");
    
    initRadio();

    readAndSendBME280();

    enterLowPowerWOR();

  } else {
    // --- COLD BOOT / POWER ON PATH ---
    Serial.println("Cold Boot / Power On. Initializing SX1262...");

    initRadio();

    enterLowPowerWOR();
  }
}

void loop() {
  // Unused in deep sleep
}