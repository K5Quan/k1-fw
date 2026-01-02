#ifndef FILES_H
#define FILES_H

#include "../driver/keyboard.h"

void FILES_init();
void FILES_update();
bool FILES_key(KEY_Code_t key, Key_State_t state);
void FILES_render();

#endif // !FILES_H
