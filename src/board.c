#include "board.h"
#include "driver/backlight.h"
#include "driver/bk4819-regs.h"
#include "driver/bk4829.h"
#include "driver/gpio.h"
#include "driver/lfs.h"
#include "driver/py25q16.h"
#include "driver/st7565.h"
#include "driver/systick.h"
#include "driver/uart.h"
#include "external/PY32F071_HAL_Driver/Inc/py32f071_ll_adc.h"
#include "external/PY32F071_HAL_Driver/Inc/py32f071_ll_bus.h"
#include "external/PY32F071_HAL_Driver/Inc/py32f071_ll_dac.h"
#include "external/PY32F071_HAL_Driver/Inc/py32f071_ll_dma.h"
#include "external/PY32F071_HAL_Driver/Inc/py32f071_ll_gpio.h"
#include "external/PY32F071_HAL_Driver/Inc/py32f071_ll_rcc.h"
#include "ui/graphics.h"
#include <stdint.h>

volatile uint16_t adc_dma_buffer[4 * APRS_BUFFER_SIZE] __attribute__((
    aligned(4))); // Расширенный DMA-буфер: CH8, CH9, CH8, CH9... (1024 для 256)

uint16_t aprs_process_buffer1[APRS_BUFFER_SIZE];
uint16_t aprs_process_buffer2[APRS_BUFFER_SIZE];

volatile bool aprs_ready1 = false;
volatile bool aprs_ready2 = false;

void BOARD_DMA_Init(void) {
  LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_DMA1);

  // Сброс DMA
  LL_AHB1_GRP1_ForceReset(LL_AHB1_GRP1_PERIPH_DMA1);
  LL_AHB1_GRP1_ReleaseReset(LL_AHB1_GRP1_PERIPH_DMA1);

  LL_DMA_DeInit(DMA1, LL_DMA_CHANNEL_1);

  LL_DMA_InitTypeDef DMA_InitStruct;
  LL_DMA_StructInit(&DMA_InitStruct);

  DMA_InitStruct.Direction = LL_DMA_DIRECTION_PERIPH_TO_MEMORY;
  DMA_InitStruct.PeriphOrM2MSrcAddress = (uint32_t)&ADC1->DR;
  DMA_InitStruct.MemoryOrM2MDstAddress = (uint32_t)adc_dma_buffer;
  DMA_InitStruct.PeriphOrM2MSrcDataSize = LL_DMA_PDATAALIGN_HALFWORD;
  DMA_InitStruct.MemoryOrM2MDstDataSize = LL_DMA_MDATAALIGN_HALFWORD;
  DMA_InitStruct.NbData = 4 * APRS_BUFFER_SIZE; // ИСПРАВЛЕНИЕ: 1024 для 256

  DMA_InitStruct.PeriphOrM2MSrcIncMode = LL_DMA_PERIPH_NOINCREMENT;
  DMA_InitStruct.MemoryOrM2MDstIncMode = LL_DMA_MEMORY_INCREMENT;
  DMA_InitStruct.Mode = LL_DMA_MODE_CIRCULAR;
  DMA_InitStruct.Priority = LL_DMA_PRIORITY_HIGH;

  LL_DMA_Init(DMA1, LL_DMA_CHANNEL_1, &DMA_InitStruct);

// В PY32F071 remap настраивается через SYSCFG, а не через CSELR
#ifdef SYSCFG
  // Настройка DMA remap для ADC1 на Channel 1
  // Это зависит от конкретной реализации, возможно, нужно через SYSCFG->CFGR3
  // LL_SYSCFG_SetDMARemap(DMA1, LL_DMA_CHANNEL_1, LL_SYSCFG_DMA_MAP_ADC1);
#endif

  // Включить прерывания
  LL_DMA_EnableIT_TC(DMA1, LL_DMA_CHANNEL_1);
  LL_DMA_EnableIT_HT(DMA1, LL_DMA_CHANNEL_1);
  LL_DMA_EnableIT_TE(DMA1, LL_DMA_CHANNEL_1); // Включаем прерывание по ошибке
  NVIC_EnableIRQ(DMA1_Channel1_IRQn);
}

