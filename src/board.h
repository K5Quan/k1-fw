#ifndef BOARD_H
#define BOARD_H

#include <stdbool.h>
#include <stdint.h>

#define APRS_BUFFER_SIZE 128

// Существующие функции
void BOARD_Init(void);
void BOARD_GPIO_Init(void);
void BOARD_ADC_Init(void);
void BOARD_DAC_Init(void);
void BOARD_ADC_GetBatteryInfo(uint16_t *pVoltage, uint16_t *pCurrent);
uint16_t BOARD_ADC_GetAPRS(void);
void BOARD_FlashlightToggle(void);
void BOARD_ToggleRed(bool on);
void BOARD_ToggleGreen(bool on);

// Функции для DMA-режима APRS
void BOARD_DMA_Init(void);
void BOARD_ADC_StartAPRS_DMA(void);
void BOARD_ADC_StopAPRS_DMA(void);
uint32_t BOARD_ADC_ReadAPRS_DMA(uint16_t *dest, uint32_t max_samples);
uint32_t BOARD_ADC_GetAvailableAPRS_DMA(void);

// Кольцевой DMA-буфер: [half-A: 0..APRS_BUFFER_SIZE-1 | half-B: APRS_BUFFER_SIZE..2*APRS_BUFFER_SIZE-1]
// half-A стабильна при aprs_ready1==true, half-B — при aprs_ready2==true.
extern volatile uint16_t adc_dma_buffer[2 * APRS_BUFFER_SIZE];

extern volatile bool aprs_ready1;
extern volatile bool aprs_ready2;

#endif // BOARD_H
