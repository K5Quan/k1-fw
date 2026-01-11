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
  bool isPaused;      // Пауза выполнения команд
  bool showInfo;      // Показать информацию о команде
  uint8_t profileNum; // Текущий профиль (1-4)
  uint16_t cmdIndex;  // Индекс текущей команды
  uint32_t lastExecTime; // Время последнего выполнения
  uint32_t execCount;    // Счетчик выполненных команд
  char filename[32];     // Имя текущего файла
} CmdScanState;

static CmdScanState cmdState = {.isActive = false,
                                .isPaused = false,
                                .showInfo = true,
                                .profileNum = 1,
                                .cmdIndex = 0,
                                .lastExecTime = 0,
                                .execCount = 0,
                                .filename = "/scans/cmd1.bin"};

// =============================
// Вспомогательные функции
// =============================

// Загрузить профиль по номеру
static void LoadProfile(uint8_t num) {
  snprintf(cmdState.filename, sizeof(cmdState.filename), "/scans/cmd%d.bin",
           num);

  if (SCMD_Init(cmdState.filename)) {
    cmdState.profileNum = num;
    cmdState.cmdIndex = 0;
    cmdState.execCount = 0;
    cmdState.isPaused = false;
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

  SCMD_Command *cmd = SCMD_GetCurrent();
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
    PrintSmallEx(2, y, POS_L, C_FILL, "Loops: %d", cmd->loop_count);
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
  PrintSmallEx(LCD_WIDTH - 2, y, POS_R, C_FILL, "#%d", cmdState.cmdIndex);

  y += 8;
  PrintSmallEx(2, y, POS_L, C_FILL, "Exec: %lu", cmdState.execCount);

  // Индикатор паузы
  if (cmdState.isPaused) {
    PrintMediumBoldEx(LCD_XCENTER, LCD_HEIGHT - 30, POS_C, C_INVERT, "PAUSED");
  }
}

// Выполнить одну команду
static void ExecuteOneCommand(void) {
  if (cmdState.isPaused)
    return;

  SCMD_Command *cmd = SCMD_GetCurrent();
  if (!cmd) {
    SCMD_Rewind();
    return;
  }

  // Пропускаем если приоритет слишком низкий
  if (SCMD_ShouldSkip()) {
    SCMD_Advance();
    return;
  }

  // Логируем выполнение
  Log("[CMDSCAN] Exec #%d: type=%d, f=%lu", cmdState.cmdIndex, cmd->type,
      cmd->start);

  // Выполняем команду
  SCMD_ExecuteCurrent();
  cmdState.execCount++;

  // Переходим к следующей
  if (!SCMD_Advance()) {
    // Конец файла - перематываем
    SCMD_Rewind();
    Log("[CMDSCAN] End of program, rewinding");
  }

  cmdState.cmdIndex = SCMD_GetCurrentIndex();
  cmdState.lastExecTime = Now();
}

// =============================
// API функций приложения
// =============================

void CMDSCAN_init(void) {
  // Переключаемся в режим VFO для командного сканирования
  SCAN_SetMode(SCAN_MODE_SINGLE);
  SCAN_SetCommandMode(true);

  // Загружаем первый профиль
  LoadProfile(cmdState.profileNum);

  cmdState.isActive = true;
  cmdState.isPaused = false;
  cmdState.execCount = 0;
  cmdState.lastExecTime = Now();

  Log("[CMDSCAN] Initialized");
}

void CMDSCAN_deinit(void) {
  // Закрываем SCMD и возвращаем обычный режим
  SCMD_Close();
  SCAN_SetCommandMode(false);
  cmdState.isActive = false;

  Log("[CMDSCAN] Deinitialized");
}

void CMDSCAN_update(void) {
  if (!cmdState.isActive)
    return;

  // Выполняем команды каждые 100мс (или по dwell из команды)
  static uint32_t lastUpdate = 0;
  uint32_t now = Now();

  if (now - lastUpdate >= 100) { // 10Hz выполнение
    ExecuteOneCommand();
    lastUpdate = now;
  }

  // Обновляем радио (для squelch и т.д.)
  RADIO_UpdateMultiwatch(gRadioState);
  RADIO_UpdateSquelch(gRadioState);
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

    // Управление выполнением
    case KEY_UP:
      if (cmdState.isPaused) {
        // Шаг вперед при паузе
        ExecuteOneCommand();
      }
      return true;

    case KEY_DOWN:
      // Перемотка в начало
      SCMD_Rewind();
      cmdState.cmdIndex = 0;
      return true;

    case KEY_SIDE1:
      // Пауза/продолжить
      cmdState.isPaused = !cmdState.isPaused;
      Log("[CMDSCAN] %s", cmdState.isPaused ? "Paused" : "Resumed");
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
      return true;

    case KEY_EXIT:
      // Полный сброс
      SCMD_Rewind();
      cmdState.execCount = 0;
      cmdState.cmdIndex = 0;
      return true;
    }
  }

  return false;
}

void CMDSCAN_render(void) {
  // Статусная строка
  STATUSLINE_RenderRadioSettings();

  // Заголовок
  PrintSmallEx(LCD_XCENTER, 14, POS_C, C_FILL, "CMD SCAN %s",
               cmdState.filename);

  // Отображаем текущую частоту VFO
  if (vfo) {
    char freqBuf[16];
    mhzToS(freqBuf, vfo->msm.f);
    PrintMediumBoldEx(LCD_XCENTER, 14 + 8, POS_C, C_FILL, "%s", freqBuf);

    // RSSI и squelch
    PrintSmallEx(2, 40, POS_L, C_FILL, "RSSI: %u", vfo->msm.rssi);
    PrintSmallEx(LCD_WIDTH - 2, 40, POS_R, C_FILL,
                 vfo->msm.open ? "OPEN" : "CLOSED");
  }

  // Информация о команде
  RenderCommandInfo();

  // Нижняя панель с подсказками
  int y = LCD_HEIGHT - 1;
  PrintSmallEx(2, y, POS_L, C_FILL, cmdState.isPaused ? "PLAY" : "PAUSE");
  PrintSmallEx(LCD_WIDTH / 4, y, POS_C, C_FILL, "1-4:PROF");
  PrintSmallEx(LCD_WIDTH / 2, y, POS_C, C_FILL, "#:EXIT");
  PrintSmallEx(LCD_WIDTH * 3 / 4, y, POS_C, C_FILL, "*:RELOAD");

  // Индикатор активности
  if (cmdState.isActive && !cmdState.isPaused) {
    // Мигающий индикатор выполнения
    static bool blink = false;
    static uint32_t lastBlink = 0;
    if (Now() - lastBlink > 500) {
      blink = !blink;
      lastBlink = Now();
    }
    if (blink) {
      FillRect(LCD_WIDTH - 10, 2, 8, 8, C_FILL);
    }
  }
}
