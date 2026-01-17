#include "board.h"
#include "driver/audio.h"
#include "driver/bk1080.h"
#include "driver/bk4829.h"
#include "driver/lfs.h"
#include "driver/systick.h"
#include "driver/uart.h"
#include "external/printf/printf.h"
#include "helper/measurements.h"
#include "helper/scancommand.h"
#include "helper/storage.h"
#include "inc/channel.h"
#include "system.h"
#include <assert.h>
#include <stdbool.h>

// Самый простой обработчик HardFault
static void HardFault_Handler(void) {
  uint32_t stacked_pc, stacked_lr, stacked_sp;

  // Получаем значения из стека
  __asm volatile("mrs %0, msp \n" // MSP -> stacked_sp
                 : "=r"(stacked_sp));

  // PC и LR сохранены на стеке
  stacked_pc = ((uint32_t *)stacked_sp)[6];
  stacked_lr = ((uint32_t *)stacked_sp)[5];

  // Просто выводим информацию
  LogC(LOG_C_BRIGHT_RED, "!!! HARD FAULT !!!");
  LogC(LOG_C_RED, "PC: 0x%08X", stacked_pc);
  LogC(LOG_C_RED, "LR: 0x%08X", stacked_lr);
  LogC(LOG_C_RED, "SP: 0x%08X", stacked_sp);

  // Проверка на переполнение стека
  extern uint32_t _estack;
  uint32_t stack_end = (uint32_t)&_estack;

  if (stacked_sp > stack_end) {
    LogC(LOG_C_BRIGHT_RED, "STACK OVERFLOW DETECTED!");
    LogC(LOG_C_RED, "SP (0x%08X) > StackEnd (0x%08X)", stacked_sp, stack_end);
  }

  // Останавливаем систему
  while (1) {
    // Мигаем или делаем что-то для индикации
  }
}

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
    if (!lfs_file_exists(slName)) {
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

void BK1080_DumpRegisters(void) {
  for (int i = 0; i <= 0x25; i++) {
    uint16_t val = BK1080_ReadRegister(i);
    Log("Reg[0x%02X] = 0x%04X", i, val);
    SYSTICK_DelayMs(10);
  }
}

int main(void) {
  SYSTICK_Init();
  BOARD_Init();

  SYS_Main();
}
