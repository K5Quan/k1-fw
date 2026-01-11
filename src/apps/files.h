#ifndef FILES_APP_H
#define FILES_APP_H

#include "../driver/keyboard.h"
#include <stdbool.h>
#include <stdint.h>

void FILES_init();
void FILES_update();
bool FILES_keyEx(KEY_Code_t key, Key_State_t state, bool isProMode);
bool FILES_key(KEY_Code_t key, Key_State_t state);
void FILES_render();

#endif // !FILES_APP_H
