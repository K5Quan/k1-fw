#include <stdint.h>
#include <stdio.h>
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

// Контрольные суммы строк вместо gPrevFrameBuffer[FRAME_LINES][LCD_WIDTH].
// Экономия: 1024 - 32 = 992 байта.
// Алгоритм: сумма uint32_t слов строки (wrapping add).
// Вероятность коллизии при случайных изменениях: ~1/2^32 на строку.
static uint32_t gLineChecksum[FRAME_LINES];

bool gLineChanged[FRAME_LINES];

static uint32_t gLastRender;
bool gRedrawScreen = true;

// ---------------------------------------------------------------------------
// Checksum helpers
// ---------------------------------------------------------------------------

static inline uint32_t compute_line_checksum(uint8_t line) {
  const uint32_t *p = (const uint32_t *)gFrameBuffer[line];
  uint32_t sum = 0;
  // LCD_WIDTH кратен 4 (обычно 128), остаток обрабатываем отдельно
  for (uint8_t i = 0; i < LCD_WIDTH / 4; i++) {
    sum += p[i];
  }
#if (LCD_WIDTH % 4) != 0
  for (uint8_t i = (LCD_WIDTH / 4) * 4; i < LCD_WIDTH; i++) {
    sum += gFrameBuffer[line][i];
  }
#endif
  return sum;
}

static inline void update_line_checksum(uint8_t line) {
  gLineChecksum[line] = compute_line_checksum(line);
}

// ---------------------------------------------------------------------------
// SPI
// ---------------------------------------------------------------------------

static void SPI_Init() {
  LL_APB1_GRP2_EnableClock(LL_APB1_GRP2_PERIPH_SPI1);
  LL_IOP_GRP1_EnableClock(LL_IOP_GRP1_PERIPH_GPIOA);

  LL_GPIO_InitTypeDef InitStruct;
  LL_GPIO_StructInit(&InitStruct);
  InitStruct.Mode = LL_GPIO_MODE_ALTERNATE;
  InitStruct.Alternate = LL_GPIO_AF0_SPI1;
  InitStruct.OutputType = LL_GPIO_OUTPUT_PUSHPULL;
  InitStruct.Speed = LL_GPIO_SPEED_FREQ_VERY_HIGH;

  InitStruct.Pin = LL_GPIO_PIN_5;
  InitStruct.Pull = LL_GPIO_PULL_UP;
  LL_GPIO_Init(GPIOA, &InitStruct);

  InitStruct.Pin = LL_GPIO_PIN_7;
  InitStruct.Pull = LL_GPIO_PULL_NO;
  LL_GPIO_Init(GPIOA, &InitStruct);

  LL_SPI_InitTypeDef SPI_InitStruct;
  LL_SPI_StructInit(&SPI_InitStruct);
  SPI_InitStruct.TransferDirection = LL_SPI_FULL_DUPLEX;
  SPI_InitStruct.Mode = LL_SPI_MODE_MASTER;
  SPI_InitStruct.DataWidth = LL_SPI_DATAWIDTH_8BIT;
  SPI_InitStruct.ClockPolarity = LL_SPI_POLARITY_HIGH;
  SPI_InitStruct.ClockPhase = LL_SPI_PHASE_2EDGE;
  SPI_InitStruct.NSS = LL_SPI_NSS_SOFT;
  SPI_InitStruct.BitOrder = LL_SPI_MSB_FIRST;
  SPI_InitStruct.CRCCalculation = LL_SPI_CRCCALCULATION_DISABLE;
  SPI_InitStruct.BaudRate = LL_SPI_BAUDRATEPRESCALER_DIV2;
  LL_SPI_Init(SPIx, &SPI_InitStruct);

  LL_SPI_Enable(SPIx);
}

static inline void CS_Assert() { GPIO_ResetOutputPin(PIN_CS); }

static inline void CS_Release() {
  while (LL_SPI_IsActiveFlag_BSY(SPIx))
    ;
  GPIO_SetOutputPin(PIN_CS);
}

