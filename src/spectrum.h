#ifndef SPECTRUM_H
#define SPECTRUM_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
  uint32_t f;
  uint16_t rssi;
} Measurement;

typedef struct {
  uint32_t start;
  uint32_t end;
  uint32_t step;
} Band;

void SP_Init(Band b);
void SP_AddPoint(Measurement *msm);
void SP_Draw();

#endif // SPECTRUM_H
