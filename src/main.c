#include "board.h"
#include "dcs.h"
#include "driver/audio.h"
#include "driver/bk4819-regs.h"
#include "driver/bk4829.h"
#include "driver/gpio.h"
#include "driver/st7565.h"
#include "driver/systick.h"
#include "driver/uart.h"
#include "helper/fsk2.h"
#include "misc.h"
#include "system.h"
#include "ui/graphics.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

int main(void) {
  SYSTICK_Init();
  BOARD_Init();
  GPIO_TurnOnBacklight();

  SYS_Main();

  UI_ClearStatus();
  UI_ClearScreen();
  PrintMedium(0, 8, "Initialized");
  ST7565_Blit();

  BK4819_Init();

  BK4819_TuneTo(434 * MHZ, true);
  BK4819_SetAFC(1); // small AFC

  // disable the 300Hz HPF and FM pre-emphasis filter
  const uint16_t filt_val = BK4819_ReadRegister(BK4819_REG_2B);
  BK4819_WriteRegister(BK4819_REG_2B, (1u << 2) | (1u << 0));

  // INT test
  BK4819_ToggleGpioOut(BK4819_GPIO0_PIN28_RX_ENABLE, true);
  AUDIO_AudioPathOn();
  uint16_t InterruptMask = BK4819_REG_3F_CxCSS_TAIL;

  InterruptMask |= BK4819_REG_3F_FSK_RX_SYNC |
                   BK4819_REG_3F_FSK_FIFO_ALMOST_FULL |
                   BK4819_REG_3F_FSK_RX_FINISHED;

  InterruptMask |= BK4819_REG_3F_SQUELCH_LOST | BK4819_REG_3F_SQUELCH_FOUND;
  InterruptMask |= BK4819_REG_3F_DTMF_5TONE_FOUND;
  InterruptMask |= BK4819_REG_3F_CDCSS_FOUND | BK4819_REG_3F_CDCSS_LOST;
  InterruptMask |= BK4819_REG_3F_CTCSS_FOUND | BK4819_REG_3F_CTCSS_LOST;
  BK4819_WriteRegister(BK4819_REG_3F, InterruptMask);

  BK4819_EnableDTMF();
  BK4819_RX_TurnOn();
  RF_EnterFsk();

  for (;;) {
    if (BK4819_ReadRegister(0x0C) & 1) {
      BK4819_WriteRegister(0x02, 0x0000);
      uint16_t int_bits = BK4819_ReadRegister(0x02);

      if (int_bits & BK4819_REG_02_MASK_SQUELCH_LOST) {
        LogC(LOG_C_GREEN, "SQ -");
      }
      if (int_bits & BK4819_REG_02_MASK_SQUELCH_FOUND) {
        LogC(LOG_C_GREEN, "SQ +");
      }
      if (int_bits & BK4819_REG_02_MASK_FSK_RX_SYNC) {
        LogC(LOG_C_GREEN, "FSK RX Sync");
      }
      if (int_bits & BK4819_REG_02_MASK_FSK_FIFO_ALMOST_FULL) {
        LogC(LOG_C_GREEN, "FSK FIFO alm full");
      }
      if (int_bits & BK4819_REG_02_MASK_FSK_FIFO_ALMOST_EMPTY) {
        LogC(LOG_C_GREEN, "FSK FIFO alm empt");
      }
      if (int_bits & BK4819_REG_02_MASK_FSK_RX_FINISHED) {
        LogC(LOG_C_GREEN, "FSK RX finish");
      }
      if (int_bits & BK4819_REG_02_MASK_CxCSS_TAIL) {
        LogC(LOG_C_GREEN, "TAIL tone");
      }
      if (int_bits & BK4819_REG_02_MASK_CTCSS_FOUND) {
        LogC(LOG_C_GREEN, "CT +");
      }
      if (int_bits & BK4819_REG_02_MASK_CTCSS_LOST) {
        LogC(LOG_C_GREEN, "CT -");
      }
      if (int_bits & BK4819_REG_02_MASK_DTMF_5TONE_FOUND) {
        const char c = DTMF_GetCharacter(BK4819_GetDTMF_5TONE_Code());
        LogC(LOG_C_GREEN, "DTMF %c", c);
      }
      if (RF_FskReceive(int_bits)) {
      }
    }
  }

  // #define MODE_TX

#ifdef MODE_TX
  for (uint16_t i = 0; i < ARRAY_SIZE(FSK_TXDATA); ++i) {
    FSK_TXDATA[i] = 0xC0FE;
  }

  FSK_TXDATA[0] = 0xDEAD;
  FSK_TXDATA[1] = 0xBEEF;

  for (;;) {
    BK4819_ToggleGpioOut(BK4819_RED, true);

    BK4819_TxOn_Beep();
    SYSTICK_DelayMs(10);
    BK4819_ToggleGpioOut(BK4819_GPIO1_PIN29_PA_ENABLE, true);
    SYSTICK_DelayMs(5);
    BK4819_SetupPowerAmplifier(90, 434 * MHZ);
    SYSTICK_DelayMs(10);

    RF_EnterFsk(); // without this deviation is small at 2+ tx
    RF_FskTransmit();
    RF_ExitFsk();

    BK4819_SetupPowerAmplifier(0, 0);
    BK4819_ToggleGpioOut(BK4819_GPIO1_PIN29_PA_ENABLE, false);
    BK4819_ToggleGpioOut(BK4819_RED, false);
    BK4819_TurnsOffTones_TurnsOnRX();

    SYSTICK_DelayMs(2000);
  }
#else

  BK4819_ToggleGpioOut(BK4819_GPIO0_PIN28_RX_ENABLE, true);
  AUDIO_AudioPathOn();
  GPIO_TurnOnBacklight();

  // 1. Инициализация BK4829 с софт-ресетом
  BK4819_Init();

  // 2. Настройка частоты
  BK4819_TuneTo(434 * MHZ, true);
  BK4819_SetAFC(1); // small AFC

  // 3. Включаем RX
  BK4819_RX_TurnOn();

  // 4. Настройка FSK
  RF_EnterFsk();

  // 5. Включаем нужные прерывания FSK
  BK4819_WriteRegister(BK4819_REG_3F, BK4819_REG_3F_FSK_RX_SYNC |
                                          BK4819_REG_3F_FSK_FIFO_ALMOST_FULL |
                                          BK4819_REG_3F_FSK_RX_FINISHED);

  printf("FSK Receiver started on 434MHz\n");

  // 7. Главный цикл приема
  uint32_t last_print = 0;
  uint32_t packet_count = 0;

  for (;;) {
    // Проверяем наличие прерываний (бит 0 регистра 0x0C)
    if (BK4819_ReadRegister(0x0C) & 1) {
      BK4819_WriteRegister(0x02, 0x0000);
      uint16_t int_bits = BK4819_ReadRegister(0x02);

      if (RF_FskReceive(int_bits)) {
        packet_count++;

        // Обновляем дисплей не чаще чем раз в 500мс
        if (Now() - last_print > 500) {
          UI_ClearStatus();
          UI_ClearScreen();
          PrintMedium(0, 8, "FSK RX #%u", packet_count);
          PrintSmall(0, 22, "%04X %04X %04X %04X %c", FSK_RXDATA[0],
                     FSK_RXDATA[1], FSK_RXDATA[2], FSK_RXDATA[3],
                     BK4819_ReadRegister(0x0B) & 0x10 ? 'V' : 'X');
          ST7565_Blit();
          last_print = Now();
        }
      }
    }

    // Небольшая задержка для снижения нагрузки
    SYSTICK_DelayMs(1);
  }

#endif

  // SYS_Main();
}
