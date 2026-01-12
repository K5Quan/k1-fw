#include "scancommand.h"
#include "../driver/lfs.h"
#include "../driver/systick.h"
#include "../driver/uart.h"
#include "../misc.h"

// Внутренний буфер для файла
static uint8_t s_file_buffer[256];

// Текущий контекст
static SCMD_Context s_ctx = {0};
static lfs_file_t s_file;

void ScanChannel(uint32_t freq, uint16_t dwell_ms, uint16_t timeout_ms) {
  Log("ScanChannel: freq=%lu Hz, dwell=%u ms, timeout=%u ms", freq, dwell_ms,
      timeout_ms);
  // Заглушка: ничего не делаем, просто логируем
}

void ScanRange(uint32_t start, uint32_t end, uint32_t step, uint16_t dwell_ms,
               uint16_t timeout_ms) {
  Log("ScanRange: %lu-%lu Hz, step=%lu, dwell=%u ms, timeout=%u ms", start, end,
      step, dwell_ms, timeout_ms);
  // Заглушка
}

bool IsBlacklisted(uint32_t freq) {
  Log("IsBlacklisted: freq=%lu Hz -> returning false", freq);
  return false; // По умолчанию не в черном списке
}

bool CheckSignalCondition(uint32_t freq) {
  Log("CheckSignalCondition: freq=%lu Hz -> returning false", freq);
  return false; // По умолчанию сигнал не обнаружен
}

uint8_t GetCurrentMinPriority(void) {
  static uint8_t min_prio = 0;
  // Log("GetCurrentMinPriority: -> returning %u", min_prio);
  return min_prio; // Принимаем все команды
}

void DelayMs(uint32_t ms) {
  Log("DelayMs: %lu ms (simulated)", ms);
  // Заглушка: не реальная задержка, просто логируем
  // В реальной системе здесь будет vTaskDelay или HAL_Delay
}

// ============================================================================
// ДОПОЛНИТЕЛЬНЫЕ ЗАГЛУШКИ ДЛЯ РАСШИРЕННЫХ ФУНКЦИЙ
// ============================================================================

void AddToWhitelist(uint32_t freq) { Log("AddToWhitelist: freq=%lu Hz", freq); }

bool SignalDetected(void) {
  Log("SignalDetected: -> returning false");
  return false;
}

void SetScanPriority(uint8_t priority) {
  Log("SetScanPriority: priority=%u", priority);
}

void SetScanMode(uint32_t mode) { Log("SetScanMode: mode=%lu", mode); }

// ===================================

// Загрузка команды по текущей позиции
bool SCMD_LoadCommand(SCMD_Command *cmd) {
  if (lfs_file_size(&gLfs, &s_file) <= s_ctx.file_pos) {
    return false;
  }

  // Читаем структуру
  int bytes = lfs_file_read(&gLfs, &s_file, cmd, sizeof(SCMD_Command));
  if (bytes != sizeof(SCMD_Command)) {
    return false;
  }

  return true;
}

// Обработка переходов
bool SCMD_HandleGoto(void) {
  uint16_t offset = s_ctx.current.goto_offset;

  Log("[SCMD] HandleGoto: offset=%u, current_pos=%lu", offset, s_ctx.file_pos);

  // Защита от бесконечного цикла
  static uint16_t goto_count = 0;
  goto_count++;
  if (goto_count > 100) {
    Log("[SCMD] ERROR: Too many GOTO in a row, possible infinite loop");
    goto_count = 0;
    return false;
  }

  // Рассчитываем новую позицию
  uint32_t target_pos = sizeof(SCMD_Header) + (offset * sizeof(SCMD_Command));

  Log("[SCMD] Target position: %lu", target_pos);

  // Проверяем, что позиция в пределах файла
  lfs_soff_t file_size = lfs_file_size(&gLfs, &s_file);
  if (target_pos >= file_size) {
    Log("[SCMD] ERROR: GOTO target %lu beyond file size %ld", target_pos,
        file_size);
    return false;
  }

  // Перемещаемся
  lfs_file_seek(&gLfs, &s_file, target_pos, LFS_SEEK_SET);
  s_ctx.file_pos = target_pos;
  s_ctx.cmd_index = offset;

  // Сбрасываем следующую команду
  memset(&s_ctx.next, 0, sizeof(s_ctx.next));

  // Загружаем текущую
  if (!SCMD_LoadCommand(&s_ctx.current)) {
    Log("[SCMD] ERROR: Cannot load command at target position");
    return false;
  }

  // Загружаем следующую
  s_ctx.has_next = SCMD_LoadCommand(&s_ctx.next);

  Log("[SCMD] GOTO complete: new cmd_index=%u, type=%d", s_ctx.cmd_index,
      s_ctx.current.type);

  goto_count = 0; // Сбрасываем счетчик после успешного перехода
  return true;
}

