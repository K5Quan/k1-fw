#include "cmdedit.h"
#include "../driver/st7565.h"
#include "../driver/systick.h"
#include "../driver/uart.h"
#include "../external/printf/printf.h"
#include "../helper/menu.h"
#include "../helper/scan.h"
#include "../helper/scancommand.h"
#include "../ui/components.h"
#include "../ui/finput.h"
#include "../ui/graphics.h"
#include "../ui/statusline.h"
#include "apps.h"
#include <string.h>

// ============================================================================
// Контекст редактора
// ============================================================================

#define SCMD_MAX_COMMANDS 16

typedef enum {
  MODE_LIST, // Режим списка
  MODE_EDIT, // Режим редактирования
} EditorMode;

typedef struct {
  uint16_t totalCommands; // Общее количество команд
  SCMD_Command commands[SCMD_MAX_COMMANDS]; // Буфер команд
  char filename[32]; // Имя редактируемого файла
  bool modified;     // Флаг изменения
  EditorMode mode;   // Текущий режим
  uint8_t editField; // Поле для редактирования (в режиме EDIT)
} EditContext;

static EditContext gEditCtx;
static Menu cmdMenu;

uint8_t selected_index;

// ============================================================================
// Вспомогательные функции
// ============================================================================

static void LoadFile(const char *filename) {
  strcpy(gEditCtx.filename, filename);

  // Открываем файл для чтения
  uint8_t buffer[256];
  struct lfs_file_config config = {.buffer = buffer, .attr_count = 0};
  lfs_file_t file;

  int err = lfs_file_opencfg(&gLfs, &file, filename, LFS_O_RDONLY, &config);
  if (err < 0) {
    Log("[CMDEDIT] Failed to open %s", filename);
    gEditCtx.totalCommands = 0;
    return;
  }

  // Пропускаем заголовок
  SCMD_Header header;
  lfs_file_read(&gLfs, &file, &header, sizeof(header));

  if (header.magic != SCMD_MAGIC) {
    Log("[CMDEDIT] Invalid file format");
    lfs_file_close(&gLfs, &file);
    gEditCtx.totalCommands = 0;
    return;
  }

  // Читаем команды
  gEditCtx.totalCommands = header.cmd_count;
  if (gEditCtx.totalCommands > SCMD_MAX_COMMANDS) {
    gEditCtx.totalCommands = SCMD_MAX_COMMANDS;
  }

  for (uint16_t i = 0; i < gEditCtx.totalCommands; i++) {
    lfs_file_read(&gLfs, &file, &gEditCtx.commands[i], sizeof(SCMD_Command));
  }

  lfs_file_close(&gLfs, &file);
  gEditCtx.modified = false;

  Log("[CMDEDIT] Loaded %u commands from %s", gEditCtx.totalCommands, filename);
}

static void SaveFile(void) {
  if (gEditCtx.filename[0] == 0) {
    return;
  }

  // Создаем заголовок
  SCMD_Header header = {.magic = SCMD_MAGIC,
                        .version = SCMD_VERSION,
                        .cmd_count = gEditCtx.totalCommands,
                        .entry_point = 0,
                        .crc32 = 0xDEADBEEF};

  // Сохраняем в файл
  if (SCMD_CreateFile(gEditCtx.filename, gEditCtx.commands,
                      gEditCtx.totalCommands)) {
    gEditCtx.modified = false;

    // Показываем уведомление
    FillRect(0, LCD_YCENTER - 4, LCD_WIDTH, 9, C_FILL);
    PrintMediumBoldEx(LCD_XCENTER, LCD_YCENTER + 3, POS_C, C_INVERT, "Saved!");
    ST7565_Blit();
    SYSTICK_DelayMs(1000);

    Log("[CMDEDIT] Saved %u commands to %s", gEditCtx.totalCommands,
        gEditCtx.filename);
  } else {
    Log("[CMDEDIT] Failed to save commands");
  }
}

// ============================================================================
// Функции работы с командами
// ============================================================================

static void AddCommand(void) {
  if (gEditCtx.totalCommands >= SCMD_MAX_COMMANDS) {
    return;
  }

  // Создаем новую команду с дефолтными значениями
  SCMD_Command newCmd = {.type = SCMD_CHANNEL,
                         .start = 100000000, // 100 MHz
                         .end = 100000000,
                         .step = 12500,
                         .dwell_ms = 100,
                         .priority = 0,
                         .flags = 0};

  gEditCtx.commands[gEditCtx.totalCommands] = newCmd;
  gEditCtx.totalCommands++;
  gEditCtx.modified = true;

  cmdMenu.num_items = gEditCtx.totalCommands;
}

