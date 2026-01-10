#ifndef CH_HELPER_H
#define CH_HELPER_H

#include "../inc/channel.h"

bool CH_Init(const char *name);
bool CH_Load(const char *name, uint16_t num, CH *ch);
bool CH_Save(const char *name, uint16_t num, CH *ch);

#endif // !CH_HELPER_H
