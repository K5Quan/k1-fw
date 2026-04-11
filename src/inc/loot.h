#ifndef LOOT_H
#define LOOT_H

#include "channel.h"
#include <stdint.h>

typedef struct {
  uint8_t modulation : 4;
  uint8_t bw : 4;
  Radio radio : 2;
  Squelch squelch;
  uint8_t gainIndex : 5;

  uint16_t lastTimeOpen; // minutes since boot (65535 min = 45 days)
  uint16_t duration;
  uint8_t code;
  uint32_t f : 27;
  bool isCd : 1;
  bool open : 1;
  bool blacklist : 1;
  bool whitelist : 1;
} __attribute__((packed)) Loot;

#endif // !LOOT_H
