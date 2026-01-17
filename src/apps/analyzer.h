#ifndef ANALYZER_H
#define ANALYZER_H

#include "../driver/keyboard.h"
#include <stdbool.h>

void ANALYZER_init(void);
void ANALYZER_update(void);
void ANALYZER_render(void);
void ANALYZER_deinit(void);
bool ANALYZER_key(KEY_Code_t key, Key_State_t state);

#endif // ANALYZER_H
