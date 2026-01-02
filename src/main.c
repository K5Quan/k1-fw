#include "board.h"
#include "driver/fat.h"
#include "driver/gpio.h"
#include "driver/systick.h"
#include "driver/uart.h"
#include "external/printf/printf.h"
#include "system.h"
#include <stdbool.h>

int main(void) {
  SYSTICK_Init();
  BOARD_Init();
  UART_Init();

  usb_fs_init();

  file_info_t list[20];
  int count = usb_fs_list_files(list, 20);
  printf("Files count: %d\n", count);
  for (int i = 0; i < count; i++) {
    printf("File: '%s' size=%u\n", list[i].name, list[i].size);
  }

  printf("Hawk\n");

  GPIO_EnableAudioPath();

  SYS_Main();
}