// Инициализация сканера
bool SCMD_Init(const char *filename) {
  // Открываем файл с буфером
  struct lfs_file_config config = {.buffer = s_file_buffer, .attr_count = 0};

  int err = lfs_file_opencfg(&gLfs, &s_file, filename, LFS_O_RDONLY, &config);
  if (err < 0) {
    return false;
  }

  // Читаем заголовок
  SCMD_Header header;
  lfs_file_read(&gLfs, &s_file, &header, sizeof(header));

  if (header.magic != SCMD_MAGIC) {
    lfs_file_close(&gLfs, &s_file);
    return false;
  }

  // Сбрасываем контекст
  memset(&s_ctx, 0, sizeof(s_ctx));
  s_ctx.file_pos = sizeof(SCMD_Header);
  s_ctx.cmd_index = 0;
  s_ctx.cmd_count = header.cmd_count;

  // Загружаем первую команду
  s_ctx.has_next = SCMD_LoadCommand(&s_ctx.current);
  if (s_ctx.has_next) {
    // Предзагружаем следующую
    SCMD_Advance();
  }

  return true;
}

// Переход к следующей команде
bool SCMD_Advance(void) {
  // Текущая становится предыдущей, следующая - текущей
  s_ctx.current = s_ctx.next;
  s_ctx.cmd_index++;

  Log("[SCMD] Advance: cmd_index=%u, goto_offset=%u", s_ctx.cmd_index,
      s_ctx.current.goto_offset);

  // Проверяем переходы
  if (s_ctx.current.goto_offset > 0) {
    Log("[SCMD] Executing GOTO to offset %u", s_ctx.current.goto_offset);
    return SCMD_HandleGoto();
  }

  // Загружаем следующую команду
  s_ctx.has_next = SCMD_LoadCommand(&s_ctx.next);

  if (!s_ctx.has_next) {
    Log("[SCMD] No more commands available");
  }

  return s_ctx.has_next;
}

// Реализации команд
static void SCMD_ExecuteChannel(void) {
  uint32_t freq = s_ctx.current.start;
  uint16_t dwell = s_ctx.current.dwell_ms;
  uint16_t timeout = s_ctx.current.timeout_ms;

  // Проверка черного списка (если не игнорировать)
  if (!(s_ctx.current.flags & SCMD_FLAG_IGNORE_BLACK)) {
    if (IsBlacklisted(freq)) {
      return;
    }
  }

  // Сканирование канала
  ScanChannel(freq, dwell, timeout);

  // Автоматический белый список
  if ((s_ctx.current.flags & SCMD_FLAG_AUTO_WHITELIST) && SignalDetected()) {
    AddToWhitelist(freq);
  }
}

static void SCMD_ExecuteRange(void) {
  uint32_t start = s_ctx.current.start;
  uint32_t end = s_ctx.current.end;
  uint16_t dwell = s_ctx.current.dwell_ms;
  uint32_t step = (end - start) / 100; // Пример: 1% шаг

  for (uint32_t freq = start; freq <= end; freq += step) {
    if (IsBlacklisted(freq))
      continue;

    ScanChannel(freq, dwell, s_ctx.current.timeout_ms);
  }
}

