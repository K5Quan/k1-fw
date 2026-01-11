#ifndef BANDS_H
#define BANDS_H

#include "../inc/band.h"
#include <stdint.h>

#define MAX_BANDS 128
#define RANGES_STACK_SIZE 5

Band BANDS_ByFrequency(uint32_t f);
bool BANDS_InRange(uint32_t f, Band *b);
uint8_t BANDS_CalculateOutputPower(TXOutputPower power, uint32_t f);

void BANDS_RangeClear();
int8_t BANDS_RangeIndex();
bool BANDS_RangePush(Band r);
Band BANDS_RangePop(void);
Band *BANDS_RangePeek(void);

extern const Band DEFAULT_BAND;

#endif // !BANDS_H
