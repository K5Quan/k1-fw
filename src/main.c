#include "board.h"
#include "driver/fat.h"
#include "driver/flash_sync.h"
#include "driver/gpio.h"
#include "driver/py25q16.h"
#include "driver/systick.h"
#include "driver/uart.h"
#include "external/printf/printf.h"
#include "system.h"
#include <assert.h>
#include <stdbool.h>

int main(void) {
  SYSTICK_Init();
  BOARD_Init();


  SYS_Main();
}
