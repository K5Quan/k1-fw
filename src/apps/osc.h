#pragma once
#include "../driver/keyboard.h"
#include <stdbool.h>

void OSC_init(void);
void OSC_deinit(void);
void OSC_update(void);
void OSC_render(void);
bool OSC_key(KEY_Code_t key, Key_State_t state);
