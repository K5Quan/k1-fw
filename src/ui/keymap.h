#ifndef KEYMAP_UI_H
#define KEYMAP_UI_H

#include "../driver/keyboard.h"

extern bool gKeymapActive;

void KEYMAP_Render();
bool KEYMAP_Key(KEY_Code_t key, KEY_State_t state);
void KEYMAP_Show();
void KEYMAP_Hide();

#endif
