#include "battery.h"
#include "../helper/measurements.h"
#include "../misc.h"
#include "../settings.h"

uint16_t gBatteryCalibration[6];
uint16_t gBatteryCurrentVoltage;
uint16_t gBatteryCurrent;
uint16_t gBatteryVoltages[4];
uint16_t gBatteryVoltageAverage;
uint8_t gBatteryDisplayLevel;
uint8_t gBatteryPercent;
bool gChargingWithTypeC;
bool gLowBatteryBlink;
bool gLowBattery;
bool gLowBatteryConfirmed;
uint16_t gBatteryCheckCounter;

typedef enum {
  BATTERY_LOW_INACTIVE,
  BATTERY_LOW_ACTIVE,
  BATTERY_LOW_CONFIRMED
} BatteryLow_t;

const char *BATTERY_TYPE_NAMES[6] = {
    [BATTERY_TYPE_1600_MAH] = "1600_MAH", //
    [BATTERY_TYPE_2200_MAH] = "2200_MAH", //
    [BATTERY_TYPE_3500_MAH] = "3500_MAH", //
    [BATTERY_TYPE_1500_MAH] = "1500_MAH", //
    [BATTERY_TYPE_2500_MAH] = "2500_MAH", //
    [BATTERY_TYPE_UNKNOWN] = "UNKNOWN",   //
};
const char *BATTERY_STYLE_NAMES[3] = {"Icon", "%", "V"};

uint16_t lowBatteryCountdown;
const uint16_t lowBatteryPeriod = 30;

volatile uint16_t gPowerSave_10ms;

const uint16_t Voltage2PercentageTable[][7][2] = {
    [BATTERY_TYPE_1600_MAH] =
        {
            {828, 100},
            {814, 97},
            {760, 25},
            {729, 6},
            {630, 0},
            {0, 0},
            {0, 0},
        },

    [BATTERY_TYPE_2200_MAH] =
        {
            {832, 100},
            {813, 95},
            {740, 60},
            {707, 21},
            {682, 5},
            {630, 0},
            {0, 0},
        },

    [BATTERY_TYPE_3500_MAH] =
        {
            {837, 100},
            {826, 95},
            {750, 50},
            {700, 25},
            {620, 5},
            {600, 0},
            {0, 0},
        },

    // Estimated discharge curve for 1500 mAh K1 battery (improve this)
    [BATTERY_TYPE_1500_MAH] =
        {
            {828, 100}, // Fully charged (measured ~8.28V)
            {813, 97},  // Top end
            {758, 25},  // Mid level
            {726, 6},   // Almost empty
            {630, 0},   // Fully discharged (conservative)
            {0, 0},
            {0, 0},
        },

    // Estimated discharge curve for 2500 mAh K1 battery (improve this)
    [BATTERY_TYPE_2500_MAH] =
        {
            {839, 100}, // Fully charged (measured ~8.39V)
            {818, 95},  // Top end (slightly raised vs 816)
            {745, 55},  // Mid range
            {703, 25},  // Low level
            {668, 5},   // Almost empty
            {623, 0},   // Fully discharged (between 630 and 600)
            {0, 0},
        },
};

unsigned int BATTERY_VoltsToPercent(const unsigned int voltage_10mV) {
  BATTERY_Type_t BATTERY_TYPE = BATTERY_TYPE_1500_MAH;
  const uint16_t(*crv)[2] = Voltage2PercentageTable[BATTERY_TYPE];
  const int mulipl = 1000;
  for (unsigned int i = 1;
       i < ARRAY_SIZE(Voltage2PercentageTable[BATTERY_TYPE_2200_MAH]); i++) {
    if (voltage_10mV > crv[i][0]) {
      const int a =
          (crv[i - 1][1] - crv[i][1]) * mulipl / (crv[i - 1][0] - crv[i][0]);
      const int b = crv[i][1] - a * crv[i][0] / mulipl;
      const int p = a * voltage_10mV / mulipl + b;
      return Clamp(p, 0, 100);
    }
  }

  return 0;
}

void BATTERY_GetReadings() {
  BATTERY_Type_t BATTERY_TYPE = BATTERY_TYPE_1500_MAH;
  const uint8_t PreviousBatteryLevel = gBatteryDisplayLevel;
  const uint16_t Voltage = (gBatteryVoltages[0] + gBatteryVoltages[1] +
                            gBatteryVoltages[2] + gBatteryVoltages[3]) /
                           4;

  gBatteryVoltageAverage = (Voltage * 760) / gBatteryCalibration[3];

  if (gBatteryVoltageAverage > 890)
    gBatteryDisplayLevel = 7; // battery overvoltage
  else if (gBatteryVoltageAverage < 630 &&
           (BATTERY_TYPE == BATTERY_TYPE_1600_MAH ||
            BATTERY_TYPE == BATTERY_TYPE_2200_MAH))
    gBatteryDisplayLevel = 0; // battery critical
  else if (gBatteryVoltageAverage < 600 &&
           (BATTERY_TYPE == BATTERY_TYPE_3500_MAH))
    gBatteryDisplayLevel = 0; // battery critical
  else {
    gBatteryDisplayLevel = 1;
    const uint8_t levels[] = {5, 17, 41, 65, 88};
    gBatteryPercent = BATTERY_VoltsToPercent(gBatteryVoltageAverage);

    for (uint8_t i = 6; i >= 2; i--) {
      if (gBatteryPercent > levels[i - 2]) {
        gBatteryDisplayLevel = i;
        break;
      }
    }
  }

  gChargingWithTypeC = gBatteryCurrent > 500;

  if (PreviousBatteryLevel != gBatteryDisplayLevel) {
    if (gBatteryDisplayLevel > 2)
      gLowBatteryConfirmed = false;
    else if (gBatteryDisplayLevel < 2) {
      gLowBattery = true;
    } else {
      gLowBattery = false;
    }

    lowBatteryCountdown = 0;
  }
}

uint32_t BATTERY_GetPreciseVoltage(uint16_t cal) {
  return gBatteryVoltageAverage * 76000 / cal;
}

uint16_t BATTERY_GetCal(uint32_t v) {
  return (uint32_t)gSettings.batteryCalibration *
         BATTERY_GetPreciseVoltage(gSettings.batteryCalibration) / v;
}
