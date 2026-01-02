#include "files.h"
#include "../driver/fat.h"
#include "../external/printf/printf.h"
#include "../helper/menu.h"
#include "../ui/graphics.h"
#include <string.h>

file_info_t file_list[MAX_FILES];
uint8_t filesCount;

// Имя N-го файла - читаем сектор напрямую
bool fileName(uint8_t index, char *name_out) {
  if (index >= filesCount)
    return false;

  strcpy(name_out, file_list[index].name);

  return true;
}

static void renderItem(uint16_t index, uint8_t i) {
  uint8_t y = MENU_Y + i * MENU_ITEM_H;
  char name[13];

  if (fileName(index, name)) {
    PrintMediumEx(13, y + 8, POS_L, C_INVERT, "%u", i, name);
  }
}

static bool action(const uint16_t index, KEY_Code_t key, Key_State_t state) {
  return false;
}

static Menu filesMenu = {
    .render_item = renderItem,
    .itemHeight = MENU_ITEM_H,
    .action = action,
};

void FILES_init() {
  filesCount = usb_fs_list_files(file_list, MAX_FILES);
  filesMenu.num_items = filesCount;
  MENU_Init(&filesMenu);
}

void FILES_update() {}

bool FILES_key(KEY_Code_t key, Key_State_t state) {
  return MENU_HandleInput(key, state);
}

void FILES_render() {}
