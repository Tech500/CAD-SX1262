#  Ultra‑Low‑Power LoRa Wake Receiver  
### SX1262 RxDutyCycle + EoRa‑S3‑900TB (ESP32‑S3)  
### *LoRa’s only job is to wake the MCU.*

This open‑source project demonstrates how to build an **ultra‑low‑power LoRa wake receiver** using:

- The **built‑in SX1262** LoRa transceiver on the EoRa‑S3‑900TB  
- **RxDutyCycle** (Semtech Wake‑On‑Radio mode)  
- **EXT0 deep‑sleep wake**  
- **One jumper wire**  

The SX1262 periodically listens for a **LoRa preamble**.  
If a preamble is detected, the radio asserts **DIO1**, waking the EoRa‑S3‑900TB from deep sleep.

**In this project, LoRa’s only function is to wake the MCU.**  
The SX1262 does not decode packets during RxDutyCycle — it simply detects the preamble and wakes the microcontroller.

This design is ideal for battery‑powered sensors, remote monitors, and long‑range low‑power systems.

---

#  What You Need

### Hardware
- **EoRa‑S3‑900TB** (ESP32‑S3 low‑power board with built‑in SX1262)  
- **One jumper wire** (GPIO33 → GPIO16)

### Required Files (must be in the sketch folder)
Place these **three files** in the same folder as your `.ino` or `main.cpp`:

File

Purpose

boards.h

Ebyte board pin definitions

utilities.h

SX1262 hardware mapping (DIO1 = GPIO33)

sx1262_commands.h

Semtech low‑level command set (SetRxDutyCycle, IRQ ops, etc.)

 Sketch Folder Placement

The entire sketch folder can be copied anywhere:

Arduino sketchbook

A custom project directory

A local library folder

As long as these three files remain in the same folder as the sketch, the project will compile and run correctly.

 Mandatory Requirement

ESP32 Arduino Core 3.3.10 is required

This project must be compiled using:

ESP32 Arduino Core 3.3.10

Earlier or later versions may:

break EXT0 wake behavior

change RTC GPIO handling

alter deep‑sleep current

modify SX1262 SPI timing

cause WOR wake failures

Core 3.3.10 is the validated, stable version for the EoRa‑S3‑900TB + SX1262 WOR architecture.

 Wiring Overview

 SX1262 is built into the EoRa‑S3‑900TB

All SX1262 connections (SPI, RESET, BUSY, DIO1) are already routed internally on the board.No external SX1262 wiring is required.

 Wake‑On‑Radio Jumper (Important)

The SX1262’s DIO1 pin is internally routed to GPIO33 on the EoRa‑S3‑900TB.

But GPIO33 cannot wake the ESP32‑S3 from deep sleep.

So we add one jumper wire:

GPIO33 → GPIO16

GPIO16 is RTC‑capable and is used for EXT0 wake.

This jumper is required for WOR wake.

 How RxDutyCycle Works

RxDutyCycle is a low‑power receive mode built into the Semtech SX126x LoRa transceivers.Instead of staying in continuous receive, the radio periodically wakes up, opens a short listening window, and checks only for the presence of a LoRa preamble.

Semtech describes it as:

“The receiver is activated periodically to detect the presence of a valid LoRa preamble.”

 What the SX1262 does

Sleep

Wake periodically

Open RX window

Look for LoRa preamble

If detected → assert DIO1

MCU wakes via EXT0

MCU completes reception or logs the event

MCU re‑arms WOR and returns to sleep

 The Only Function of LoRa Here

In this project, LoRa is used only to wake the EoRa‑S3‑900TB.The SX1262 does not decode packets during RxDutyCycle — it simply detects the preamble and wakes the MCU.

 Firmware Wake Setup (EXT0 on GPIO16)

Wake pin is defined separately:

#define WAKEUP_PIN GPIO_NUM_16

Before entering deep sleep, GPIO16 must be placed into the RTC domain:

rtc_gpio_init(GPIO_NUM_16);
rtc_gpio_set_direction(GPIO_NUM_16, RTC_GPIO_MODE_INPUT_ONLY);
rtc_gpio_pullup_en(GPIO_NUM_16);     // or pulldown_en() depending on inverter polarity

esp_sleep_enable_ext0_wakeup(GPIO_NUM_16, 1);   // or 0 depending on DIO1 polarity

If rtc_gpio_init() is missing, EXT0 will never wake the MCU.

 Power Profile (Three‑Segment Model — Corrected)

These values describe radio behavior only during the RxDutyCycle WOR pattern, measured using the Nordic PPK2 grey‑selection box averages.
 Sleep Segment — WOR Sleep Portion

This is the radio’s sleep portion inside the WOR cycle, not the SX1262 standalone deep‑sleep mode.

Average: ~21 µA

This is the correct value from the Nordic PPK2 grey‑selection box

Any lower instantaneous readings (including single‑digit µA) are not representative averages and must not be used for documentation

 Listening Segment — WOR RX Window

The radio opens its RX window and listens for a LoRa preamble.

Peak: ~14 mA

Short duration, periodic

 Complete RxDutyCycle Segment — One Full WOR Pattern

This is the average of the entire WOR cycle:

Sleep portion (~21 µA)

Listening window (~14 mA peak)

Return to sleep

This full‑cycle average is the correct representation of WOR behavior.

 EoRa‑S3‑900TB Wake Behavior (Event‑Driven)

When WOR triggers:

The MCU wakes

Performs housekeeping

Reads SX1262 IRQ flags

Re‑arms WOR

Returns to sleep

Wake current is ~30 µA, but only during the short wake window.

Typical deployments see ~20 wake events per day, so this cost is small and intermittent.

 Example Log Output

Wakeup cause = 0
[COLD BOOT] Initializing SX1262 for RxDutyCycle WOR...

SX1262 reset...
SX1262 MINIMAL configuration complete.
Arming RxDutyCycle...
=== Entering deep sleep ===

Wakeup cause = 3
[WOR WAKE] EXT0 fired! Waiting for packet completion...
SX1262 IRQ = 0x0000

 Project Structure

/src
  main.cpp
  boards.h
  utilities.h
  sx1262_commands.h
/docs
  images/
  timing/

 Contributions Welcome

This project is open‑source and beginner‑friendly.Contributions are welcome in:

documentation

timing diagrams

SX1262 improvements

deep‑sleep optimization

transmitter examples

 Summary

SX1262 is built into the EoRa‑S3‑900TB

SX1262 runs RxDutyCycle autonomously

LoRa’s only job is to wake the MCU

DIO1 → GPIO33 → jumper → GPIO16 → EXT0 wake

Three support files must be in the sketch folder

Sketch folder can be placed anywhere

ESP32 Core 3.3.10 is mandatory

WOR sleep segment averages ~21 µA

Wake events are rare and efficient


---

