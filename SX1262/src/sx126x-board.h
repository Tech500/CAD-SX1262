#ifndef __SX126x_ARCH_H__
#define __SX126x_ARCH_H__

#include <stdint.h>
#include <stdbool.h>

#include "sx126x.h"

#ifdef __cplusplus
extern "C" {
#endif

void SX126xIoInit(void);
void SX126xIoIrqInit(DioIrqHandler dioIrq);
void SX126xIoDeInit(void);

void SX126xReset(void);
void SX126xWaitOnBusy(void);
void SX126xWakeup(void);

void SX126xWriteCommand(
    RadioCommands_t opcode,
    uint8_t *buffer,
    uint16_t size
);

void SX126xReadCommand(
    RadioCommands_t opcode,
    uint8_t *buffer,
    uint16_t size
);

void SX126xWriteRegisters(
    uint16_t address,
    uint8_t *buffer,
    uint16_t size
);

void SX126xWriteRegister(
    uint16_t address,
    uint8_t value
);

uint8_t SX126xReadRegister(
    uint16_t address
);

void SX126xSetRfTxPower(int8_t power);

uint8_t SX126xGetPaSelect(uint32_t channel);

void SX126xAntSwOn(void);
void SX126xAntSwOff(void);

bool SX126xCheckRfFrequency(uint32_t frequency);

extern SX126x_t SX126x;

#ifdef __cplusplus
}
#endif

#endif