static void DeleteCommand(uint16_t index) {
  if (index >= gEditCtx.totalCommands) {
    return;
  }

  // Сдвигаем все команды после удаляемой
  for (uint16_t i = index; i < gEditCtx.totalCommands - 1; i++) {
    gEditCtx.commands[i] = gEditCtx.commands[i + 1];
  }

  gEditCtx.totalCommands--;
  gEditCtx.modified = true;

  cmdMenu.num_items = gEditCtx.totalCommands;
}

static void DuplicateCommand(uint16_t index) {
  if (gEditCtx.totalCommands >= SCMD_MAX_COMMANDS ||
      index >= gEditCtx.totalCommands) {
    return;
  }

  // Копируем команду в конец списка
  gEditCtx.commands[gEditCtx.totalCommands] = gEditCtx.commands[index];
  gEditCtx.totalCommands++;
  gEditCtx.modified = true;

  cmdMenu.num_items = gEditCtx.totalCommands;
}

// ============================================================================
// Режим редактирования команды
// ============================================================================

static void setCommandFreq(uint32_t fs, uint32_t fe) {
  uint16_t index = selected_index;
  gEditCtx.commands[index].start = fs;
  gEditCtx.commands[index].end = fe;
  gEditCtx.modified = true;
  gFInputActive = false;
}

static void setCommandDwell(uint32_t dwell, uint32_t _) {
  uint16_t index = selected_index;
  gEditCtx.commands[index].dwell_ms = dwell;
  gEditCtx.modified = true;
  gFInputActive = false;
}

static void setCommandStep(uint32_t step, uint32_t _) {
  uint16_t index = selected_index;
  gEditCtx.commands[index].step = step;
  gEditCtx.modified = true;
  gFInputActive = false;
}

static void EditCommandField(uint16_t index, uint8_t field) {
  SCMD_Command *cmd = &gEditCtx.commands[index];

  switch (field) {
  case 0: // Тип команды
    cmd->type = (cmd->type + 1) % SCMD_COUNT;
    gEditCtx.modified = true;
    break;

  case 1: // Начальная частота
    gFInputCallback = setCommandFreq;
    FINPUT_setup(0, BK4819_F_MAX, UNIT_MHZ, cmd->type == SCMD_RANGE);
    gFInputValue1 = 0;
    gFInputValue2 = 0;
    FINPUT_init();
    gFInputActive = true;
    return;

  case 2: // Конечная частота (для RANGE)
    if (cmd->type == SCMD_RANGE) {
      gFInputCallback = setCommandFreq;
      FINPUT_setup(0, BK4819_F_MAX, UNIT_MHZ, cmd->type == SCMD_RANGE);
      gFInputValue1 = 0;
      gFInputValue2 = 0;
      FINPUT_init();
      gFInputActive = true;
    }
    return;

  case 3: // Шаг (для RANGE)
    if (cmd->type == SCMD_RANGE) {
      gFInputCallback = setCommandStep;
      FINPUT_setup(100, 100000, UNIT_HZ, false);
      gFInputValue1 = cmd->step;
      FINPUT_init();
      gFInputActive = true;
    }
    return;

  case 4: // Пауза
    gFInputCallback = setCommandDwell;
    FINPUT_setup(0, 60000, UNIT_MS, false);
    gFInputValue1 = cmd->dwell_ms;
    FINPUT_init();
    gFInputActive = true;
    return;

  case 5: // Приоритет
    cmd->priority = (cmd->priority + 1) % 10;
    gEditCtx.modified = true;
    break;

  case 6: // Флаги
    cmd->flags ^= SCMD_FLAG_AUTO_WHITELIST;
    gEditCtx.modified = true;
    break;
  }
}

// ============================================================================
// Отображение списка команд
// ============================================================================

