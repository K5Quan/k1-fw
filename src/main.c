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
#include "misc.h"
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

void pack_string(const char *str, uint16_t *output, size_t *output_len) {
  size_t len = strlen(str);
  size_t words = (len + 1) / 2; // +1 для завершающего '\0'

  for (size_t i = 0; i < words; i++) {
    uint16_t word = 0;

    // Младший байт (первый символ)
    if (i * 2 < len) {
      word = (uint8_t)str[i * 2];
    }

    // Старший байт (второй символ)
    if (i * 2 + 1 < len) {
      word |= ((uint16_t)(uint8_t)str[i * 2 + 1]) << 8;
    }

    output[i] = word;
  }

  *output_len = words;
}

void unpack_string(const uint16_t *input, size_t input_len, char *output,
                   size_t max_output_len) {
  size_t pos = 0;

  for (size_t i = 0; i < input_len && pos < max_output_len - 1; i++) {
    // Младший байт (первый символ)
    char c1 = (char)(input[i] & 0xFF);
    if (c1 == '\0')
      break;
    output[pos++] = c1;

    if (pos >= max_output_len - 1)
      break;

    // Старший байт (второй символ)
    char c2 = (char)((input[i] >> 8) & 0xFF);
    if (c2 == '\0')
      break;
    output[pos++] = c2;
  }

  output[pos] = '\0';
}

int main(void) {
  SYSTICK_Init();
  BOARD_Init();

  UI_ClearStatus();
  UI_ClearScreen();
  PrintMedium(0, 8, "Initialized");
  ST7565_Blit();

  BK4819_Init();

  BK4819_SetModulation(MOD_FM);
  BK4819_SetAGC(true, 1);
  BK4819_SetAFC(0);
  // BK4819_SetFilterBandwidth(BK4819_FILTER_BW_12k);

  BK4819_TuneTo(434 * MHZ, true);

  /* static const char *str =
      "To get started with investigating a wireless signal we need to capture "
      "it with a Software Defined Radio";
  size_t len = strlen(str) + 1;
  pack_string(str, FSK_TXDATA, &len); */

  BK4819_WriteRegister(0x43, 0x3028);

  for (uint8_t i = 0; i < ARRAY_SIZE(FSK_TXDATA); ++i) {
    FSK_TXDATA[i] = 0xC0FE;
  }

  FSK_TXDATA[0] = 0xDEAD;
  FSK_TXDATA[1] = 0xBEEF;

  /* for (;;) {
    BK4819_ToggleGpioOut(BK4819_RED, true);

    BK4819_TxOn_Beep();
    SYSTICK_DelayMs(10);
    BK4819_ToggleGpioOut(BK4819_GPIO1_PIN29_PA_ENABLE, true);
    SYSTICK_DelayMs(5);
    BK4819_SetupPowerAmplifier(10, 434 * MHZ);
    SYSTICK_DelayMs(10);

    RF_EnterFsk(); // without this deviation is small at 2+ tx
    RF_FskTransmit();
    RF_ExitFsk();

    BK4819_SetupPowerAmplifier(0, 0);
    BK4819_ToggleGpioOut(BK4819_GPIO1_PIN29_PA_ENABLE, false);
    BK4819_ToggleGpioOut(BK4819_RED, false);
    BK4819_TurnsOffTones_TurnsOnRX();

    SYSTICK_DelayMs(2000);
  } */

  BK4819_ToggleGpioOut(BK4819_GPIO0_PIN28_RX_ENABLE, true);
  AUDIO_AudioPathOn();
  GPIO_TurnOnBacklight();

  BK4819_RX_TurnOn();

  BK4819_WriteRegister(BK4819_REG_3F, BK4819_REG_3F_FSK_RX_SYNC |
                                          BK4819_REG_3F_FSK_FIFO_ALMOST_FULL |
                                          BK4819_REG_3F_FSK_RX_FINISHED);

  RF_EnterFsk();

  const uint16_t REG_59 = (1 << 3)         // fsk sync length = 4B
                          | ((8 - 1) << 4) // preamble len = (v + 1)B 0..15
      ;
  BK4819_WriteRegister(0x59, REG_59 | 0x4000);    //[14]fifo clear
  BK4819_WriteRegister(0x59, REG_59 | (1 << 12)); //[12]fsk_rx_en
  for (;;) {
    while (BK4819_ReadRegister(0x0C) & 1) {
      BK4819_WriteRegister(0x02, 0); // clear int
      uint16_t int_bits = BK4819_ReadRegister(0x02);
      if (RF_FskReceive(int_bits)) {
        UI_ClearStatus();
        UI_ClearScreen();
        PrintMedium(0, 8, "%+10u", Now());
        PrintSmall(0, 22, "%04X %04X %04X", FSK_RXDATA[0], FSK_RXDATA[1],
                   FSK_RXDATA[2]);
        ST7565_Blit();
      }
    }
    // __WFI();
  }
  RF_ExitFsk();

  SYS_Main();
}
