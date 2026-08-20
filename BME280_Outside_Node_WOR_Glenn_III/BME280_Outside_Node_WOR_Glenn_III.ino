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


// ============================================================
// WAKE / RADIO PINS
// ============================================================

#define WAKEUP_PIN GPIO_NUM_16  // DIO1, rewired from non-RTC GPIO33

// ============================================================
// BME280 -- second I2C bus (Wire1), GPIO48/47 = physical pins
// 19/20 on the EoRa-S3-900TB header. Separate from the board's
// default I2C_SDA/I2C_SCL (18/17, OLED/PMU bus).
// ============================================================

#define BME_SDA_PIN 48
#define BME_SCL_PIN 47
#define BME_I2C_ADDR 0x76

// Send OpCode + Parameter Payload
void sx1262_writeCommand(uint8_t opCode, const uint8_t *buffer, size_t size) {
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
  SPI.transfer(0x12);  // OpCode 0x12: GetIrqStatus
  SPI.transfer(0x00);  // Status NOP dummy byte
  uint8_t msb = SPI.transfer(0x00);
  uint8_t lsb = SPI.transfer(0x00);
  digitalWrite(RADIO_CS_PIN, HIGH);
  return ((uint16_t)msb << 8) | lsb;
}

  // Explicit Header (Standard - Header present in packet)
  uint8_t headerType = 0x00;

// Implicit Header (Fixed packet length, no header sent)
// uint8_t headerType = 0x01;

void sxSetPacketParams(uint8_t payloadLen) {
  uint8_t data[6];
  data[0] = (LORA_PREAMBLE >> 8) & 0xFF;
  data[1] = LORA_PREAMBLE & 0xFF;
  data[2] = headerType;  // 0x00 = Explicit, 0x01 = Implicit
  data[3] = payloadLen;  // Must match exact byte count if Implicit
  data[4] = 0x01;        // CRC ON
  data[5] = 0x00;        // Standard IQ

  sxCommand(SX126X_CMD_SET_PACKET_PARAMS, data, 6);
}


#define LORA_FREQ_HZ 915000000UL

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

  // ONLY clear IRQs on a fresh cold boot.
  // Do NOT clear here when wokeFromSleep, or you lose the raw wake status!
  if (!wokeFromSleep) {
    sxClearIrq();
  }

  // Route CAD / Preamble interrupts to DIO1 last
  sxConfigureRxDutyCycleIrq();

  Serial.println("SX1262 LoRa configuration complete.");
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
// ARM RxDutyCycle + ESP32-S3 EXT0 WAKE, THEN DEEP SLEEP
// ============================================================

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

void enterLowPowerWOR() {
  // 1. Force Standby RC to reset internal modem state
  sxStandby();

  // 2. Clear any lingering IRQs from previous wake events
  sxClearIrq();

  // 3. Configure IRQs to route Preamble/Header/RX_DONE to DIO1
  sxConfigureRxDutyCycleIrq();




  // 4. Arm Duty Cycle mode with the expanded listen window
  sxSetRxDutyCycle(RXDC_RX_TICKS, RXDC_SLEEP_TICKS);

  // Fresh read -- IRQs were just cleared/re-armed above, so this
  // reflects current post-arm state, not the prior wake event.
  uint16_t armIrq = sxGetIrq();

  Serial.printf("WOR Armed (32ms RX / 80ms Sleep). DIO1: %d, IRQ: 0x%04X\n",
                digitalRead(WAKEUP_PIN), armIrq);

  sxSetPacketParams();

  // 4. Configure GPIO 16 as input (external 10k pulldown holds LOW)
  pinMode(WAKEUP_PIN, INPUT);

  // 5. Enable EXT1 wakeup on HIGH level via Core 3.3.10 API
  //const uint64_t wakeupBitmask = (1ULL << WAKEUP_PIN);
  //esp_sleep_enable_ext1_wakeup_io(wakeupBitmask, ESP_EXT1_WAKEUP_ANY_HIGH);

  // Enable Deep Sleep Wake
  // Use AUTO or remove explicitly to allow RTC domain to sleep
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_AUTO);
  esp_sleep_enable_ext0_wakeup(WAKEUP_PIN, 1);

  Serial.printf("DIO1 GPIO %d pre-sleep level = %d | IRQ = 0x%04X\n",
                WAKEUP_PIN, digitalRead(WAKEUP_PIN), armIrq);
  Serial.println(F("=== SX1262 WOR Armed -> Entering Deep Sleep ==="));
  Serial.flush();
  delay(100);

  esp_deep_sleep_start();
}

// ============================================================
// SETUP / LOOP
// ============================================================

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

  pinMode(WAKEUP_PIN, INPUT);

  radioSPI.begin(RADIO_SCLK_PIN, RADIO_MISO_PIN, RADIO_MOSI_PIN);

  // 3. Determine Wake Reason
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();

  // Compare the actual wakeup_reason variable instead:
  bool wokeFromSleep = wakeup_reason;

  Serial.print("wokeFromSleep:  ");
  Serial.println(wokeFromSleep);

  if (wakeup_reason == ESP_SLEEP_WAKEUP_UNDEFINED) {
    Serial.println("[POWER UP / COLD BOOT] Initializing hardware...");

    // Pass 'false' to allow cold-boot sxReset()
    initRadio(wokeFromSleep);

    enterLowPowerWOR();
  }

if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT0) {
    // 1. Immediately abort active RF listen and put radio into STDBY_RC (~0.6 mA)
    sxStandby();

    // 2. Fetch IRQ flags to verify PREAMBLE_DETECTED or RX_DONE
    uint16_t wakeIrq = sxGetIrq();
    Serial.printf("[WOR WAKE] SX1262 IRQ: 0x%04X\n", wakeIrq);

    // 3. Clear IRQs
    sxClearIrq();

    // 4. Run application logic (Read BME280 & transmit via ESP-NOW)
    readAndSendBME280();

    // 5. Re-arm RxDutyCycle & return to deep sleep
    enterLowPowerWOR();
  }
}

void loop() {
  // Unused -- everything happens in setup() before deep sleep.
}