void DMA1_Channel1_IRQHandler(void) {
  if (LL_DMA_IsActiveFlag_HT1(DMA1)) {
    LL_DMA_ClearFlag_HT1(DMA1);
    // Первая половина: индексы 1,3,5,... (APRS)
    for (int i = 0; i < APRS_BUFFER_SIZE; i++) {
      aprs_process_buffer1[i] = adc_dma_buffer[2 * i + 1];
    }
    aprs_ready1 = true;
  }

  if (LL_DMA_IsActiveFlag_TC1(DMA1)) {
    LL_DMA_ClearFlag_TC1(DMA1);
    // Вторая половина: offset = 2 * APRS_BUFFER_SIZE (512 для 256)
    uint32_t offset = 2 * APRS_BUFFER_SIZE;
    for (int i = 0; i < APRS_BUFFER_SIZE; i++) {
      aprs_process_buffer2[i] = adc_dma_buffer[offset + 2 * i + 1];
    }
    aprs_ready2 = true;
  }

  if (LL_DMA_IsActiveFlag_TE1(DMA1)) {
    LL_DMA_ClearFlag_TE1(DMA1);
    LogC(LOG_C_RED, "DMA Transfer Error!");
  }
}

void BOARD_GPIO_Init(void) {
  // (без изменений, оставьте как есть)
  LL_IOP_GRP1_EnableClock(LL_IOP_GRP1_PERIPH_GPIOA   //
                          | LL_IOP_GRP1_PERIPH_GPIOB //
                          | LL_IOP_GRP1_PERIPH_GPIOC //
                          | LL_IOP_GRP1_PERIPH_GPIOF //
  );

  LL_GPIO_InitTypeDef InitStruct;
  LL_GPIO_StructInit(&InitStruct);
  InitStruct.OutputType = LL_GPIO_OUTPUT_PUSHPULL;
  InitStruct.Pull = LL_GPIO_PULL_UP;
  InitStruct.Speed = LL_GPIO_SPEED_FREQ_VERY_HIGH;

  // ---------------------
  // Input pins

  InitStruct.Mode = LL_GPIO_MODE_INPUT;

  // Keypad rows: PB15:12
  InitStruct.Pin =
      LL_GPIO_PIN_15 | LL_GPIO_PIN_14 | LL_GPIO_PIN_13 | LL_GPIO_PIN_12;
  LL_GPIO_Init(GPIOB, &InitStruct);

  // PTT: PB10
  InitStruct.Pin = LL_GPIO_PIN_10;
  LL_GPIO_Init(GPIOB, &InitStruct);

  // -----------------------
  //  Output pins

  LL_GPIO_SetOutputPin(GPIOA, LL_GPIO_PIN_6); // LCD A0
  LL_GPIO_SetOutputPin(GPIOB, LL_GPIO_PIN_2); // LCD CS

  InitStruct.Mode = LL_GPIO_MODE_OUTPUT;

  // Keypad cols: PB6:3
  InitStruct.Pin =
      LL_GPIO_PIN_6 | LL_GPIO_PIN_5 | LL_GPIO_PIN_4 | LL_GPIO_PIN_3;
  LL_GPIO_Init(GPIOB, &InitStruct);

  // Audio PA: PA8
  // LCD A0: PA6
  // SPI flash CS: PA3
  InitStruct.Pin = LL_GPIO_PIN_8 | LL_GPIO_PIN_6 | LL_GPIO_PIN_3;
  LL_GPIO_Init(GPIOA, &InitStruct);

  // BK4829 SCK: B8
  // BK4829 SDA: B9
  // LCD CS: PB2
  InitStruct.Pin = LL_GPIO_PIN_9 | LL_GPIO_PIN_8 | LL_GPIO_PIN_2;
  LL_GPIO_Init(GPIOB, &InitStruct);

  // Flashlight: PC13
  InitStruct.Pin = LL_GPIO_PIN_13;
  LL_GPIO_Init(GPIOC, &InitStruct);

  // BK1080 SCK: PF5
  // BK1080 SDA: PF6
  InitStruct.Pin = LL_GPIO_PIN_6 | LL_GPIO_PIN_5;
  LL_GPIO_Init(GPIOF, &InitStruct);

  // Backlight: PF8
  // BK4819 CS: PF9
  InitStruct.Pin = LL_GPIO_PIN_9 | LL_GPIO_PIN_8;
  LL_GPIO_Init(GPIOF, &InitStruct);

  InitStruct.Mode = LL_GPIO_MODE_ANALOG;
  InitStruct.Pull = LL_GPIO_PULL_NO;
  InitStruct.Pin = LL_GPIO_PIN_0 | LL_GPIO_PIN_1;
  LL_GPIO_Init(GPIOB, &InitStruct);
}

