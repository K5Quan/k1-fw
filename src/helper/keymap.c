#include "keymap.h"
#include "../apps/apps.h"
#include "../driver/lfs.h"
#include "../external/printf/printf.h"
#include "storage.h"
#include <string.h>

AppKeymap_t gCurrentKeymap;

const char *KA_NAMES[] = {
    [KA_NONE] = "NONE",
    [KA_STEP] = "STEP",
    [KA_BW] = "BW",
    [KA_GAIN] = "GAIN",
    [KA_POWER] = "POWER",
    [KA_BL] = "BL",
    [KA_RSSI] = "RSSI",
    [KA_FLASHLIGHT] = "FLASHLIGHT",
    [KA_MONI] = "MONI",
    [KA_TX] = "TX",
    [KA_VOX] = "VOX",
    [KA_OFFSET] = "OFFSET",
    [KA_BLACKLIST_LAST] = "LAST",
    [KA_WHITELIST_LAST] = "LAST",
    [KA_FASTMENU1] = "FASTMENU1",
    [KA_FASTMENU2] = "FASTMENU2",
    [KA_CH_SETTING] = "SETTING",
    [KA_BANDS] = "BANDS",
    [KA_CHANNELS] = "CHANNELS",
    [KA_LOOTLIST] = "LOOTLIST",
};

static char keymapDir[16];
static char keymapFile[32];

void KEYMAP_Load() {
  snprintf(keymapDir, 16, "/%s", apps[gCurrentApp].name);
  snprintf(keymapFile, 32, "%s/keymap.key", keymapDir);

  struct lfs_info info;
  if (lfs_stat(&gLfs, keymapDir, &info) < 0) {
    lfs_mkdir(&gLfs, keymapDir);
  }
  if (!lfs_file_exists(keymapFile)) {
    STORAGE_INIT(keymapFile, AppKeymap_t, 1);
  }

  STORAGE_LOAD(keymapFile, 1, &gCurrentKeymap);
}

void KEYMAP_Save() { STORAGE_SAVE(keymapFile, 1, &gCurrentKeymap); }