static void SCMD_ExecuteCondJump(void) {
  // Проверяем условие (сигнал на указанной частоте)
  if (CheckSignalCondition(s_ctx.current.start)) {
    SCMD_HandleGoto();
  }
}

static void SCMD_ExecutePause(void) { SYSTICK_DelayMs(s_ctx.current.dwell_ms); }

static void SCMD_ExecuteCall(void) {
  if (s_ctx.call_ptr < 4) {
    // Сохраняем точку возврата (текущая позиция + 1 команда)
    s_ctx.call_stack[s_ctx.call_ptr] = s_ctx.file_pos + sizeof(SCMD_Command);
    s_ctx.call_ptr++;

    // Выполняем переход
    SCMD_HandleGoto();
  }
}

static void SCMD_ExecuteReturn(void) {
  if (s_ctx.call_ptr > 0) {
    s_ctx.call_ptr--;
    uint32_t return_pos = s_ctx.call_stack[s_ctx.call_ptr];

    // Возвращаемся
    lfs_file_seek(&gLfs, &s_file, return_pos, LFS_SEEK_SET);
    s_ctx.file_pos = return_pos;

    // Загружаем команду
    SCMD_LoadCommand(&s_ctx.current);
    SCMD_LoadCommand(&s_ctx.next);
  }
}

static void SCMD_ExecuteSetPriority(void) {
  SetScanPriority(s_ctx.current.priority);
}

static void SCMD_ExecuteSetMode(void) {
  SetScanMode(s_ctx.current.start); // start содержит код режима
}

// Выполнение текущей команды
void SCMD_ExecuteCurrent(void) {
  switch (s_ctx.current.type) {
  case SCMD_CHANNEL:
    SCMD_ExecuteChannel();
    break;

  case SCMD_RANGE:
    SCMD_ExecuteRange();
    break;

  case SCMD_JUMP:
    // Переход уже обработан в Advance
    break;

  case SCMD_CJUMP:
    SCMD_ExecuteCondJump();
    break;

  case SCMD_PAUSE:
    SCMD_ExecutePause();
    break;

  case SCMD_CALL:
    SCMD_ExecuteCall();
    break;

  case SCMD_RETURN:
    SCMD_ExecuteReturn();
    break;

  case SCMD_SETPRIO:
    SCMD_ExecuteSetPriority();
    break;

  case SCMD_SETMODE:
    SCMD_ExecuteSetMode();
    break;
  }
}

// Проверка, нужно ли пропускать команду по приоритету
bool SCMD_ShouldSkip(void) {
  return s_ctx.current.priority < GetCurrentMinPriority();
}

// Получение текущей и следующей команд
SCMD_Command *SCMD_GetCurrent(void) { return &s_ctx.current; }

SCMD_Command *SCMD_GetNext(void) { return s_ctx.has_next ? &s_ctx.next : NULL; }

// Проверка наличия следующей команды
bool SCMD_HasNext(void) { return s_ctx.has_next; }

// Сброс к началу файла
void SCMD_Rewind(void) {
  lfs_file_seek(&gLfs, &s_file, sizeof(SCMD_Header), LFS_SEEK_SET);
  s_ctx.file_pos = sizeof(SCMD_Header);
  s_ctx.cmd_index = 0;
  s_ctx.loop_ptr = 0;
  s_ctx.call_ptr = 0;

  SCMD_LoadCommand(&s_ctx.current);
  SCMD_LoadCommand(&s_ctx.next);
}

// Закрытие файла
void SCMD_Close(void) {
  lfs_file_close(&gLfs, &s_file);
  memset(&s_ctx, 0, sizeof(s_ctx));
}