static inline void A0_Set() { GPIO_SetOutputPin(PIN_A0); }

static inline void A0_Reset() { GPIO_ResetOutputPin(PIN_A0); }

static inline void SPI_WriteByte(uint8_t Value) {
  while (!LL_SPI_IsActiveFlag_TXE(SPIx))
    ;
  LL_SPI_TransmitData8(SPIx, Value);
}

void ST7565_SelectColumnAndLine(uint8_t Column, uint8_t Line) {
  A0_Reset();
  SPI_WriteByte(Line + 0xB0);
  SPI_WriteByte(((Column >> 4) & 0x0F) | 0x10);
  SPI_WriteByte((Column & 0x0F));
  while (LL_SPI_IsActiveFlag_BSY(SPIx))
    ;
}

void ST7565_WriteByte(uint8_t Value) {
  A0_Reset();
  SPI_WriteByte(Value);
  while (LL_SPI_IsActiveFlag_BSY(SPIx))
    ;
}

static void DrawLine(uint8_t column, uint8_t line, const uint8_t *lineBuffer,
                     unsigned size) {
  ST7565_SelectColumnAndLine(column + 4, line);
  A0_Set();

  if (lineBuffer) {
    for (unsigned i = 0; i < size; i++) {
      SPI_WriteByte(lineBuffer[i]);
    }
  } else {
    for (unsigned i = 0; i < size; i++) {
      SPI_WriteByte(size);
    }
  }
  while (LL_SPI_IsActiveFlag_BSY(SPIx))
    ;
}

void ST7565_DrawLine(const unsigned int Column, const unsigned int Line,
                     const uint8_t *pBitmap, const unsigned int Size) {
  CS_Assert();
  DrawLine(Column, Line, pBitmap, Size);
  CS_Release();
}

void ST7565_MarkLineDirty(uint8_t line) {
  if (line < FRAME_LINES) {
    gRedrawScreen = true;
  }
}

void ST7565_MarkRegionDirty(uint8_t start_line, uint8_t end_line) {
  (void)start_line;
  (void)end_line;
  gRedrawScreen = true;
}

void ST7565_ForceFullRedraw(void) {
  // Инвалидируем все checksums → все строки будут перерисованы
  memset(gLineChecksum, 0xFF, sizeof(gLineChecksum));
  gRedrawScreen = true;
}

void ST7565_Blit(void) {
  bool any_change = false;

  for (uint8_t line = 0; line < FRAME_LINES; line++) {
    uint32_t cs = compute_line_checksum(line);
    gLineChanged[line] = (cs != gLineChecksum[line]);
    if (gLineChanged[line]) {
      any_change = true;
    }
  }

  if (!any_change) {
    gRedrawScreen = false;
    return;
  }

  CS_Assert();

  uint8_t start_line = 0xFF;
  for (uint8_t line = 0; line < FRAME_LINES; line++) {
    if (gLineChanged[line]) {
      if (start_line == 0xFF)
        start_line = line;

      DrawLine(0, line, gFrameBuffer[line], LCD_WIDTH);
      update_line_checksum(line); // обновляем checksum вместо memcpy

      if (line + 1 < FRAME_LINES && !gLineChanged[line + 1]) {
        start_line = 0xFF;
      }
    }
  }

  CS_Release();
  gRedrawScreen = false;
}

void ST7565_BlitLine(unsigned line) {
  if (line >= FRAME_LINES)
    return;

  uint32_t cs = compute_line_checksum(line);
  if (cs == gLineChecksum[line])
    return;

  CS_Assert();
  ST7565_WriteByte(0x40);
  DrawLine(0, line, gFrameBuffer[line], LCD_WIDTH);
  CS_Release();

  gLineChecksum[line] = cs;
}

