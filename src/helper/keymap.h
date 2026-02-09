#ifndef KEYMAP_H
#define KEYMAP_H

#include "../driver/keyboard.h"
#include <stdint.h>

typedef enum {
  KA_NONE,
  KA_STEP,
  KA_BW,
  KA_GAIN,
  KA_POWER,
  KA_BL,
  KA_RSSI,
  KA_FLASHLIGHT,
  KA_MONI,
  KA_TX,
  KA_VOX,
  KA_OFFSET,
  KA_BLACKLIST_LAST,
  KA_WHITELIST_LAST,
  KA_FASTMENU1,
  KA_FASTMENU2,
  KA_CH_SETTING,
  KA_BANDS,
  KA_CHANNELS,
  KA_LOOTLIST,

  KA_COUNT,
} KeyAction;

typedef struct {
  KeyAction action;
  uint8_t param; // Параметр действия (например, ID приложения)
} AppAction_t;

typedef struct {
  AppAction_t click[KEY_COUNT]; // Действия на клик (KEY_RELEASED)
  AppAction_t long_press[KEY_COUNT]; // Действия на удержание (KEY_LONG_PRESSED)
} AppKeymap_t;

void KEYMAP_Load();
void KEYMAP_Save();

extern AppKeymap_t gCurrentKeymap;
extern const char *KA_NAMES[];

#endif // !KEYMAP_H
