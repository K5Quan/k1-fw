#ifndef LOOT_H
#define LOOT_H

#include <stdint.h>

typedef struct {
  uint32_t lastTimeOpen; // TODO: last seen minutes, ++ every min for each
  uint16_t duration;
  uint8_t cd; // TODO: make single field with binary ct/cd
  uint8_t ct;
  uint32_t f : 27;
  bool open : 1;
  bool blacklist : 1; // move to flash or freq list
  bool whitelist : 1;
} __attribute__((packed)) Loot;

#endif // !LOOT_H
