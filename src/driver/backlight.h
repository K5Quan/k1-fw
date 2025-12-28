#ifndef DRIVER_BACKLIGHT_H
#define DRIVER_BACKLIGHT_H

#include <stdbool.h>
#include <stdint.h>

extern uint16_t gBacklightCountdown_500ms;
extern uint8_t gBacklightBrightness;

void BACKLIGHT_InitHardware();
void BACKLIGHT_TurnOn();
void BACKLIGHT_TurnOff();
bool BACKLIGHT_IsOn();
void BACKLIGHT_SetBrightness(uint8_t brigtness);
uint8_t BACKLIGHT_GetBrightness(void);
void BACKLIGHT_Init(void);
void BACKLIGHT_UpdateTimer(void);

#endif
