#ifndef OOK_H
#define OOK_H

#include <stdbool.h>
#include <stdint.h>

void ook_reset(void);
void ook_sink(const uint16_t *buf, uint32_t n);
extern void (*ookHandler)(const uint8_t *data, uint16_t nbytes);

#endif
