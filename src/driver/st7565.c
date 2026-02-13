#include <stdint.h>
#include <stdio.h> // NULL
#include <string.h>

#include "../settings.h"
#include "gpio.h"
#include "py32f071_ll_bus.h"
#include "py32f071_ll_gpio.h"
#include "py32f071_ll_spi.h"
#include "st7565.h"
#include "systick.h"

#define SPIx SPI1

#define PIN_CS GPIO_MAKE_PIN(GPIOB, LL_GPIO_PIN_2)
#define PIN_A0 GPIO_MAKE_PIN(GPIOA, LL_GPIO_PIN_6)

uint8_t gFrameBuffer[FRAME_LINES][LCD_WIDTH];
uint8_t gPrevFrameBuffer[FRAME_LINES][LCD_WIDTH]; // Предыдущий кадр
bool gLineChanged[FRAME_LINES]; // Флаги изменений строк

static uint32_t gLastRender;
bool gRedrawScreen = true;

static void SPI_Init() {
  LL_APB1_GRP2_EnableClock(LL_APB1_GRP2_PERIPH_SPI1);
  LL_IOP_GRP1_EnableClock(LL_IOP_GRP1_PERIPH_GPIOA);

  do {
    LL_GPIO_InitTypeDef InitStruct;
    LL_GPIO_StructInit(&InitStruct);
    InitStruct.Mode = LL_GPIO_MODE_ALTERNATE;
    InitStruct.Alternate = LL_GPIO_AF0_SPI1;
    InitStruct.OutputType = LL_GPIO_OUTPUT_PUSHPULL;
    InitStruct.Speed = LL_GPIO_SPEED_FREQ_VERY_HIGH;

    // SCK: PA5
    InitStruct.Pin = LL_GPIO_PIN_5;
    InitStruct.Pull = LL_GPIO_PULL_UP;
    LL_GPIO_Init(GPIOA, &InitStruct);

    // SDA: PA7
    InitStruct.Pin = LL_GPIO_PIN_7;
    InitStruct.Pull = LL_GPIO_PULL_NO;
    LL_GPIO_Init(GPIOA, &InitStruct);
  } while (0);

  LL_SPI_InitTypeDef InitStruct;
  LL_SPI_StructInit(&InitStruct);
  InitStruct.TransferDirection = LL_SPI_FULL_DUPLEX;
  InitStruct.Mode = LL_SPI_MODE_MASTER;
  InitStruct.DataWidth = LL_SPI_DATAWIDTH_8BIT;
  InitStruct.ClockPolarity = LL_SPI_POLARITY_HIGH;
  InitStruct.ClockPhase = LL_SPI_PHASE_2EDGE;
  InitStruct.NSS = LL_SPI_NSS_SOFT;
  InitStruct.BitOrder = LL_SPI_MSB_FIRST;
  InitStruct.CRCCalculation = LL_SPI_CRCCALCULATION_DISABLE;
  InitStruct.BaudRate = LL_SPI_BAUDRATEPRESCALER_DIV32;
  LL_SPI_Init(SPIx, &InitStruct);

  LL_SPI_Enable(SPIx);
}

static inline void CS_Assert() {
  __disable_irq();
  GPIO_ResetOutputPin(PIN_CS);
}

static inline void CS_Release() {
  GPIO_SetOutputPin(PIN_CS);
  __enable_irq();
}

static inline void A0_Set() { GPIO_SetOutputPin(PIN_A0); }

static inline void A0_Reset() { GPIO_ResetOutputPin(PIN_A0); }

static inline uint8_t SPI_WriteByte(uint8_t Value) {
  while (!LL_SPI_IsActiveFlag_TXE(SPIx))
    ;
  LL_SPI_TransmitData8(SPIx, Value);
  while (!LL_SPI_IsActiveFlag_RXNE(SPIx))
    ;
  return LL_SPI_ReceiveData8(SPIx);
}

// Пометить строку как изменённую (вызывать при изменении данных)
void ST7565_MarkLineDirty(uint8_t line) {
  if (line < FRAME_LINES) {
    gRedrawScreen = true;
  }
}

// Пометить область как изменённую
void ST7565_MarkRegionDirty(uint8_t start_line, uint8_t end_line) {
  for (uint8_t line = start_line; line <= end_line && line < FRAME_LINES;
       line++) {
    gRedrawScreen = true;
  }
}

