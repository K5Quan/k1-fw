#include "board.h"
#include "driver/audio.h"
#include "driver/bk1080.h"
#include "driver/bk4819-regs.h"
#include "driver/bk4829.h"
#include "driver/gpio.h"
#include "driver/keyboard.h"
#include "driver/lfs.h"
#include "driver/st7565.h"
#include "driver/systick.h"
#include "driver/uart.h"
#include "external/printf/printf.h"
#include "helper/measurements.h"
#include "helper/scancommand.h"
#include "helper/storage.h"
#include "inc/band.h"
#include "inc/channel.h"
#include "inc/common.h"
#include "settings.h"
#include "system.h"
#include "ui/graphics.h"
#include "ui/spectrum.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

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

typedef enum {
  NO_RESET,
  VCO,
  VCO_DSP,
  RST_0,
  RST_0x200,

  RST_COUNT,
} ResetType;

ResetType resetType = NO_RESET;

uint32_t stabDelay = 300;
uint32_t scanDelay = 1200;

const char *RTN[] = {
    "NO_RESET",  //
    "VCO",       //
    "VCO_DSP",   //
    "RST_0",     //
    "RST_0x200", //
};

static void onKey(KEY_Code_t key, KEY_State_t state) {
  if (state == KEY_RELEASED) {
    switch (key) {
    case KEY_1:
    case KEY_7:
      scanDelay = AdjustU(scanDelay, 0, 5000, key == KEY_1 ? 100 : -100);
      break;
    case KEY_2:
    case KEY_8:
      stabDelay = AdjustU(stabDelay, 0, 5000, key == KEY_2 ? 100 : -100);
      break;
    case KEY_STAR:
    case KEY_F:
      resetType = AdjustU(resetType, 0, RST_COUNT, key == KEY_STAR ? 1 : -1);
      break;
    }
  }
}

int main(void) {
  SYSTICK_Init();
  BOARD_Init();

  Band b = {
      .start = 433 * MHZ,
      .end = 433 * MHZ + 25 * KHZ * 128,
      .step = STEP_25_0kHz,
  };
  Measurement m = {.f = b.start};
  const uint16_t StepFrequencyTable[15] = {
      2,   5,   50,  100,

      250, 500, 625, 833, 900, 1000, 1250, 2500, 5000, 10000, 50000,
  };
  STORAGE_LOAD("Settings.set", 0, &gSettings);

  keyboard_init(onKey);

  BK4819_Init();
  BK4819_ToggleGpioOut(BK4819_GPIO0_PIN28_RX_ENABLE, true);
  BK4819_RX_TurnOn();
  BK4819_SetModulation(MOD_FM);
  BK4819_SetFilterBandwidth(BK4819_FILTER_BW_12k);
  BK4819_SetAGC(true, 0);
  BK4819_SetAFC(0);

  BK4819_SelectFilter(434 * MHZ);

  GPIO_TurnOnBacklight();

  uint32_t appsKeyboardTimer;

  SP_Init(&b);
  uint16_t reg = BK4819_ReadRegister(BK4819_REG_30);

  for (;;) {
    for (uint8_t i = 0; i < LCD_WIDTH; ++i) {

      if (Now() - appsKeyboardTimer >= 1) {
        keyboard_tick_1ms();
        appsKeyboardTimer = Now();
      }

      BK4819_SetFrequency(m.f);

      switch (resetType) {
      case NO_RESET:
        break;
      case VCO:
        BK4819_WriteRegister(BK4819_REG_30,
                             reg & ~BK4819_REG_30_ENABLE_VCO_CALIB);
        break;
      case VCO_DSP:
        BK4819_WriteRegister(BK4819_REG_30, reg & ~((1 << 15) | (1)));
        break;
      case RST_0:
        BK4819_WriteRegister(BK4819_REG_30, 0);
        break;
      case RST_0x200:
        BK4819_WriteRegister(BK4819_REG_30, 0x200);
        break;
      }

      SYSTICK_DelayUs(stabDelay);
      BK4819_WriteRegister(BK4819_REG_30, reg);

      SYSTICK_DelayUs(scanDelay);
      m.rssi = BK4819_GetRSSI();
      SP_AddPoint(&m);
      m.f += StepFrequencyTable[b.step];
      if (m.f > b.end) {
        m.f = b.start;
      }
    }
    VMinMax mm = SP_GetMinMax();
    UI_ClearScreen();
    SP_Render(&b, mm);
    PrintSmall(0, 16 + 6 * 0, "%u", scanDelay);
    PrintSmall(0, 16 + 6 * 1, "%u", stabDelay);
    PrintSmallEx(LCD_WIDTH - 1, 16 + 6 * 0, POS_R, C_FILL, "%u", mm.vMax);
    PrintSmallEx(LCD_WIDTH - 1, 16 + 6 * 1, POS_R, C_FILL, "%u", mm.vMin);
    PrintSmallEx(LCD_WIDTH - 1, 16 + 6 * 2, POS_R, C_FILL, "%s",
                 RTN[resetType]);
    ST7565_Blit();
  }

  SYS_Main();
}
