#include "board.h"
#include "driver/crc.h"
#include "driver/systick.h"
#include "driver/uart.h"
#include "external/printf/printf.h"

int main(void) {
  SYSTICK_Init();
  BOARD_Init();
  UART_Init();
  printf("1111\n");

  uint16_t *v, *c;

  for (;;) {
    BOARD_ADC_GetBatteryInfo(v, c);
    printf("V=%u\n", *v);
    BOARD_FlashlightToggle();
    SYSTICK_DelayUs(1000000);
  }
}
