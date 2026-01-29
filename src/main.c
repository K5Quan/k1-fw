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
#include "helper/fsk2.h"
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
#include <string.h>

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

int main(void) {
  SYSTICK_Init();
  BOARD_Init();

  BK4819_Init();
  BK4819_ToggleGpioOut(BK4819_GPIO0_PIN28_RX_ENABLE, true);
  BK4819_RX_TurnOn();

  BK4819_SetModulation(MOD_FM);
  BK4819_SetAGC(true, 0);
  BK4819_SetAFC(0);
  BK4819_SetFilterBandwidth(BK4819_FILTER_BW_12k);

  BK4819_TuneTo(434 * MHZ, true);
  AUDIO_AudioPathOn();
  // BK4819_WriteRegister(0x43, 0x3028);
  for (;;) {
    RF_EnterFsk();
    BK4819_ToggleGpioOut(BK4819_RED, true);
    BK4819_SetupPowerAmplifier(128, 434 * MHZ);
    RF_FskTransmit();
    BK4819_TurnsOffTones_TurnsOnRX();
    BK4819_ToggleGpioOut(BK4819_RED, false);
    RF_ExitFsk();
    SYSTICK_DelayMs(2000);
  }

  GPIO_TurnOnBacklight();
  RF_EnterFsk();
  for (;;) {
    if (RF_FskReceive()) {
      printf("%+10u: ", Now());
      for (uint8_t i = 0; i < 64; ++i) {
        printf("%02x%02x", FSK_RXDATA[i] >> 8, FSK_RXDATA[i] & 0xFF);
      }
      printf("\n");
      UI_ClearStatus();
      UI_ClearScreen();
      PrintMedium(0, 8, "%+10u", Now());
      for (uint8_t y = 0; y < 64; ++y) {
        PrintSmall(y % 6 * 20, 8 + (y / 6 + 1) * 6, "%02x%02x",
                   FSK_RXDATA[y] >> 8, FSK_RXDATA[y] & 0xFF);
      }
      ST7565_Blit();
    }
    __WFI();
  }
  RF_ExitFsk();

  SYS_Main();
}