// Оптимизация: отправка буфера DMA-style (пакетная передача)
static void DrawLine(uint8_t column, uint8_t line, const uint8_t *lineBuffer,
                     unsigned size) {
  ST7565_SelectColumnAndLine(column + 4, line);
  A0_Set();

  if (lineBuffer) {
    // Отправляем реальные данные
    for (unsigned i = 0; i < size; i++) {
      SPI_WriteByte(lineBuffer[i]);
    }
  } else {
    // Заполняем константой (для FillScreen)
    for (unsigned i = 0; i < size; i++) {
      SPI_WriteByte(size); // size используется как значение
    }
  }
}

void ST7565_ForceFullRedraw(void) {
  // Помечаем все строки как изменённые и копируем текущий буфер в предыдущий
  for (uint8_t line = 0; line < FRAME_LINES; line++) {
    memcpy(gPrevFrameBuffer[line], gFrameBuffer[line], LCD_WIDTH);
  }
  gRedrawScreen = true;
}

void ST7565_DrawLine(const unsigned int Column, const unsigned int Line,
                     const uint8_t *pBitmap, const unsigned int Size) {
  CS_Assert();
  DrawLine(Column, Line, pBitmap, Size);
  CS_Release();
}

// Оптимизированная отрисовка - только изменённые строки
void ST7565_Blit(void) {
  bool any_change = false;

  // Определяем изменённые строки
  for (uint8_t line = 0; line < FRAME_LINES; line++) {
    gLineChanged[line] =
        memcmp(gFrameBuffer[line], gPrevFrameBuffer[line], LCD_WIDTH) != 0;
    if (gLineChanged[line])
      any_change = true;
  }

  if (!any_change) {
    gRedrawScreen = false;
    return;
  }

  CS_Assert();
  ST7565_WriteByte(0x40); // Start line

  for (uint8_t line = 0; line < FRAME_LINES; line++) {
    if (gLineChanged[line]) {
      DrawLine(0, line, gFrameBuffer[line], LCD_WIDTH);
      // Копируем в буфер предыдущего кадра
      memcpy(gPrevFrameBuffer[line], gFrameBuffer[line], LCD_WIDTH);
    }
  }

  CS_Release();
  gRedrawScreen = false;
}

void ST7565_BlitLine(unsigned line) {
  if (line >= FRAME_LINES)
    return;

  // Проверяем, изменилась ли строка
  if (memcmp(gFrameBuffer[line], gPrevFrameBuffer[line], LCD_WIDTH) == 0) {
    return; // Строка не изменилась
  }

  CS_Assert();
  ST7565_WriteByte(0x40);
  DrawLine(0, line, gFrameBuffer[line], LCD_WIDTH);
  CS_Release();

  // Обновляем буфер предыдущего кадра
  memcpy(gPrevFrameBuffer[line], gFrameBuffer[line], LCD_WIDTH);
}
/* void ST7565_BlitLine(unsigned line) {
  if (line >= FRAME_LINES)
    return;

  CS_Assert();
  ST7565_WriteByte(0x40);
  DrawLine(0, line, gFrameBuffer[line], LCD_WIDTH);
  CS_Release();
} */

void ST7565_FillScreen(uint8_t value) {
  memset(gFrameBuffer, value, sizeof(gFrameBuffer));

  // При заполнении экрана помечаем все строки как изменённые
  for (uint8_t i = 0; i < FRAME_LINES; i++) {
    memset(gPrevFrameBuffer[i], ~value,
           LCD_WIDTH); // Заполняем противоположным значением
  }

  gRedrawScreen = true;
}
/* void ST7565_FillScreen(uint8_t value) {
  CS_Assert();
  for (unsigned i = 0; i < FRAME_LINES; i++) {
    DrawLine(0, i, NULL, value);
  }
  CS_Release();

  // Синхронизируем буфер с экраном
  memset(gFrameBuffer, value, sizeof(gFrameBuffer));
} */

// Команды ST7565
#define ST7565_CMD_SOFTWARE_RESET 0xE2
#define ST7565_CMD_BIAS_SELECT 0xA2      // +0=1/9, +1=1/7
#define ST7565_CMD_COM_DIRECTION 0xC0    // +8=reverse
#define ST7565_CMD_SEG_DIRECTION 0xA0    // +1=reverse
#define ST7565_CMD_INVERSE_DISPLAY 0xA6  // +1=inverse
#define ST7565_CMD_ALL_PIXEL_ON 0xA4     // +1=all on
#define ST7565_CMD_REGULATION_RATIO 0x20 // +0..+7 (3.0..6.5)
#define ST7565_CMD_SET_EV 0x81 // Следующий байт: контраст 0-63
#define ST7565_CMD_POWER_CIRCUIT 0x28  // +bit0=VF, +bit1=VR, +bit2=VB
#define ST7565_CMD_SET_START_LINE 0x40 // +0..+63
#define ST7565_CMD_DISPLAY_ON_OFF 0xAE // +1=ON

