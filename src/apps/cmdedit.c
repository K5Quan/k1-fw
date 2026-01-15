#include "cmdedit.h"
#include "../driver/uart.h"
#include "../external/printf/printf.h"
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

typedef struct {
  int16_t currentIndex;   // Текущая выбранная команда
  uint16_t totalCommands; // Общее количество команд
  SCMD_Command commands[SCMD_MAX_COMMANDS]; // Буфер команд
  char filename[32]; // Имя редактируемого файла
  bool modified;     // Флаг изменения
} EditContext;

static EditContext gEditCtx;

// ============================================================================
// Вспомогательные функции
// ============================================================================

static const char *GetCommandTypeName(SCMD_Type type) {
  switch (type) {
  case SCMD_CHANNEL:
    return "Channel";
  case SCMD_RANGE:
    return "Range";
  case SCMD_PAUSE:
    return "Pause";
  case SCMD_JUMP:
    return "Jump";
  case SCMD_MARKER:
    return "Marker";
  default:
    return "Unknown";
  }
}

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
  gEditCtx.currentIndex = (gEditCtx.totalCommands > 0) ? 0 : -1;
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
    Log("[CMDEDIT] Saved %u commands to %s", gEditCtx.totalCommands,
        gEditCtx.filename);
  } else {
    Log("[CMDEDIT] Failed to save commands");
  }
}

static void setCommandFreq(uint32_t fs, uint32_t fe) {
  gEditCtx.commands[gEditCtx.currentIndex].start = fs;
  gEditCtx.commands[gEditCtx.currentIndex].end = fe;
  gEditCtx.modified = true;
  gFInputActive = false;
}

static void setCommandDwell(uint32_t dwell, uint32_t _) {
  gEditCtx.commands[gEditCtx.currentIndex].dwell_ms = dwell;
  gEditCtx.modified = true;
  gFInputActive = false;
}

static void EditCommandField(uint8_t field) {
  SCMD_Command *cmd = &gEditCtx.commands[gEditCtx.currentIndex];

  switch (field) {
  case 0: // Тип команды
    cmd->type = (cmd->type + 1) % 5;
    break;

  case 1: // Начальная частота
    gFInputCallback = setCommandFreq;
    FINPUT_setup(0, BK4819_F_MAX, UNIT_HZ, false);
    gFInputValue1 = cmd->start;
    gFInputActive = true;
    return;

  case 3: // Пауза
    gFInputCallback = setCommandDwell;
    FINPUT_setup(0, 60000, UNIT_MS, false); // До 60 секунд
    gFInputValue1 = cmd->dwell_ms;
    gFInputActive = true;
    return;

  case 4: // Приоритет
    cmd->priority = (cmd->priority + 1) % 10;
    break;

  case 5: // Флаги
    cmd->flags ^= SCMD_FLAG_AUTO_WHITELIST;
    break;
  }

  gEditCtx.modified = true;
}

// ============================================================================
// Обработка ввода
// ============================================================================

bool CMDEDIT_key(KEY_Code_t key, Key_State_t state) {
  if (gFInputActive) {
    // Обработка ввода частоты
    return false;
  }

  if (state == KEY_RELEASED) {
    switch (key) {
    case KEY_MENU:
      // Режим редактирования поля
      if (gEditCtx.currentIndex >= 0) {
        static uint8_t editField = 0;
        EditCommandField(editField);
        editField = (editField + 1) % 6;
      }
      return true;

    case KEY_EXIT:
      if (gEditCtx.modified) {
        // Предлагаем сохранить
        // TODO: Диалог подтверждения
        SaveFile();
      }
      APPS_exit();
      return true;

    case KEY_UP:
      if (gEditCtx.currentIndex > 0) {
        gEditCtx.currentIndex--;
      }
      return true;

    case KEY_DOWN:
      if (gEditCtx.currentIndex < gEditCtx.totalCommands - 1) {
        gEditCtx.currentIndex++;
      }
      return true;

    case KEY_F:
      // Сохранение
      SaveFile();
      return true;

    case KEY_STAR:
      // Запуск сканирования с этого файла
      SCAN_LoadCommandFile(gEditCtx.filename);
      return true;
    }
  }

  return false;
}

// ============================================================================
// Отображение
// ============================================================================

void CMDEDIT_render(void) {
  if (gFInputActive) {
    FINPUT_render();
    return;
  }

  if (gEditCtx.totalCommands == 0) {
    PrintMediumEx(LCD_XCENTER, 40, POS_C, C_FILL, "No commands");
    return;
  }

  // Текущая команда
  SCMD_Command *cmd = &gEditCtx.commands[gEditCtx.currentIndex];

  char fBuf[16];

  // Детальная информация
  PrintMediumEx(2, 18, POS_L, C_FILL, "Cmd %u/%u: %s",
                gEditCtx.currentIndex + 1, gEditCtx.totalCommands,
                GetCommandTypeName(cmd->type));

  switch (cmd->type) {
  case SCMD_RANGE:
    PrintMediumEx(2, 30, POS_L, C_FILL, "Freq: %lu-%lu", cmd->start / 1000,
                  cmd->end / 1000);
    PrintMediumEx(2, 38, POS_L, C_FILL, "Step: %lu", cmd->step);
    break;

  case SCMD_CHANNEL:
    PrintMediumEx(2, 30, POS_L, C_FILL, "Freq: %lu", cmd->start);
    break;

  case SCMD_PAUSE:
    PrintMediumEx(2, 30, POS_L, C_FILL, "Time: %u ms", cmd->dwell_ms);
    break;

  case SCMD_JUMP:
  case SCMD_MARKER:
    PrintMediumEx(2, 30, POS_L, C_FILL, "Special command");
    break;
  }

  PrintMediumEx(2, 38 + 8, POS_L, C_FILL, "Prio: %u  Flags: 0x%02X",
                cmd->priority, cmd->flags);

  if (cmd->flags & SCMD_FLAG_AUTO_WHITELIST) {
    PrintMediumEx(LCD_WIDTH - 2, 38 + 8 + 8, POS_R, C_FILL, "Auto-W");
  }

  // Подсказки
  PrintSmallEx(2, LCD_HEIGHT - 2, POS_L, C_FILL,
               "MENU:Edit EXIT:Exit F:Save *:Run");
}

// ============================================================================
// API функции
// ============================================================================

void CMDEDIT_init() {
  memset(&gEditCtx, 0, sizeof(gEditCtx));
  LoadFile("/scans/cmd1.bin");
  STATUSLINE_SetText("CMD edit %s", "/scans/cmd1.bin");
}
