#ifndef __BOARD_H__
#define __BOARD_H__

#include <Arduino.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================
// EoRa-S3-900TB / ESP32-S3 pin assignment
// ============================================================

#define RADIO_SCLK      5
#define RADIO_MISO      3
#define RADIO_MOSI      6
#define RADIO_NSS       7
#define RADIO_DIO_1     16
#define RADIO_BUSY      34
#define RADIO_RESET     8

// ============================================================
// Minimal Semtech compatibility types
// ============================================================

typedef struct
{
    int pin;
} Gpio_t;

typedef struct
{
    int mosi;
    int miso;
    int sclk;
    int nss;
} Spi_t;

// ============================================================
// Semtech compatibility functions
// ============================================================

void DelayMs(uint32_t ms);

void BoardDisableIrq(void);
void BoardEnableIrq(void);

#ifdef __cplusplus
}
#endif

#endif