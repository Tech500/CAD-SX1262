#include <Arduino.h>
#include <SPI.h>

#include "board.h"
#include "sx126x.h"
#include "sx126x-board.h"

// ============================================================
// ESP32-S3 SPI
// ============================================================

static SPIClass *sxSPI = &SPI;

// ============================================================
// DIO1 callback
// ============================================================

static DioIrqHandler *dio1Handler = nullptr;

// ============================================================
// ESP32 adapter
// ============================================================

void DelayMs(uint32_t ms)
{
    delay(ms);
}

void BoardDisableIrq(void)
{
    noInterrupts();
}

void BoardEnableIrq(void)
{
    interrupts();
}

// ============================================================
// SPI transaction helper
// ============================================================

static inline void spiBeginTransaction()
{
    sxSPI->beginTransaction(
        SPISettings(
            8000000,
            MSBFIRST,
            SPI_MODE0
        )
    );
}

static inline void spiEndTransaction()
{
    sxSPI->endTransaction();
}

// ============================================================
// DIO1 ISR
// ============================================================

static void IRAM_ATTR sx126xDio1ISR()
{
    if (dio1Handler != nullptr)
    {
        dio1Handler();
    }
}

// ============================================================
// I/O initialization
// ============================================================

void SX126xIoInit(void)
{
    pinMode(RADIO_NSS, OUTPUT);
    digitalWrite(RADIO_NSS, HIGH);

    pinMode(RADIO_BUSY, INPUT);
    pinMode(RADIO_DIO_1, INPUT);
    pinMode(RADIO_RESET, OUTPUT);

    digitalWrite(RADIO_RESET, HIGH);

    sxSPI->begin(
        RADIO_SCLK,
        RADIO_MISO,
        RADIO_MOSI,
        RADIO_NSS
    );

    delay(10);
}

// ============================================================
// DIO1 interrupt
// ============================================================

void SX126xIoIrqInit(DioIrqHandler dioIrq)
{
    dio1Handler = dioIrq;

    pinMode(RADIO_DIO_1, INPUT);

    attachInterrupt(
        digitalPinToInterrupt(RADIO_DIO_1),
        sx126xDio1ISR,
        RISING
    );
}

// ============================================================
// De-init
// ============================================================

void SX126xIoDeInit(void)
{
    detachInterrupt(
        digitalPinToInterrupt(RADIO_DIO_1)
    );

    sxSPI->end();

    pinMode(RADIO_NSS, INPUT);
    pinMode(RADIO_BUSY, INPUT);
    pinMode(RADIO_DIO_1, INPUT);
    pinMode(RADIO_RESET, INPUT);
}

// ============================================================
// SX1262 hardware reset
// ============================================================

void SX126xReset(void)
{
    digitalWrite(RADIO_RESET, HIGH);
    delay(10);

    digitalWrite(RADIO_RESET, LOW);
    delay(20);

    digitalWrite(RADIO_RESET, HIGH);
    delay(10);
}

// ============================================================
// Wait for BUSY to go LOW
// ============================================================

void SX126xWaitOnBusy(void)
{
    uint32_t start = millis();

    while (digitalRead(RADIO_BUSY))
    {
        if ((millis() - start) > 1000)
        {
            Serial.println(
                "[SX1262] ERROR: BUSY timeout!"
            );

            Serial.printf(
                "[SX1262] BUSY pin=%d\n",
                digitalRead(RADIO_BUSY)
            );

            return;
        }

        delayMicroseconds(10);
    }
}

// ============================================================
// Wake SX1262
// ============================================================

void SX126xWakeup(void)
{
    BoardDisableIrq();

    digitalWrite(RADIO_NSS, LOW);

    spiBeginTransaction();

    sxSPI->transfer(RADIO_GET_STATUS);
    sxSPI->transfer(0x00);

    spiEndTransaction();

    digitalWrite(RADIO_NSS, HIGH);

    SX126xWaitOnBusy();

    BoardEnableIrq();
}

