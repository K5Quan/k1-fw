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

  uint32_t lastTimeOpen; // TODO: last seen minutes, ++ every min for each
  uint16_t duration;
  uint8_t code;
  uint32_t f : 27;
  bool isCd : 1;
  bool open : 1;
  bool blacklist : 1; // move to flash or freq list
  bool whitelist : 1;
} __attribute__((packed)) Loot;

#endif // !LOOT_H
