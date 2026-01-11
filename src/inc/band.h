#ifndef BAND_H
#define BAND_H

#include "common.h"

typedef struct {
  uint16_t scanlists;
  char name[10];
  uint32_t start : 27;
  int32_t ppm : 5;
  uint32_t end : 27;
  OffsetDirection offsetDir : 2;
  bool allowTx : 1;
  uint8_t reserved2 : 2;
  Step step : 4;
  uint8_t modulation : 4;
  uint8_t bw : 4;
  Radio radio : 2;
  TXOutputPower power : 2;
  uint8_t scrambler : 4;
  Squelch squelch;
  uint8_t bank;
  PowerCalibration powCalib;
  uint32_t lastUsedFreq : 27;
  uint8_t gainIndex : 5;
} __attribute__((packed)) Band;

#endif // !BAND_H
