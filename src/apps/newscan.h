#ifndef NEWSCAN_H
#define NEWSCAN_H

#include "../driver/keyboard.h"
#include <stdbool.h>
#include <stdint.h>

bool NEWSCAN_key(KEY_Code_t Key, Key_State_t state);
void NEWSCAN_init(void);
void NEWSCAN_deinit(void);
void NEWSCAN_update(void);
void NEWSCAN_render(void);

#endif /* end of include guard: NEWSCAN_H */
