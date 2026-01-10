#include "board.h"
#include "driver/bk4829.h"
#include "driver/fat.h"
#include "driver/systick.h"
#include "driver/uart.h"
#include "external/printf/printf.h"
#include "helper/ch.h"
#include "helper/measurements.h"
#include "helper/storage.h"
#include "system.h"
#include <assert.h>
#include <stdbool.h>

CH testChannels[] = {
    {.name = "CH1 SL1", .scanlists = 1 << 0},
    {.name = "CH2 SL2", .scanlists = 1 << 1},
    {.name = "CH3 SL1", .scanlists = 1 << 0},
    {.name = "CH4 SL2", .scanlists = 1 << 1},
    {.name = "CH5 SL1", .scanlists = 1 << 0},
    {.name = "CH6 SL0", .scanlists = 0},
    {.name = "CH7 SL1", .scanlists = 1 << 0},
    {.name = "CH8 SL1", .scanlists = 1 << 0},
    {.name = "CH9 SL2", .scanlists = 1 << 1},
    {.name = "CH10 SL1", .scanlists = 1 << 0},
};

void ch_init() {
  Log("Init channels");
  STORAGE_INIT("CHANNELS.CH", CH, 1024);

  for (uint16_t i = 0; i < ARRAY_SIZE(testChannels); ++i) {
    STORAGE_SAVE("CHANNELS.CH", i, &testChannels[i]);
  }
}

void ch_prepare_sl() {
  Log("Read channels");
  char slName[10];
  uint16_t scanlistNumeration[16] = {0};
  for (uint16_t i = 0; i < 1024; ++i) {
    CH ch;
    STORAGE_LOAD("CHANNELS.CH", i, &ch);

    if (IsReadable(ch.name)) {
      for (uint8_t sl = 0; sl < 16; ++sl) {
        if ((ch.scanlists >> sl) & 1) {
          snprintf(slName, 10, "SL%u.SL", sl);

          Log("ADD %s TO %s AT %u", ch.name, slName, scanlistNumeration[sl]);

          STORAGE_INIT(slName, CH, 1024);
          STORAGE_SAVE(slName, scanlistNumeration[sl], &ch);
          scanlistNumeration[sl]++;
        }
      }
    }
  }
}

void ch_show_scanlists() {
  for (uint8_t sl = 0; sl < 16; ++sl) {
    char slName[10];
    snprintf(slName, 10, "SL%u.SL", sl);
    if (!usb_fs_file_exists(slName)) {
      continue;
    }
    for (uint16_t i = 0; i < 1024; ++i) {
      CH ch;
      STORAGE_LOAD(slName, i, &ch);

      if (!IsReadable(ch.name)) {
        break;
      }

      Log("%s: CH %s", slName, ch.name);
    }
  }
}

void ch_measure_scan() {
  uint32_t start = Now();

  uint16_t i;

  for (i = 0; i < 1024; ++i) {
    CH ch;
    STORAGE_LOAD("SL0.SL", i, &ch);

    if (!IsReadable(ch.name)) {
      break;
    }
  }
  Log("SCAN %u channels, t=%ums", i, Now() - start);
}

int main(void) {
  SYSTICK_Init();
  BOARD_Init();

  // usb_fs_format();

  ch_init();
  ch_prepare_sl();
  ch_show_scanlists();

  ch_measure_scan();

  BOARD_USBInit();

  for (;;) {
  }

  /* BOARD_USBInit();
  for (;;) {
    printf("%lu\n", Now());
    SYSTICK_DelayMs(1000);
  } */

  // check_fat_consistency();

  SYS_Main();
}