static void renderCommandItem(uint16_t index, uint8_t i) {
  const SCMD_Command *cmd = &gEditCtx.commands[index];
  const uint8_t y = MENU_Y + i * MENU_ITEM_H;
  const uint8_t ty = y + 7;

  // Номер и тип команды
  PrintMediumEx(2, ty, POS_L, C_FILL, "%u:%s", index + 1,
                SCMD_NAMES_SHORT[cmd->type]);

  // Информация о команде
  switch (cmd->type) {
  case SCMD_RANGE:
    PrintSmallEx(40, ty, POS_L, C_INVERT, "%lu-%lu", cmd->start / KHZ,
                 cmd->end / KHZ);
    PrintSmallEx(LCD_WIDTH - 22, ty, POS_R, C_INVERT, "%u", cmd->step / KHZ);
    break;

  case SCMD_CHANNEL:
    PrintSmallEx(40, ty, POS_L, C_INVERT, "%lu.%05lu", cmd->start / MHZ,
                 cmd->start % MHZ);
    break;

  case SCMD_PAUSE:
    PrintSmallEx(40, ty, POS_L, C_INVERT, "%ums", cmd->dwell_ms);
    break;

  case SCMD_JUMP:
  case SCMD_MARKER:
    PrintSmallEx(40, ty, POS_L, C_INVERT, "---");
    break;
  }

  // Приоритет и флаги
  if (cmd->priority > 0) {
    PrintSmallEx(LCD_WIDTH - 5, ty, POS_R, C_INVERT, "P%u", cmd->priority);
  }
  if (cmd->flags & SCMD_FLAG_AUTO_WHITELIST) {
    PrintSmallEx(LCD_WIDTH - 5, ty, POS_R, C_INVERT, "W");
  }
}

// ============================================================================
// Отображение режима редактирования
// ============================================================================

static void renderEditMode(void) {
  if (gFInputActive) {
    FINPUT_render();
    return;
  }

  uint16_t index = selected_index;
  if (index >= gEditCtx.totalCommands) {
    return;
  }

  SCMD_Command *cmd = &gEditCtx.commands[index];

  // Заголовок
  PrintMediumEx(LCD_XCENTER, 16, POS_C, C_FILL, "Edit Cmd %u: %s", index + 1,
                SCMD_NAMES_SHORT[cmd->type]);

  uint8_t y = 22;
  const uint8_t lineH = 6;

  // Поля для редактирования
  bool highlight = gEditCtx.editField == 0;
  PrintSmallEx(2, y, POS_L, highlight ? C_INVERT : C_FILL, "[0] Type: %s",
               SCMD_NAMES[cmd->type]);
  y += lineH;

  highlight = gEditCtx.editField == 1;
  PrintSmallEx(2, y, POS_L, highlight ? C_INVERT : C_FILL,
               "[1] Start: %lu.%05lu", cmd->start / 100000,
               cmd->start % 100000);
  y += lineH;

  if (cmd->type == SCMD_RANGE) {
    highlight = gEditCtx.editField == 2;
    PrintSmallEx(2, y, POS_L, highlight ? C_INVERT : C_FILL,
                 "[2] End: %lu.%05lu", cmd->end / 100000, cmd->end % 100000);
    y += lineH;

    highlight = gEditCtx.editField == 3;
    PrintSmallEx(2, y, POS_L, highlight ? C_INVERT : C_FILL, "[3] Step: %lu",
                 cmd->step);
    y += lineH;
  }

  highlight = gEditCtx.editField == 4;
  PrintSmallEx(2, y, POS_L, highlight ? C_INVERT : C_FILL, "[4] Dwell: %u ms",
               cmd->dwell_ms);
  y += lineH;

  highlight = gEditCtx.editField == 5;
  PrintSmallEx(2, y, POS_L, highlight ? C_INVERT : C_FILL, "[5] Priority: %u",
               cmd->priority);
  y += lineH;

  highlight = gEditCtx.editField == 6;
  PrintSmallEx(2, y, POS_L, highlight ? C_INVERT : C_FILL, "[6] Auto-WL: %s",
               (cmd->flags & SCMD_FLAG_AUTO_WHITELIST) ? "ON" : "OFF");

  // Подсказки
  PrintSmallEx(2, LCD_HEIGHT - 2, POS_L, C_FILL,
               "0-6:Field MENU:Chg EXIT:List");
}

// ============================================================================
// Обработка ввода
// ============================================================================