// Базовые команды инициализации (const для Flash)
static const uint8_t init_cmds[] = {
    ST7565_CMD_BIAS_SELECT | 0,          // 1/9 bias
    ST7565_CMD_COM_DIRECTION | (0 << 3), // Normal COM direction
    ST7565_CMD_SEG_DIRECTION | 1,        // Reverse SEG direction
    ST7565_CMD_INVERSE_DISPLAY | 0,      // Normal display
    ST7565_CMD_ALL_PIXEL_ON | 0,         // Normal pixel mode
    ST7565_CMD_REGULATION_RATIO | 4,     // 5.0 regulation ratio
};

void ST7565_Init(void) {
  SPI_Init();

  CS_Assert();

  // Software reset
  ST7565_WriteByte(ST7565_CMD_SOFTWARE_RESET);
  SYSTICK_DelayMs(5);

  // Базовые настройки
  for (uint8_t i = 0; i < sizeof(init_cmds); i++) {
    ST7565_WriteByte(init_cmds[i]);
  }

  // Контраст из настроек (23 - базовое значение, как в оригинале)
  ST7565_WriteByte(ST7565_CMD_SET_EV);
  ST7565_WriteByte(23 + gSettings.contrast);

  // Постепенное включение питания для стабильности
  ST7565_WriteByte(ST7565_CMD_POWER_CIRCUIT | 0b011); // VR=1, VF=1
  SYSTICK_DelayMs(1);
  ST7565_WriteByte(ST7565_CMD_POWER_CIRCUIT | 0b110); // VB=1, VR=1
  SYSTICK_DelayMs(1);

  // Полное включение питания (повторяем 4 раза для надежности)
  for (uint8_t i = 0; i < 4; i++) {
    ST7565_WriteByte(ST7565_CMD_POWER_CIRCUIT | 0b111); // VB=1, VR=1, VF=1
  }
  SYSTICK_DelayMs(10);

  // Финальные настройки
  ST7565_WriteByte(ST7565_CMD_SET_START_LINE | 0); // Start line = 0
  ST7565_WriteByte(ST7565_CMD_DISPLAY_ON_OFF | 1); // Display ON

  CS_Release();

  // Очистка экрана
  ST7565_FillScreen(0x00);
  memset(gFrameBuffer, 0, sizeof(gFrameBuffer));
  memset(gPrevFrameBuffer, 0xFF,
         sizeof(gPrevFrameBuffer)); // Заполняем противоположными значениями
  gRedrawScreen = true;
}

// Обновление контраста на лету
void ST7565_SetContrast(uint8_t contrast) {
  CS_Assert();
  ST7565_WriteByte(ST7565_CMD_SET_EV);
  ST7565_WriteByte(23 + contrast);
  CS_Release();
}

// Быстрое восстановление после сбоев интерфейса
void ST7565_FixInterfGlitch(void) {
  CS_Assert();

  // Повторяем критичные команды инициализации
  for (uint8_t i = 0; i < sizeof(init_cmds); i++) {
    ST7565_WriteByte(init_cmds[i]);
  }

  ST7565_WriteByte(ST7565_CMD_SET_EV);
  ST7565_WriteByte(23 + gSettings.contrast);

  ST7565_WriteByte(ST7565_CMD_POWER_CIRCUIT | 0b111);
  ST7565_WriteByte(ST7565_CMD_SET_START_LINE | 0);
  ST7565_WriteByte(ST7565_CMD_DISPLAY_ON_OFF | 1);

  CS_Release();
}

void ST7565_SelectColumnAndLine(uint8_t Column, uint8_t Line) {
  A0_Reset();                                   // Command mode
  SPI_WriteByte(Line + 0xB0);                   // Page address
  SPI_WriteByte(((Column >> 4) & 0x0F) | 0x10); // Column high nibble
  SPI_WriteByte((Column & 0x0F));               // Column low nibble
}

void ST7565_WriteByte(uint8_t Value) {
  A0_Reset(); // Command mode
  SPI_WriteByte(Value);
}