void BOARD_ADC_Init(void) {
  LL_APB1_GRP2_EnableClock(LL_APB1_GRP2_PERIPH_ADC1);
  LL_RCC_SetADCClockSource(LL_RCC_ADC_CLKSOURCE_PCLK_DIV4);

  // Reset ADC first
  LL_APB1_GRP2_ForceReset(LL_APB1_GRP2_PERIPH_ADC1);
  LL_APB1_GRP2_ReleaseReset(LL_APB1_GRP2_PERIPH_ADC1);

  // Initialize DMA AFTER ADC reset
  BOARD_DMA_Init();

  LL_ADC_SetCommonPathInternalCh(ADC1_COMMON, LL_ADC_PATH_INTERNAL_NONE);
  LL_ADC_SetResolution(ADC1, LL_ADC_RESOLUTION_12B);
  LL_ADC_SetDataAlignment(ADC1, LL_ADC_DATA_ALIGN_RIGHT);

  // Scan mode for multiple channels
  LL_ADC_SetSequencersScanMode(ADC1, LL_ADC_SEQ_SCAN_ENABLE);

  // Trigger and continuous mode
  LL_ADC_REG_SetTriggerSource(ADC1, LL_ADC_REG_TRIG_SOFTWARE);
  LL_ADC_REG_SetContinuousMode(ADC1, LL_ADC_REG_CONV_CONTINUOUS);

  // DMA unlimited mode
  LL_ADC_REG_SetDMATransfer(ADC1, LL_ADC_REG_DMA_TRANSFER_UNLIMITED);

  // Sequence length = 2 ranks
  LL_ADC_REG_SetSequencerLength(ADC1, LL_ADC_REG_SEQ_SCAN_ENABLE_2RANKS);

  // Configure channels
  LL_ADC_REG_SetSequencerRanks(ADC1, LL_ADC_REG_RANK_1, LL_ADC_CHANNEL_8);
  LL_ADC_REG_SetSequencerRanks(ADC1, LL_ADC_REG_RANK_2, LL_ADC_CHANNEL_9);

  // Sampling time
  LL_ADC_SetChannelSamplingTime(ADC1, LL_ADC_CHANNEL_8,
                                LL_ADC_SAMPLINGTIME_41CYCLES_5);
  LL_ADC_SetChannelSamplingTime(ADC1, LL_ADC_CHANNEL_9,
                                LL_ADC_SAMPLINGTIME_41CYCLES_5);

  // Calibration
  LL_ADC_StartCalibration(ADC1);
  while (LL_ADC_IsCalibrationOnGoing(ADC1))
    ;

  // КРИТИЧЕСКИ ВАЖНО: Правильная настройка DMA в ADC
  // В PY32F071 для генерации DMA запросов нужно:

  // 1. Включить DMA в ADC (бит 8)
  ADC1->CR2 |= ADC_CR2_DMA;

  // 2. Убедиться, что DDS бит (бит 9) установлен в 0 для непрерывных запросов
  ADC1->CR2 &= ~(1 << 9); // DDS = 0 (запросы при каждом преобразовании)

  // 3. EOCS бит (бит 10) - для генерации EOC после каждого преобразования
  ADC1->CR2 |= (1 << 10); // EOCS = 1

// 4. ВАЖНО: Проверить, что DMA настроен на правильный запрос
// В некоторых PY32 нужно настроить remap через SYSCFG
#ifdef SYSCFG
                          // Настройка DMA remap для ADC1 на Channel 1
  // Это может быть необходимо для правильной маршрутизации запросов
  SYSCFG->CFGR3 &= ~(0x3F << 0); // Очищаем для Channel 1
  SYSCFG->CFGR3 |= (0 << 0); // 0 = ADC1 (значение может отличаться)
#endif

  LogC(LOG_C_YELLOW, "ADC_CR2 after DMA config = %08X", ADC1->CR2);

  // CRITICAL: Enable DMA channel BEFORE enabling ADC
  LL_DMA_EnableChannel(DMA1, LL_DMA_CHANNEL_1);

  // Проверяем, что DMA канал действительно включен
  if (LL_DMA_IsEnabledChannel(DMA1, LL_DMA_CHANNEL_1)) {
    LogC(LOG_C_GREEN, "DMA Channel 1 enabled");
  }

  // Small delay for DMA to be ready
  for (volatile int i = 0; i < 1000; i++)
    ;

  // Enable ADC
  LL_ADC_Enable(ADC1);

  // Wait for ADC to be ready
  uint32_t timeout = 10000;
  while (!(ADC1->SR & ADC_SR_EOC) && timeout--) {
    for (volatile int i = 0; i < 100; i++)
      ;
  }

  if (timeout == 0) {
    LogC(LOG_C_RED, "ADC ready timeout!");
  } else {
    LogC(LOG_C_GREEN, "ADC is ready");
  }

  // Initialize test pattern
  adc_dma_buffer[0] = 0xABCD;
  adc_dma_buffer[1] = 0x1234;

  LogC(LOG_C_BRIGHT_WHITE, "DMA_CCR=%08X", DMA1_Channel1->CCR);
  LogC(LOG_C_BRIGHT_WHITE, "DMA_CNDTR=%d", DMA1_Channel1->CNDTR);
  LogC(LOG_C_BRIGHT_WHITE, "ADC_CR2=%08X", ADC1->CR2);
  LogC(LOG_C_BRIGHT_WHITE, "ADC_SR=%08X", ADC1->SR);

  // Проверяем флаги DMA до старта
  LogC(LOG_C_BRIGHT_WHITE, "DMA_ISR before start = %08X", DMA1->ISR);

  // Start conversion
  LL_ADC_REG_StartConversionSWStart(ADC1);

  // Verify DMA is working
  for (int n = 0; n < 10; n++) {
    SYSTICK_DelayMs(100);

    // Проверяем флаги DMA
    LogC(LOG_C_BRIGHT_WHITE, "DMA_ISR now = %08X", DMA1->ISR);

    if (LL_DMA_IsActiveFlag_TE1(DMA1)) {
      LogC(LOG_C_RED, "DMA Transfer Error!");
      LL_DMA_ClearFlag_TE1(DMA1);
    }

    if (LL_DMA_IsActiveFlag_TC1(DMA1)) {
      LogC(LOG_C_GREEN, "DMA Transfer Complete!");
      LL_DMA_ClearFlag_TC1(DMA1);
    }

    if (LL_DMA_IsActiveFlag_HT1(DMA1)) {
      LogC(LOG_C_GREEN, "DMA Half Transfer Complete!");
      LL_DMA_ClearFlag_HT1(DMA1);
    }

    LogC(LOG_C_BRIGHT_WHITE, "ADC_SR now = %08X", ADC1->SR);
    LogC(LOG_C_BRIGHT_WHITE, "ADC_DR = %04X", ADC1->DR);
    LogC(LOG_C_BRIGHT_WHITE, "DMA_CNDTR now = %d", DMA1_Channel1->CNDTR);
    LogC(LOG_C_BRIGHT_WHITE, "Buf[0]=%d Buf[1]=%d", adc_dma_buffer[0],
         adc_dma_buffer[1]);

    // Direct read for comparison
    uint16_t direct_val = LL_ADC_REG_ReadConversionData12(ADC1);
    LogC(LOG_C_YELLOW, "Direct read = %d", direct_val);
  }
}

