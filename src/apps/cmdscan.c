// cmdscan.c
#include "cmdscan.h"

#include "../driver/systick.h"
#include "../driver/uart.h"
#include "../helper/bands.h"
#include "../helper/lootlist.h"
#include "../helper/regs-menu.h"
#include "../helper/scan.h"
#include "../helper/scancommand.h"
#include "../radio.h"
#include "../ui/components.h"
#include "../ui/graphics.h"
#include "../ui/statusline.h"
#include "apps.h"

// =============================
// Состояние приложения
// =============================
typedef struct {
  bool isActive;      // Приложение активно
  bool showInfo;      // Показать информацию о команде
  uint8_t profileNum; // Текущий профиль (1-4)
  uint16_t cmdIndex;  // Индекс текущей команды
  uint32_t execCount; // Счетчик выполненных команд
  char filename[32];  // Имя текущего файла
} CmdScanState;

static CmdScanState cmdState = {.isActive = false,
                                .showInfo = true,
                                .profileNum = 1,
                                .cmdIndex = 0,
                                .execCount = 0,
                                .filename = "/scans/cmd1.bin"};

// =============================
// Вспомогательные функции
// =============================

// Загрузить профиль по номеру
static void LoadProfile(uint8_t num) {
  snprintf(cmdState.filename, sizeof(cmdState.filename), "/scans/cmd%d.bin",
           num);

  // Закрываем предыдущий файл если был открыт
  if (SCAN_IsCommandMode()) {
    SCAN_SetCommandMode(false);
  }

  // Загружаем новый файл через SCAN API
  SCAN_LoadCommandFile(cmdState.filename);

  if (SCAN_IsCommandMode()) {
    cmdState.profileNum = num;
    cmdState.cmdIndex = 0;
    cmdState.execCount = 0;
    Log("[CMDSCAN] Loaded profile %d: %s", num, cmdState.filename);
  } else {
    Log("[CMDSCAN] Failed to load %s", cmdState.filename);
    // Пробуем загрузить по умолчанию
    if (num != 1) {
      LoadProfile(1);
    }
  }
}

// Отобразить информацию о текущей команде
static void RenderCommandInfo(void) {
  if (!cmdState.showInfo)
    return;

  // Используем API из scan.c для получения текущей команды
  SCMD_Command *cmd = SCAN_GetCurrentCommand();
  if (!cmd)
    return;

  // Координаты для отображения
  int y = 30;

  // Тип команды
  const char *typeNames[] = {"CH", "RG", "JU", "CJ", "PA", "BO", "MK", "CL",
                             "RT", "SP", "SM", "PW", "GN", "RC", "SR", "TR"};

  PrintSmallEx(2, y, POS_L, C_FILL, "CMD: %s P:%d", typeNames[cmd->type % 16],
               cmd->priority);
  y += 8;

  // Параметры в зависимости от типа
  switch (cmd->type) {
  case SCMD_CHANNEL:
    PrintSmallEx(2, y, POS_L, C_FILL, "F: %.3f MHz", cmd->start / 1000000.0f);
    y += 8;
    PrintSmallEx(2, y, POS_L, C_FILL, "Dwell: %dms", cmd->dwell_ms);
    break;

  case SCMD_RANGE:
    PrintSmallEx(2, y, POS_L, C_FILL, "R: %.3f-%.3f", cmd->start / 1000000.0f,
                 cmd->end / 1000000.0f);
    y += 8;
    PrintSmallEx(2, y, POS_L, C_FILL, "Step: %dms", cmd->dwell_ms);
    break;

  case SCMD_PAUSE:
    PrintSmallEx(2, y, POS_L, C_FILL, "Pause: %dms", cmd->dwell_ms);
    break;

  case SCMD_JUMP:
  case SCMD_CJUMP:
    PrintSmallEx(2, y, POS_L, C_FILL, "Jump to: %d", cmd->goto_offset);
    y += 8;
    // PrintSmallEx(2, y, POS_L, C_FILL, "Loops: %d", cmd->loop_count);
    break;
  }

  // Флаги
  if (cmd->flags) {
    y += 8;
    PrintSmallEx(2, y, POS_L, C_FILL, "Flags: 0x%02X", cmd->flags);
  }

  // Статистика
  y = LCD_HEIGHT - 20;
  PrintSmallEx(2, y, POS_L, C_FILL, "Profile: %d", cmdState.profileNum);

  // Получаем реальный индекс команды
  cmdState.cmdIndex = SCAN_GetCommandIndex();
  PrintSmallEx(LCD_WIDTH - 2, y, POS_R, C_FILL, "#%d", cmdState.cmdIndex);

  y += 8;
  PrintSmallEx(2, y, POS_L, C_FILL, "Exec: %lu", cmdState.execCount);

  // Индикатор паузы
  if (SCAN_IsCommandMode()) {
    /* if (SCAN_IsCommandPaused()) {
      PrintMediumBoldEx(LCD_XCENTER, LCD_HEIGHT - 30, POS_C, C_INVERT,
                        "PAUSED");
    } */
  }
}

