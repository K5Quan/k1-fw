// ============================================================================
// FSK Packet Handler - Упрощённая версия
// ============================================================================

#ifndef FSK_H
#define FSK_H

#include "measurements.h"
#include <stdbool.h>
#include <stdint.h>

// Сброс буферов и состояния
void FSK_Reset(void);

// Передача
bool FSK_PrepareData(const char *data, size_t len);
bool FSK_Transmit(void);
bool FSK_IsTxReady(void);

// Прием
bool FSK_ReadFifo(uint16_t irq);
bool FSK_ProcessPacket(void);
bool FSK_IsRxFull(void);

// Получение данных
const char *FSK_GetMessage(void);
uint16_t FSK_GetMessageLen(void);
const uint16_t *FSK_GetRawData(void);

#endif // FSK_H
