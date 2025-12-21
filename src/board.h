#ifndef BOARD_H
#define BOARD_H

#include <stdbool.h>
#include <stdint.h>

void BOARD_FLASH_Init(void);
void BOARD_GPIO_Init(void);
void BOARD_ADC_Init(void);
void BOARD_ADC_GetBatteryInfo(uint16_t *pVoltage, uint16_t *pCurrent);
void BOARD_Init(void);
void BOARD_FlashlightToggle();
void BOARD_ToggleRed(bool on);
void BOARD_ToggleGreen(bool on);

#endif