// =============================
// API функций приложения
// =============================

void CMDSCAN_init(void) {
  // Переключаемся в режим VFO для командного сканирования
  SCAN_SetMode(SCAN_MODE_SINGLE);

  // Загружаем первый профиль через SCAN API
  LoadProfile(cmdState.profileNum);

  cmdState.isActive = true;
  cmdState.execCount = 0;

  Log("[CMDSCAN] Initialized");
}

void CMDSCAN_deinit(void) {
  // Возвращаем обычный режим через SCAN API
  SCAN_SetCommandMode(false);
  cmdState.isActive = false;

  Log("[CMDSCAN] Deinitialized");
}

// В cmdscan.c, функция CMDSCAN_update()
void CMDSCAN_update(void) {
  if (!cmdState.isActive)
    return;

  // Выполняем команды каждые 50мс (вместо 100мс для более плавного отображения)
  static uint32_t lastUpdate = 0;
  uint32_t now = Now();

  if (now - lastUpdate < 50) {
    return;
  }
  lastUpdate = now;

  // ОСНОВНАЯ ЛОГИКА ВЫПОЛНЕНИЯ КОМАНД В scan.c
  // Мы просто даем системе время на выполнение

  // Обновляем статистику
  static uint32_t lastStatUpdate = 0;
  if (now - lastStatUpdate > 1000) {
    // Обновляем счетчик выполненных команд
    SCMD_Command *cmd = SCAN_GetCurrentCommand();
    if (cmd) {
      cmdState.cmdIndex =
          SCAN_GetCommandIndex(); // Получаем индекс из контекста
    }
    lastStatUpdate = now;
  }
}

bool CMDSCAN_key(KEY_Code_t key, Key_State_t state) {
  if (state == KEY_RELEASED) {
    switch (key) {
    // Цифры 1-4: выбор профиля
    case KEY_1:
      LoadProfile(1);
      return true;
    case KEY_2:
      LoadProfile(2);
      return true;
    case KEY_3:
      LoadProfile(3);
      return true;
    case KEY_4:
      LoadProfile(4);
      return true;
    case KEY_9:
      // Принудительный переход к следующей команде
      SCAN_CommandForceNext();
      return true;

    // Управление выполнением
    case KEY_UP:
      // Шаг вперед - переходим к следующей команде
      if (SCAN_IsCommandMode()) {
        SCAN_CommandForceNext();
        cmdState.execCount++;
      }
      return true;

      /* case KEY_DOWN:
        // Перемотка в начало через SCAN API
        SCAN_CommandRewind();
        cmdState.cmdIndex = 0;
        cmdState.execCount = 0;
        return true; */

    case KEY_SIDE1:
      // Пауза/продолжить - нужно добавить API в scan.c
      // Пока просто переключаем командный режим
      if (SCAN_IsCommandMode()) {
        // Будем считать, что закрытие файла = пауза
        SCAN_SetCommandMode(false);
      } else {
        // Восстанавливаем из текущего профиля
        LoadProfile(cmdState.profileNum);
      }
      return true;

    case KEY_SIDE2:
      // Показать/скрыть информацию
      cmdState.showInfo = !cmdState.showInfo;
      return true;

    case KEY_STAR:
      // Быстрая перезагрузка текущего профиля
      LoadProfile(cmdState.profileNum);
      return true;

    case KEY_EXIT:
      // Выход из приложения
      APPS_exit();
      return true;

    case KEY_PTT:
      // Выход в VFO режим
      APPS_run(APP_VFO1);
      return true;
    }
  }

  // Долгое нажатие
  if (state == KEY_LONG_PRESSED) {
    switch (key) {
    case KEY_STAR:
      // Создать тестовый файл (если есть место)
      SCMD_CreateExampleScan();
      Log("[CMDSCAN] Created example file");
      // Перезагружаем текущий профиль
      LoadProfile(cmdState.profileNum);
      return true;

      /* case KEY_EXIT:
        // Полный сброс
        SCAN_CommandRewind();
        cmdState.execCount = 0;
        cmdState.cmdIndex = 0;
        return true; */
    }
  }

  return false;
}