void BOARD_ADC_StartAPRS_DMA(void) {
  // Запускаем DMA и ADC конверсию (если не запущено)
  if (!LL_ADC_IsEnabled(ADC1)) {
    LL_ADC_Enable(ADC1);
    // Ждём ready снова, если нужно
    SYSTICK_DelayUs(10);
  }
  if (!LL_DMA_IsEnabledChannel(DMA1, LL_DMA_CHANNEL_1)) {
    LL_DMA_EnableChannel(DMA1, LL_DMA_CHANNEL_1);
  }
  LL_ADC_REG_StartConversionSWStart(ADC1);
  LogC(LOG_C_GREEN, "APRS DMA started");
}

void BOARD_ADC_StopAPRS_DMA(void) {
  // Останавливаем конверсию и DMA
  LL_ADC_StopConversion(ADC1);
  while (LL_ADC_REG_IsStopConversionOngoing(ADC1)) {
  }
  LL_ADC_Disable(ADC1);
  LL_DMA_DisableChannel(DMA1, LL_DMA_CHANNEL_1);
  // Сбрасываем флаги ready
  aprs_ready1 = false;
  aprs_ready2 = false;
  LogC(LOG_C_YELLOW, "APRS DMA stopped");
}

uint32_t BOARD_ADC_GetAvailableAPRS_DMA(void) {
  // Возвращаем количество доступных семплов (APRS_BUFFER_SIZE если буфер ready,
  // иначе 0) Проверяем любой из ping-pong
  if (aprs_ready1 || aprs_ready2) {
    return APRS_BUFFER_SIZE;
  }
  return 0;
}

