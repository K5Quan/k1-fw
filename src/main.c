#include "board.h"
#include "driver/audio.h"
#include "driver/backlight.h"
#include "driver/bk4829.h"
#include "driver/crc.h"
#include "driver/keyboard.h"
#include "driver/st7565.h"
#include "driver/systick.h"
#include "driver/uart.h"
#include "external/printf/printf.h"
#include "helper/measurements.h"
#include "ui/graphics.h"
#include <stdbool.h>
#include <stdint.h>

static const uint32_t fStart = 43300000;
static const uint32_t fEnd = fStart + 2500 * LCD_WIDTH;
static uint32_t f;

uint16_t rssiHistory[LCD_WIDTH];
bool redraw = true;
uint32_t lastRedrawtime;
bool open = false;
bool listen = false;

uint16_t rssiMin = 60;

uint16_t measure(uint32_t f) {
  BK4819_SelectFilter(f);
  BK4819_TuneTo(f, true);
  SYSTICK_DelayUs(1200);
  return BK4819_GetRSSI();
}

void tick() {
  uint16_t rssi = measure(f);
  // listen = rssi > 75;

  uint8_t x = ConvertDomain(f, fStart, fEnd, 0, LCD_WIDTH);
  if (rssi > rssiHistory[x]) {
    rssiHistory[x] = rssi;
  }

  if (listen != open) {
    listen = open;
    redraw = true;
    if (listen) {
      AUDIO_AudioPathOn();
    } else {
      AUDIO_AudioPathOff();
    }
  }

  if (f >= fEnd) {
    f = fStart;

    UI_ClearScreen();
    uint16_t mi = 512;
    uint16_t ma = 0;
    for (uint8_t i = 0; i < LCD_WIDTH; ++i) {
      uint16_t r = rssiHistory[i];
      if (r > ma) {
        ma = r;
      }
      if (r < mi) {
        mi = r;
      }
      uint8_t v = ConvertDomain(r, rssiMin - 2, 180, 0, LCD_HEIGHT - 16);
      DrawVLine(i, LCD_HEIGHT - v, v, C_FILL);
      rssiHistory[i] = 0;
    }
    rssiMin = mi;
    PrintSmall(0, 8 + 6, "Max: %u", ma);
    PrintSmall(0, 8 + 12, "Min: %u", mi);
    redraw = true;
  } else {
    f += 2500;
  }

  if (redraw && Now() - lastRedrawtime >= 40) {
    redraw = false;
    lastRedrawtime = Now();
    ST7565_BlitFullScreen();
  }
}

int main(void) {
  SYSTICK_Init();
  BOARD_Init();
  UART_Init();

  BK4819_Init();
  BK4819_ToggleGpioOut(BK4819_GPIO0_PIN28_RX_ENABLE, true);
  BK4819_RX_TurnOn();

  f = fStart;

  BK4819_TuneTo(f, true);
  BK4819_SelectFilter(f);
  BK4819_SetFilterBandwidth(BK4819_FILTER_BW_12k);
  BK4819_SetModulation(MOD_FM);
  BK4819_SetAGC(true, 0);

  BACKLIGHT_TurnOn();

  for (;;) {
    tick();
  }
}