static bool listModeAction(const uint16_t index, KEY_Code_t key,
                           Key_State_t state) {
  if (state == KEY_LONG_PRESSED) {
    switch (key) {
    case KEY_0:
      // Очистить все команды
      gEditCtx.totalCommands = 0;
      gEditCtx.modified = true;
      cmdMenu.num_items = 0;
      return true;
    case KEY_F:
      // Сохранить
      SaveFile();
      return true;
    default:
      break;
    }
  }

  if (state == KEY_RELEASED) {
    switch (key) {
    case KEY_EXIT:
      if (gEditCtx.modified) {
        SaveFile();
      }
      APPS_exit();
      return true;

    case KEY_MENU:
      // Войти в режим редактирования
      gEditCtx.mode = MODE_EDIT;
      gEditCtx.editField = 0;
      selected_index = index;
      return true;

    case KEY_F:
      // Сохранить
      SaveFile();
      return true;

    case KEY_STAR:
      // Запуск сканирования
      SCAN_LoadCommandFile(gEditCtx.filename);
      return true;

    case KEY_1:
      // Добавить команду
      AddCommand();
      return true;

    case KEY_2:
      // Дублировать команду
      DuplicateCommand(index);
      return true;

    case KEY_0:
      // Удалить команду
      DeleteCommand(index);
      return true;

    default:
      break;
    }
  }

  return false;
}

static bool editModeKey(KEY_Code_t key, Key_State_t state) {
  if (gFInputActive) {
    return false;
  }

  uint16_t index = selected_index;
  if (index >= gEditCtx.totalCommands) {
    return false;
  }

  if (state == KEY_RELEASED) {
    switch (key) {
    case KEY_EXIT:
      // Вернуться в список
      gEditCtx.mode = MODE_LIST;
      return true;

    case KEY_MENU:
      // Изменить текущее поле
      EditCommandField(index, gEditCtx.editField);
      return true;

    case KEY_UP:
      // Предыдущее поле
      if (gEditCtx.editField > 0) {
        gEditCtx.editField--;
      }
      return true;

    case KEY_DOWN:
      // Следующее поле
      if (gEditCtx.editField < 6) {
        gEditCtx.editField++;
      }
      return true;

    case KEY_0:
    case KEY_1:
    case KEY_2:
    case KEY_3:
    case KEY_4:
    case KEY_5:
    case KEY_6:
      // Быстрый выбор поля
      gEditCtx.editField = key - KEY_0;
      EditCommandField(index, gEditCtx.editField);
      return true;

    default:
      break;
    }
  }

  return false;
}

bool CMDEDIT_key(KEY_Code_t key, Key_State_t state) {
  if (gEditCtx.mode == MODE_EDIT) {
    return editModeKey(key, state);
  }

  // Режим списка
  if (MENU_HandleInput(key, state)) {
    return true;
  }

  return false;
}

// ============================================================================
// Отображение
// ============================================================================

void CMDEDIT_render(void) {
  if (gEditCtx.mode == MODE_EDIT) {
    renderEditMode();
    return;
  }

  // Режим списка
  if (gEditCtx.totalCommands == 0) {
    PrintMediumEx(LCD_XCENTER, 40, POS_C, C_FILL, "No commands");
    PrintSmallEx(LCD_XCENTER, 50, POS_C, C_FILL, "Press 1 to add");
    return;
  }

  MENU_Render();

  // Подсказки
  PrintSmallEx(2, LCD_HEIGHT - 2, POS_L, C_FILL,
               "MENU:Edit 1:Add 2:Dup 0:Del F:Save *:Run");
}

// ============================================================================
// Инициализация
// ============================================================================

static void initMenu(void) {
  cmdMenu.num_items = gEditCtx.totalCommands;
  cmdMenu.itemHeight = MENU_ITEM_H;
  cmdMenu.title = "Commands";
  cmdMenu.render_item = renderCommandItem;
  cmdMenu.action = listModeAction;
  MENU_Init(&cmdMenu);
}

void CMDEDIT_init() {
  memset(&gEditCtx, 0, sizeof(gEditCtx));
  gEditCtx.mode = MODE_LIST;

  LoadFile("/scans/cmd1.bin");
  initMenu();

  STATUSLINE_SetText("CMD edit %s%s", gEditCtx.filename,
                     gEditCtx.modified ? "*" : "");
}

void CMDEDIT_update() {
  // Обновление статусной строки при изменениях
  if (gEditCtx.modified) {
    STATUSLINE_SetText("CMD edit %s*", gEditCtx.filename);
  }
}
