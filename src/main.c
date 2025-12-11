#include "board.h"
#include "driver/backlight.h"
#include "driver/bk4829.h"
#include "driver/crc.h"
#include "driver/st7565.h"
#include "driver/systick.h"
#include "driver/uart.h"
#include "external/printf/printf.h"
#include "ui/graphics.h"
#include <stdbool.h>

static uint32_t fInitial = 17230000;

int main(void) {
  SYSTICK_Init();
  BOARD_Init();
  UART_Init();

  BK4819_Init();
  BK4819_RX_TurnOn();
  BK4819_TuneTo(fInitial, true);
  BK4819_SelectFilter(fInitial);
  BK4819_SetFilterBandwidth(BK4819_FILTER_BW_12k);
  BK4819_SetModulation(MOD_FM);
  // BK4819_SetAGC(true, 0);

  BACKLIGHT_TurnOn();
  UI_ClearScreen();
  PrintMedium(0, 32, "Hello world!");
  ST7565_BlitFullScreen();

  for (;;) {
    printf("RSSI=%u\n", BK4819_GetRSSI());
    BOARD_FlashlightToggle();
    SYSTICK_DelayMs(1000);
  }
}