void CMDSCAN_render(void) {
  // Статусная строка
  STATUSLINE_RenderRadioSettings();

  // Заголовок
  PrintSmallEx(LCD_XCENTER, 14, POS_C, C_FILL, "CMD SCAN P%d",
               cmdState.profileNum);

  // Отображаем имя файла (укороченное)
  const char *filename = strrchr(cmdState.filename, '/');
  if (filename)
    filename++;
  else
    filename = cmdState.filename;

  PrintSmallEx(LCD_XCENTER, 24, POS_C, C_FILL, "%s", filename);

  if (vfo) {
    char freqBuf[16];
    mhzToS(freqBuf, vfo->msm.f);
    PrintMediumBoldEx(LCD_XCENTER, 40, POS_C, C_FILL, "%s", freqBuf);

    UI_RSSIBar(64 - 8);

    // Текст RSSI
    PrintSmallEx(20, 70, POS_L, C_FILL, "RSSI: %u", vfo->msm.rssi);
    PrintSmallEx(LCD_WIDTH - 20, 70, POS_R, C_FILL,
                 vfo->msm.open ? "SIGNAL" : "NOISE");
    // Индикатор открытия squelch
    if (vfo->is_open) {
      FillRect(LCD_WIDTH - 30, 20, 20, 20, C_INVERT);
      PrintSmallEx(LCD_WIDTH - 20, 30, POS_C, C_INVERT, "SQL");
    }
  }

  // Информация о текущей команде
  SCMD_Command *cmd = SCAN_GetCurrentCommand();
  if (cmd && cmdState.showInfo) {
    int y = 85;

    // Тип команды
    const char *typeNames[] = {"CH", "RG", "JU", "CJ", "PA", "BO", "MK", "CL",
                               "RT", "SP", "SM", "PW", "GN", "RC", "SR", "TR"};
    uint8_t type_idx = cmd->type % 16;

    PrintSmallEx(2, y, POS_L, C_FILL, "CMD: %s", typeNames[type_idx]);
    PrintSmallEx(LCD_WIDTH - 2, y, POS_R, C_FILL, "PRIO: %d", cmd->priority);
    y += 10;

    // Параметры в зависимости от типа
    switch (cmd->type) {
    case SCMD_CHANNEL:
      if (cmd->start > 0) {
        PrintSmallEx(2, y, POS_L, C_FILL, "Freq: %.3f MHz",
                     cmd->start / 1000000.0f);
        y += 10;
      }
      if (cmd->dwell_ms > 0) {
        PrintSmallEx(2, y, POS_L, C_FILL, "Time: %d ms", cmd->dwell_ms);
      }
      break;

    case SCMD_RANGE:
      PrintSmallEx(2, y, POS_L, C_FILL, "Range: %.3f-%.3f",
                   cmd->start / 1000000.0f, cmd->end / 1000000.0f);
      y += 10;
      if (cmd->dwell_ms > 0) {
        PrintSmallEx(2, y, POS_L, C_FILL, "Dwell: %d ms/pt", cmd->dwell_ms);
      }
      break;

    case SCMD_PAUSE:
      PrintSmallEx(2, y, POS_L, C_FILL, "Pause: %d ms", cmd->dwell_ms);
      break;
    }

    // Индекс команды
    y = LCD_HEIGHT - 30;
    PrintSmallEx(2, y, POS_L, C_FILL, "Cmd: %d/%d", cmdState.cmdIndex,
                 SCAN_GetCommandCount());
    PrintSmallEx(LCD_WIDTH - 2, y, POS_R, C_FILL, "Exec: %lu",
                 cmdState.execCount);

    // Прогресс выполнения
    if (SCAN_GetCommandCount() > 0) {
      int progress_width =
          (cmdState.cmdIndex * (LCD_WIDTH - 40)) / SCAN_GetCommandCount();
      DrawRect(20, LCD_HEIGHT - 20, LCD_WIDTH - 40, 6, C_FILL);
      if (progress_width > 0) {
        FillRect(20, LCD_HEIGHT - 20, progress_width, 6, C_INVERT);
      }
    }
  }

  // Нижняя панель с подсказками
  int y = LCD_HEIGHT - 10;
  PrintSmallEx(2, y, POS_L, C_FILL, SCAN_IsCommandMode() ? "RUN" : "STOP");

  // Индикатор активности (мигающий)
  static bool blink = false;
  static uint32_t lastBlink = 0;
  if (Now() - lastBlink > 500) {
    blink = !blink;
    lastBlink = Now();
  }

  if (SCAN_IsCommandMode() && blink) {
    FillRect(LCD_WIDTH - 10, y - 3, 3, 3, C_FILL);
  }

  PrintSmallEx(LCD_XCENTER, y, POS_C, C_FILL, "EXIT:PTT");
}
