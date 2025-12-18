#include "spectrum.h"
#include "driver/st7565.h"
#include "external/printf/printf.h"
#include "helper/measurements.h"
#include "ui/graphics.h"
#include <string.h>

#define SPECTRUM_HEIGHT 48
#define SPECTRUM_TOP 8

static uint16_t trace[LCD_WIDTH];
static Band band;
static uint16_t mi;
static uint16_t ma;

static uint8_t markers[4];

uint8_t SP_F2X(uint32_t f) {
  if (f <= band.start) {
    return 0;
  }
  if (f >= band.end) {
    return LCD_WIDTH - 1;
  }

  uint32_t delta = f - band.start;
  uint32_t span = band.end - band.start;
  uint32_t step = span / (LCD_WIDTH - 1);

  if (step == 0) {
    return 0;
  }

  return delta / step;
}

void SP_Init(Band b) {
  band = b;
  mi = 512;
  ma = 0;
  for (uint8_t x = 0; x < LCD_WIDTH; ++x) {
    trace[x] = 0;
  }
}

void SP_AddPoint(Measurement *msm) {
  if (msm->f < band.start || msm->f > band.end) {
    return;
  }

  uint8_t x = SP_F2X(msm->f);

  uint16_t v = msm->rssi;
  if (v > trace[x]) {
    trace[x] = v;
    if (v > ma) {
      ma = v;
    }
    if (v < mi) {
      mi = v;
    }
  }
}

void SP_Draw() {
  uint8_t bottom = SPECTRUM_TOP + SPECTRUM_HEIGHT;

  for (uint8_t x = 0; x < LCD_WIDTH; ++x) {
    uint16_t v = trace[x];
  }

  for (uint8_t x = 1; x < LCD_WIDTH - 1; ++x) {
    if (trace[x] < 10 && trace[x - 1] > 10 && trace[x + 1] > 10) {
      trace[x] = (trace[x - 1] + trace[x + 1]) / 2;
    }
  }

  for (uint8_t x = 0; x < LCD_WIDTH; ++x) {
    uint8_t h = ConvertDomain(trace[x], mi - 2, ma - mi < 40 ? mi + 40 : ma, 0,
                              SPECTRUM_HEIGHT);
    DrawVLine(x, bottom - h, h, C_FILL);
  }
}