// ============================================================
// Write command
// ============================================================

void SX126xWriteCommand(
    RadioCommands_t command,
    uint8_t *buffer,
    uint16_t size
)
{
    SX126xCheckDeviceReady();

    digitalWrite(RADIO_NSS, LOW);

    spiBeginTransaction();

    sxSPI->transfer((uint8_t)command);

    for (uint16_t i = 0; i < size; i++)
    {
        sxSPI->transfer(buffer[i]);
    }

    spiEndTransaction();

    digitalWrite(RADIO_NSS, HIGH);

    if (command != RADIO_SET_SLEEP)
    {
        SX126xWaitOnBusy();
    }
}

// ============================================================
// Read command
// ============================================================

void SX126xReadCommand(
    RadioCommands_t command,
    uint8_t *buffer,
    uint16_t size
)
{
    SX126xCheckDeviceReady();

    digitalWrite(RADIO_NSS, LOW);

    spiBeginTransaction();

    sxSPI->transfer((uint8_t)command);
    sxSPI->transfer(0x00);

    for (uint16_t i = 0; i < size; i++)
    {
        buffer[i] = sxSPI->transfer(0x00);
    }

    spiEndTransaction();

    digitalWrite(RADIO_NSS, HIGH);

    SX126xWaitOnBusy();
}

// ============================================================
// Register write
// ============================================================

void SX126xWriteRegisters(
    uint16_t address,
    uint8_t *buffer,
    uint16_t size
)
{
    SX126xCheckDeviceReady();

    digitalWrite(RADIO_NSS, LOW);

    spiBeginTransaction();

    sxSPI->transfer(RADIO_WRITE_REGISTER);
    sxSPI->transfer((address >> 8) & 0xFF);
    sxSPI->transfer(address & 0xFF);

    for (uint16_t i = 0; i < size; i++)
    {
        sxSPI->transfer(buffer[i]);
    }

    spiEndTransaction();

    digitalWrite(RADIO_NSS, HIGH);

    SX126xWaitOnBusy();
}

// ============================================================
// Single register write
// ============================================================

void SX126xWriteRegister(
    uint16_t address,
    uint8_t value
)
{
    SX126xWriteRegisters(
        address,
        &value,
        1
    );
}

// ============================================================
// Register read
// ============================================================

uint8_t SX126xReadRegister(
    uint16_t address
)
{
    uint8_t value;

    SX126xCheckDeviceReady();

    digitalWrite(RADIO_NSS, LOW);

    spiBeginTransaction();

    sxSPI->transfer(RADIO_READ_REGISTER);
    sxSPI->transfer((address >> 8) & 0xFF);
    sxSPI->transfer(address & 0xFF);

    sxSPI->transfer(0x00);

    value = sxSPI->transfer(0x00);

    spiEndTransaction();

    digitalWrite(RADIO_NSS, HIGH);

    SX126xWaitOnBusy();

    return value;
}

// ============================================================
// SX1262 PA selection
// ============================================================

uint8_t SX126xGetPaSelect(uint32_t channel)
{
    return SX1262;
}

// ============================================================
// RF switch
//
// EoRa-S3 uses DIO2 as the RF switch control.
// The Semtech driver enables this in SX126xInit().
// ============================================================

void SX126xAntSwOn(void)
{
    // DIO2 RF switch is controlled internally by SX1262.
}

void SX126xAntSwOff(void)
{
    // DIO2 RF switch is controlled internally by SX1262.
}

// ============================================================
// TX power
// ============================================================

void SX126xSetRfTxPower(int8_t power)
{
    SX126xSetTxParams(
        power,
        RADIO_RAMP_40_US
    );
}

// ============================================================
// Frequency validation
// ============================================================

bool SX126xCheckRfFrequency(uint32_t frequency)
{
    return true;
}

