// cmdscan.h
#ifndef CMDSCAN_H
#define CMDSCAN_H

#include "../driver/keyboard.h"
#include <stdbool.h>
#include <stdint.h>

void CMDSCAN_init(void);
void CMDSCAN_deinit(void);
void CMDSCAN_update(void);
void CMDSCAN_render(void);
bool CMDSCAN_key(KEY_Code_t key, Key_State_t state);

#endif // CMDSCAN_H