void ST7565_FillScreen(uint8_t value) {
  memset(gFrameBuffer, value, sizeof(gFrameBuffer));
  // Инвалидируем checksums, чтобы Blit точно отправил данные
  memset(gLineChecksum, ~0, sizeof(gLineChecksum));
  gRedrawScreen = true;
}

#define ST7565_CMD_SOFTWARE_RESET 0xE2
#define ST7565_CMD_BIAS_SELECT 0xA2
#define ST7565_CMD_COM_DIRECTION 0xC0
#define ST7565_CMD_SEG_DIRECTION 0xA0
#define ST7565_CMD_INVERSE_DISPLAY 0xA6
#define ST7565_CMD_ALL_PIXEL_ON 0xA4
#define ST7565_CMD_REGULATION_RATIO 0x20
#define ST7565_CMD_SET_EV 0x81
#define ST7565_CMD_POWER_CIRCUIT 0x28
#define ST7565_CMD_SET_START_LINE 0x40
#define ST7565_CMD_DISPLAY_ON_OFF 0xAE

static const uint8_t init_cmds[] = {
    ST7565_CMD_BIAS_SELECT | 0,   ST7565_CMD_COM_DIRECTION | (0 << 3),
    ST7565_CMD_SEG_DIRECTION | 1, ST7565_CMD_INVERSE_DISPLAY | 0,
    ST7565_CMD_ALL_PIXEL_ON | 0,  ST7565_CMD_REGULATION_RATIO | 4,
};

void ST7565_Init(void) {
  SPI_Init();

  CS_Assert();

  ST7565_WriteByte(ST7565_CMD_SOFTWARE_RESET);
  SYSTICK_DelayMs(5);

  for (uint8_t i = 0; i < sizeof(init_cmds); i++) {
    ST7565_WriteByte(init_cmds[i]);
  }

  ST7565_WriteByte(ST7565_CMD_SET_EV);
  ST7565_WriteByte(23 + gSettings.contrast);

  ST7565_WriteByte(ST7565_CMD_POWER_CIRCUIT | 0b011);
  SYSTICK_DelayMs(1);
  ST7565_WriteByte(ST7565_CMD_POWER_CIRCUIT | 0b110);
  SYSTICK_DelayMs(1);

  for (uint8_t i = 0; i < 4; i++) {
    ST7565_WriteByte(ST7565_CMD_POWER_CIRCUIT | 0b111);
  }
  SYSTICK_DelayMs(10);

  ST7565_WriteByte(ST7565_CMD_SET_START_LINE | 0);
  ST7565_WriteByte(ST7565_CMD_DISPLAY_ON_OFF | 1);

  CS_Release();

  // Нули в framebuffer, все checksums ≠ 0 → первый Blit прогонит все строки
  memset(gFrameBuffer, 0, sizeof(gFrameBuffer));
  memset(gLineChecksum, 0xFF, sizeof(gLineChecksum));
  gRedrawScreen = true;
}

void ST7565_SetContrast(uint8_t contrast) {
  CS_Assert();
  ST7565_WriteByte(ST7565_CMD_SET_EV);
  ST7565_WriteByte(23 + contrast);
  CS_Release();
}

void ST7565_FixInterfGlitch(void) {
  CS_Assert();

  for (uint8_t i = 0; i < sizeof(init_cmds); i++) {
    ST7565_WriteByte(init_cmds[i]);
  }

  ST7565_WriteByte(ST7565_CMD_SET_EV);
  ST7565_WriteByte(23 + gSettings.contrast);

  ST7565_WriteByte(ST7565_CMD_POWER_CIRCUIT | 0b111);
  ST7565_WriteByte(ST7565_CMD_SET_START_LINE | 0);
  ST7565_WriteByte(ST7565_CMD_DISPLAY_ON_OFF | 1);

  CS_Release();

  // После glitch-fix дисплей не знает что на экране — форсируем полную перерисовку
  memset(gLineChecksum, 0xFF, sizeof(gLineChecksum));
  gRedrawScreen = true;
}