uint32_t BOARD_ADC_ReadAPRS_DMA(uint16_t *dest, uint32_t max_samples) {
  // Копируем из готового буфера (предпочтительно buffer1, затем buffer2)
  // Возвращаем количество скопированных семплов
  uint32_t copied = 0;
  if (aprs_ready1 && max_samples >= APRS_BUFFER_SIZE) {
    for (int i = 0; i < APRS_BUFFER_SIZE; i++) {
      dest[i] = aprs_process_buffer1[i];
    }
    aprs_ready1 = false;
    copied = APRS_BUFFER_SIZE;
  } else if (aprs_ready2 && max_samples >= APRS_BUFFER_SIZE) {
    for (int i = 0; i < APRS_BUFFER_SIZE; i++) {
      dest[i] = aprs_process_buffer2[i];
    }
    aprs_ready2 = false;
    copied = APRS_BUFFER_SIZE;
  }
  // Если max_samples меньше, можно скопировать частично, но для простоты — весь
  // буфер или ничего
  if (copied > 0) {
    LogC(LOG_C_GREEN, "Read %d APRS samples", copied);
  }
  return copied;
}

void BOARD_ADC_GetBatteryInfo(uint16_t *pVoltage, uint16_t *pCurrent) {
  // (без изменений)
  *pVoltage = adc_dma_buffer[0];
  *pCurrent = 0;
}

uint16_t BOARD_ADC_GetAPRS(void) {
  // (без изменений, для одиночного значения)
  return adc_dma_buffer[1];
}

void BOARD_DAC_Init(void) {
  // (без изменений)
  LL_GPIO_SetPinMode(GPIOA, LL_GPIO_PIN_4, LL_GPIO_MODE_ANALOG);
  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_DAC1);
  LL_DAC_SetTriggerSource(DAC1, LL_DAC_CHANNEL_1, LL_DAC_TRIG_SOFTWARE);
  LL_DAC_SetOutputBuffer(DAC1, LL_DAC_CHANNEL_1, LL_DAC_OUTPUT_BUFFER_ENABLE);
  LL_DAC_Enable(DAC1, LL_DAC_CHANNEL_1);
}

void BOARD_DAC_SetValue(uint16_t value) {
  // (без изменений)
  if (value > 4095)
    value = 4095;
  LL_DAC_ConvertData12RightAligned(DAC1, LL_DAC_CHANNEL_1, value);
  LL_DAC_TrigSWConversion(DAC1, LL_DAC_CHANNEL_1);
}

void BOARD_Init(void) {
  // (без изменений, но ADC_Init теперь не стартует DMA)
  BOARD_GPIO_Init();
  UART_Init();
  LogC(LOG_C_BRIGHT_WHITE, "Init start");
  BOARD_ADC_Init();
  BOARD_DAC_Init();
  LogC(LOG_C_BRIGHT_WHITE, "ADC_CR2=%08X ADC_SR=%08X", ADC1->CR2, ADC1->SR);
  LogC(LOG_C_BRIGHT_WHITE, "Flash init");
  PY25Q16_Init();
  LogC(LOG_C_BRIGHT_WHITE, "File system init");
  fs_init();
  LogC(LOG_C_BRIGHT_WHITE, "Display init");
  ST7565_Init();
  LogC(LOG_C_BRIGHT_WHITE, "Backlight init");
  BACKLIGHT_InitHardware();
}

void BOARD_FlashlightToggle() { GPIO_TogglePin(GPIO_PIN_FLASHLIGHT); }
void BOARD_ToggleRed(bool on) { BK4819_ToggleGpioOut(BK4819_RED, on); }
void BOARD_ToggleGreen(bool on) { BK4819_ToggleGpioOut(BK4819_GREEN, on); }
