#ifndef FINPUT_H
#define FINPUT_H

#include "../driver/keyboard.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum {
  UNIT_RAW,     // Raw value (e.g., 255)
  UNIT_HZ,      // Hertz (x10, e.g., 10 = 1.0Hz)
  UNIT_KHZ,     // Kilohertz (x10, e.g., 1234 = 123.4kHz)
  UNIT_MHZ,     // Megahertz (x10, e.g., 1450 = 145.0MHz)
  UNIT_VOLTS,   // Volts (x100, e.g., 824 = 8.24V)
  UNIT_DBM,     // dBm (e.g., 10 = 10dBm)
  UNIT_PERCENT, // Percent (e.g., 50 = 50%)
  UNIT_MS,
} InputUnit;

typedef void(FInputCallback)(uint32_t value1, uint32_t value2);

void FINPUT_init();
bool FINPUT_key(KEY_Code_t key, Key_State_t state);
void FINPUT_update();
void FINPUT_render();
void FINPUT_deinit();
void FINPUT_setup(uint32_t min, uint32_t max, InputUnit unit, bool is_range);
void FINPUT_Show(FInputCallback cb);

extern bool gFInputActive;
extern uint32_t gFInputValue1;
extern uint32_t gFInputValue2;
extern FInputCallback *gFInputCallback;

#endif /* end of include guard: FINPUT_H */