bool SCMD_CreateFile(const char *filename, SCMD_Command *commands,
                     uint16_t count) {
  uint8_t buffer[256];
  struct lfs_file_config config = {.buffer = buffer, .attr_count = 0};

  lfs_file_t file;
  int err = lfs_file_opencfg(&gLfs, &file, filename,
                             LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC, &config);
  if (err < 0) {
    return false;
  }

  // Заголовок без CRC
  SCMD_Header header = {
      .magic = SCMD_MAGIC,
      .version = SCMD_VERSION,
      .cmd_count = count,
      .entry_point = 0,
      .crc32 = 0xDEADBEEF // Фиктивное значение
  };

  // Пишем заголовок
  lfs_ssize_t written = lfs_file_write(&gLfs, &file, &header, sizeof(header));
  if (written != sizeof(header)) {
    lfs_file_close(&gLfs, &file);
    return false;
  }

  // Пишем команды
  written =
      lfs_file_write(&gLfs, &file, commands, sizeof(SCMD_Command) * count);

  bool success = (written == (lfs_ssize_t)(sizeof(SCMD_Command) * count));

  lfs_file_close(&gLfs, &file);
  return success;
}

uint16_t SCMD_GetCurrentIndex(void) { return s_ctx.cmd_index; }

// Пример создания файла
void SCMD_CreateExampleScan(void) {
  struct lfs_info info;
  if (lfs_stat(&gLfs, "/scans", &info) < 0) {
    lfs_mkdir(&gLfs, "/scans");
  }
  SCMD_Command cmds[] = {
      /* uint8_t type;         ///< Тип команды
      uint8_t priority;     ///< Приоритет выполнения
      uint8_t flags;        ///< Флаги команды
      uint8_t loop_count;   ///< Счетчик циклов
      uint16_t dwell_ms;    ///< Время на точке (мс)
      uint16_t timeout_ms;  ///< Таймаут прослушивания (мс)
      uint16_t goto_offset; ///< Смещение перехода
      uint32_t start; ///< Параметр 1 (частота/значение)
      uint32_t end;   ///< Параметр 2 (частота/значение) */
      // Метка 0: начало
      {SCMD_MARKER, 0, 0, 0, 0, 0, 0, 0, 0},

      // Диапазон
      {SCMD_RANGE, 5, 0, 0, 20, 2000, 0, 144 * MHZ, 176 * MHZ},
      {SCMD_RANGE, 5, 0, 0, 20, 2000, 0, 400 * MHZ, 470 * MHZ},

      // Возврат к началу
      {SCMD_JUMP, 0, 0, 0, 0, 0, 0, 0, 0},
  };

  SCMD_CreateFile("/scans/cmd1.bin", cmds, sizeof(cmds) / sizeof(cmds[0]));
}

void SCMD_DebugDumpFile(const char *filename) {
  uint8_t buffer[256];
  struct lfs_file_config config = {.buffer = buffer, .attr_count = 0};
  lfs_file_t file;

  int err = lfs_file_opencfg(&gLfs, &file, filename, LFS_O_RDONLY, &config);
  if (err < 0) {
    Log("[SCMD] Cannot open %s for debugging", filename);
    return;
  }

  // Читаем заголовок
  SCMD_Header header;
  lfs_file_read(&gLfs, &file, &header, sizeof(header));

  Log("[SCMD] File %s: magic=0x%08X, version=%u, cmd_count=%u", filename,
      header.magic, header.version, header.cmd_count);

  // Читаем и дампим все команды
  for (uint16_t i = 0; i < header.cmd_count; i++) {
    SCMD_Command cmd;
    lfs_file_read(&gLfs, &file, &cmd, sizeof(cmd));

    Log("[SCMD] Cmd %d: type=%d, prio=%d, start=%lu, end=%lu, dwell=%u, "
        "goto=%u",
        i, cmd.type, cmd.priority, cmd.start, cmd.end, cmd.dwell_ms,
        cmd.goto_offset);
  }

  lfs_file_close(&gLfs, &file);
}

uint16_t SCMD_GetCommandCount(void) { return s_ctx.cmd_count; }
