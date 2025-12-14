#include "board.h"
#include "driver/audio.h"
#include "driver/backlight.h"
#include "driver/bk4829.h"
#include "driver/crc.h"
#include "driver/keyboard.h"
#include "driver/st7565.h"
#include "driver/systick.h"
#include "driver/uart.h"
#include "external/printf/printf.h"
#include "ui/graphics.h"
#include <stdbool.h>

static uint32_t fInitial = 17230000;

#include "config/usb_config.h"
#include "driver/usb_msc.h"
#include "py32f071_ll_bus.h"

// Тест флеш перед инициализацией USB
void test_flash(void) {
  uint8_t test_buf[512];
  uint8_t read_buf[512];

  // Заполняем тестовым паттерном
  for (int i = 0; i < 512; i++) {
    test_buf[i] = i & 0xFF;
  }

  PY25Q16_Init();

  // Стираем сектор
  PY25Q16_SectorErase(0);
  for (volatile int i = 0; i < 100000; i++)
    ;

  // Пишем
  PY25Q16_WriteBuffer(0, test_buf, 512, false);
  for (volatile int i = 0; i < 100000; i++)
    ;

  // Читаем
  PY25Q16_ReadBuffer(0, read_buf, 512);

  // Проверяем
  bool ok = true;
  for (int i = 0; i < 512; i++) {
    if (read_buf[i] != test_buf[i]) {
      ok = false;
      break;
    }
  }

  if (ok) {
    // Мигнуть зелёным 3 раза = флеш работает
    for (int i = 0; i < 3; i++) {
      BK4819_ToggleGpioOut(BK4819_GREEN, true);
      for (volatile int j = 0; j < 500000; j++)
        ;
      BK4819_ToggleGpioOut(BK4819_GREEN, false);
      for (volatile int j = 0; j < 500000; j++)
        ;
    }
  } else {
    // Зелёный постоянно = флеш НЕ работает
    BK4819_ToggleGpioOut(BK4819_GREEN, true);
    while (1)
      ;
  }
}

int main(void) {
  SYSTICK_Init();
  BOARD_Init();
  UART_Init();

  BK4819_Init();
  BK4819_ToggleGpioOut(BK4819_GPIO0_PIN28_RX_ENABLE, true);
  BK4819_RX_TurnOn();

  BK4819_TuneTo(fInitial, true);
  BK4819_SelectFilter(fInitial);
  BK4819_SetFilterBandwidth(BK4819_FILTER_BW_12k);
  BK4819_SetModulation(MOD_FM);
  BK4819_SetAGC(true, 0);

  AUDIO_AudioPathOn();

  BACKLIGHT_TurnOn();

  // test_flash();

  FAT_Init();

  // Инициализация USB MSC
  LL_APB1_GRP2_EnableClock(LL_APB1_GRP2_PERIPH_SYSCFG);
  LL_IOP_GRP1_EnableClock(LL_IOP_GRP1_PERIPH_GPIOA); // PA12:11
  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_USBD);

  for (volatile int i = 0; i < 100000; i++)
    ;

  NVIC_SetPriority(USBD_IRQn, 3);
  NVIC_EnableIRQ(USBD_IRQn);

  msc_init();

  for (;;) {
    // Ничего не делаем - только USB прерывания
    __WFI(); // Wait For Interrupt - экономит энергию
  }

  bool b = false;
  for (;;) {
    bool b = !b;
    BK4819_SelectFilterEx(b ? FILTER_UHF : FILTER_VHF);
    printf("RSSI=%u\n", BK4819_GetRSSI());
    // BOARD_FlashlightToggle();
    UI_ClearScreen();
    PrintBigDigitsEx(LCD_WIDTH - 1, 32, POS_R, C_FILL, "%u",
                     BK4819_GetFrequency());
    PrintMedium(0, 40, "RSSI: %u", BK4819_GetRSSI());
    PrintMedium(0, 48, "NOW: %u", Now());
    PrintMedium(0, 56, "Key: %u", KEYBOARD_Poll());
    ST7565_BlitFullScreen();
    SYSTICK_DelayMs(500);
  }
}
