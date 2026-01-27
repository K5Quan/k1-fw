#ifndef FSK2_H
#define FSK2_H

#include <stdbool.h>
#include <stdint.h>

// Публичные буферы для FSK данных
extern uint16_t FSK_TXDATA[64];
extern uint16_t FSK_RXDATA[64];

// Функции управления RF режимом
void RF_Txon(void);
void RF_Rxon(void);
void RF_EnterFsk(void);
void RF_ExitFsk(void);
void RF_FskIdle(void);

// Основные функции FSK
bool RF_FskTransmit(void);
bool RF_FskReceive(void);

#endif // FSK2_H
