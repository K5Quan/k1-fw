#include "keymap.h"
#include "../driver/keyboard.h"
#include "../helper/keymap.h"
#include "../helper/menu.h"
#include "graphics.h"

bool gKeymapActive;

static const uint8_t ITEM_H = 19;

bool isSubmenu;
KEY_Code_t currentKey;
AppAction_t *currentAction;

static Menu menu = {
    .itemHeight = ITEM_H,
};
static Menu submenu = {
    .itemHeight = MENU_ITEM_H,
};

static void renderItem(uint16_t index, uint8_t i) {
  uint8_t y = MENU_Y + i * ITEM_H;
  KeyAction clickAction = gCurrentKeymap.click[index].action;
  KeyAction longAction = gCurrentKeymap.long_press[index].action;
  PrintMediumEx(13, y + 8, POS_L, C_INVERT, "%s: %s", KEY_NAMES[index],
                KA_NAMES[clickAction]);
  PrintMediumEx(13, y + 8 + 8, POS_L, C_INVERT, "%s L: %s", KEY_NAMES[index],
                KA_NAMES[longAction]);
}

static void renderSubItem(uint16_t index, uint8_t i) {
  uint8_t y = MENU_Y + i * MENU_ITEM_H;
  PrintMediumEx(13, y + 8, POS_L, C_INVERT, "%s", KA_NAMES[index]);
}

static bool action(const uint16_t index, KEY_Code_t key, Key_State_t state) {
  if (state == KEY_PRESSED) {
    return false;
  }
  if (key == KEY_STAR) {
    return true;
  }
  if (key == KEY_UP || key == KEY_DOWN) {
    return false;
  }
  if (state == KEY_RELEASED) {
    if (key == KEY_EXIT) {
      if (isSubmenu) {
        MENU_Deinit();
        MENU_Init(&menu);
        return true;
      }
      KEYMAP_Hide();
      return true;
    }
  }

  if (key == KEY_MENU) {
    isSubmenu = true;
    currentKey = index;
    if (state == KEY_LONG_PRESSED) {
      currentAction = &gCurrentKeymap.long_press[index];
    } else {
      currentAction = &gCurrentKeymap.click[index];
    }
    MENU_Init(&submenu);
    submenu.i = currentAction->action;
    return true;
  }

  menu.i = key;

  return true;
}
static bool subAction(const uint16_t index, KEY_Code_t key, Key_State_t state) {
  if (state != KEY_RELEASED) {
    return false;
  }
  if (key == KEY_UP) {
    return false;
  }
  if (key == KEY_EXIT) {
    MENU_Deinit();
    isSubmenu = false;
    MENU_Init(&menu);
    return true;
  }
  if (key == KEY_MENU) {
    currentAction->action = index;
    MENU_Deinit();
    isSubmenu = false;
    MENU_Init(&menu);
    return true;
  }

  menu.i = key;

  return true;
}

void KEYMAP_Render() { MENU_Render(); }

bool KEYMAP_Key(KEY_Code_t key, KEY_State_t state) {
  return MENU_HandleInput(key, state);
}

void KEYMAP_Show() {
  menu.num_items = KEY_COUNT;
  menu.render_item = renderItem;
  menu.action = action;

  submenu.num_items = KA_COUNT;
  submenu.render_item = renderSubItem;
  submenu.action = subAction;

  gKeymapActive = true;
  MENU_Init(&menu);
}

void KEYMAP_Hide() {
  KEYMAP_Save();
  gKeymapActive = false;
  MENU_Deinit();
}
