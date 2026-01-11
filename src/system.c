#include "system.h"
#include "apps/apps.h"
#include "board.h"
#include "driver/backlight.h"
#include "driver/battery.h"
#include "driver/bk4829.h"
#include "driver/eeprom.h"
#include "driver/keyboard.h"
#include "driver/lfs.h"
#include "driver/st7565.h"
#include "driver/systick.h"
#include "driver/uart.h"
#include "external/CMSIS/Device/PY32F071/Include/py32f071xB.h"
#include "external/littlefs/lfs.h"
#include "external/printf/printf.h"
#include "helper/menu.h"
#include "helper/scan.h"
#include "helper/storage.h"
#include "inc/channel.h"
#include "settings.h"
#include "ui/finput.h"
#include "ui/graphics.h"
#include "ui/statusline.h"
#include "ui/textinput.h"
#include <string.h>

#define queueLen 20
#define itemSize sizeof(SystemMessages)

static uint8_t DEAD_BUF[] = {0xDE, 0xAD};

static char notificationMessage[16] = "";
static uint32_t notificationTimeoutAt;

static uint32_t secondTimer;
static uint32_t radioTimer;
static uint32_t appsKeyboardTimer;

static uint32_t lastUartDataTime;

static bool isUartWaiting() {
  return lastUartDataTime && Now() - lastUartDataTime < 5000;
}

static void appRender() {
  if (!gRedrawScreen) {
    return;
  }

  if (Now() - gLastRender < 40) {
    return;
  }

  gRedrawScreen = false;

  UI_ClearScreen();

  APPS_render();
  if (gFInputActive) {
    FINPUT_render();
  }
  if (gTextInputActive) {
    TEXTINPUT_render();
  }

  if (notificationMessage[0]) {
    FillRect(0, 32 - 5, 128, 9, C_FILL);
    PrintMediumEx(64, 32 + 2, POS_C, C_CLEAR, notificationMessage);
  }

  STATUSLINE_render(); // coz of APPS_render calls STATUSLINE_SetText

  ST7565_Blit();
  gLastRender = Now();
}

static void systemUpdate() {
  BATTERY_UpdateBatteryInfo();
  // BACKLIGHT_Update();
}

static bool resetNeeded() {
  uint8_t buf[2];
  EEPROM_ReadBuffer(0, buf, 2);

  return memcmp(buf, DEAD_BUF, 2) == 0;
}

static void reset() {
  UI_ClearScreen();
  PrintMediumEx(LCD_XCENTER, LCD_YCENTER, POS_C, C_FILL, "Formatting...");

  lfs_format(&gLfs, &gStorage.config);

  STORAGE_INIT("SETTINGS.SET", Settings, 1);
  STORAGE_SAVE("SETTINGS.SET", 0, &gSettings);

  PrintMediumEx(LCD_XCENTER, LCD_YCENTER, POS_C, C_FILL, "Release key 0!");
  keyboard_tick_1ms();
  while (keyboard_is_pressed(KEY_0)) {
    SYSTICK_DelayMs(1);
    keyboard_tick_1ms();
  }
  NVIC_SystemReset();
}

static void loadSettingsOrReset() {
  if (!lfs_file_exists("SETTINGS.SET")) {
    reset();
  }
  STORAGE_LOAD("SETTINGS.SET", 0, &gSettings);
}

static bool checkKeylock(KEY_State_t state, KEY_Code_t key) {
  bool isKeyLocked = gSettings.keylock;
  bool isPttLocked = gSettings.pttLock;
  bool isSpecialKey = key == KEY_PTT || key == KEY_SIDE1 || key == KEY_SIDE2;
  bool isLongPressF = state == KEY_LONG_PRESSED && key == KEY_F;

  if (isLongPressF) {
    gSettings.keylock = !gSettings.keylock;
    SETTINGS_Save();
    return true;
  }

  /* if (gSettings.keylock && state == KEY_LONG_PRESSED && key == KEY_8) {
    captureScreen();
    return true;
  } */

  return isKeyLocked && (isPttLocked || !isSpecialKey) && !isLongPressF;
}

static void onKey(KEY_Code_t key, KEY_State_t state) {
  // if (!isUartWaiting()) {
  BACKLIGHT_TurnOn();

  if (checkKeylock(state, key)) {
    gRedrawScreen = true;
    return;
  }

  if (gFInputActive && FINPUT_key(key, state)) {
    gRedrawScreen = true;
    gLastRender = 0;
  } else if (gTextInputActive && TEXTINPUT_key(key, state)) {
    gRedrawScreen = true;
    gLastRender = 0;
  } else if (APPS_key(key, state) || (MENU_IsActive() && key != KEY_EXIT)) {
    LogC(LOG_C_BRIGHT_WHITE, "[SYS] Apps key %u %u", key, state);
    gRedrawScreen = true;
    gLastRender = 0;
  } else {
    LogC(LOG_C_BRIGHT_WHITE, "[SYS] Global key %u %u", key, state);
    if (key == KEY_MENU) {
      if (state == KEY_LONG_PRESSED) {
        APPS_run(APP_SETTINGS);
      } else if (state == KEY_RELEASED) {
        APPS_run(APP_APPS_LIST);
      }
    }
    if (key == KEY_EXIT) {
      if (state == KEY_RELEASED) {
        APPS_exit();
      }
    }
  }
  // }
}

void SYS_Main() {
  LogC(LOG_C_BRIGHT_WHITE, "Keyboard init");
  keyboard_init(onKey);

  keyboard_tick_1ms();
  if (keyboard_is_pressed(KEY_0)) {
    reset();
  } else {
    loadSettingsOrReset();

    LogC(LOG_C_BRIGHT_WHITE, "Bat init");
    BATTERY_UpdateBatteryInfo();

    // better UX
    STATUSLINE_render();
    ST7565_Blit();

    LogC(LOG_C_BRIGHT_WHITE, "Load bands");
    // BANDS_Load();

    LogC(LOG_C_BRIGHT_WHITE, "Run default app: %s",
         apps[gSettings.mainApp].name);
    APPS_run(gSettings.mainApp);
  }

  /* LogC(LOG_C_BRIGHT_WHITE, "USB MSC init");
  BOARD_USBInit(); */

  LogC(LOG_C_BRIGHT_WHITE, "System initialized");

  for (;;) {
    SETTINGS_UpdateSave();

    SCAN_Check();

    if (gFInputActive) {
      FINPUT_update();
    }
    if (gTextInputActive) {
      TEXTINPUT_update();
    }
    APPS_update();
    if (Now() - appsKeyboardTimer >= 1) {
      keyboard_tick_1ms();
      appsKeyboardTimer = Now();
    }

    // common: render 2 times per second minimum
    if (Now() - gLastRender >= 500) {
      BACKLIGHT_UpdateTimer();
      gRedrawScreen = true;
    }

    if (Now() - secondTimer >= 1000) {
      STATUSLINE_update();
      systemUpdate();
      secondTimer = Now();
    }

    appRender();

    /* while (gCurrentApp != APP_SCANER && UART_IsCommandAvailable()) {
      UART_HandleCommand();
      lastUartDataTime = Now();
    } */

    __WFI();
  }
}
