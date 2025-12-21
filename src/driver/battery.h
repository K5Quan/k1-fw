#ifndef BATTERY_H
#define BATTERY_H

#include <stdbool.h>
#include <stdint.h>

#define BAT_WARN_PERCENT 5

extern uint16_t gBatteryCalibration[6];
extern uint16_t gBatteryCurrentVoltage;
extern uint16_t gBatteryCurrent;
extern uint16_t gBatteryVoltages[4];
extern uint16_t gBatteryVoltageAverage;
extern uint8_t gBatteryDisplayLevel;
extern uint8_t gBatteryPercent;
extern bool gChargingWithTypeC;
extern bool gLowBatteryBlink;
extern bool gLowBattery;
extern bool gLowBatteryConfirmed;
extern uint16_t gBatteryCheckCounter;

extern volatile uint16_t gPowerSave_10ms;

typedef enum {
  BATTERY_TYPE_1600_MAH,
  BATTERY_TYPE_2200_MAH,
  BATTERY_TYPE_3500_MAH,
  BATTERY_TYPE_1500_MAH,
  BATTERY_TYPE_2500_MAH,
  BATTERY_TYPE_UNKNOWN
} BATTERY_Type_t;

extern const char *BATTERY_TYPE_NAMES[6];
extern const char *BATTERY_STYLE_NAMES[3];

void BATTERY_GetReadings();
uint32_t BATTERY_GetPreciseVoltage(uint16_t cal);
uint16_t BATTERY_GetCal(uint32_t v);

#endif
