#ifndef SCANCOMMAND_H
#define SCANCOMMAND_H

#include "../driver/lfs.h"
#include <stdbool.h>
#include <stdint.h>

#define SCMD_MAGIC 0x53434D44 // "SCMD"
#define SCMD_VERSION 1

// Типы команд
typedef enum {
  SCMD_NOP = 0,
  SCMD_CHANNEL, // Одиночный канал
  SCMD_RANGE,   // Диапазон частот
  SCMD_JUMP,    // Безусловный переход
  SCMD_CJUMP,   // Условный переход (если сигнал)
  SCMD_PAUSE,   // Пауза
  SCMD_CALL,    // Вызов подпрограммы
  SCMD_RETURN,  // Возврат из подпрограммы
  SCMD_MARKER,  // Метка для переходов
  SCMD_SETPRIO, // Установка приоритета
  SCMD_SETMODE, // Установка режима
} SCMD_Type;

// Флаги команд
#define SCMD_FLAG_IGNORE_BLACK (1 << 0) // Игнорировать черный список
#define SCMD_FLAG_AUTO_WHITELIST (1 << 1) // Автодобавление в белый список
#define SCMD_FLAG_ONCE (1 << 2) // Выполнить только раз

// Структура команды
typedef struct {
  uint8_t type;         // Тип команды
  uint8_t priority;     // Приоритет
  uint8_t flags;        // Флаги
  uint8_t reserved;     // Зарезервировано
  uint16_t dwell_ms;    // Время на точке (мс)
  uint16_t timeout_ms;  // Таймаут
  uint16_t goto_offset; // Смещение для перехода
  uint16_t step;        // Шаг (для RANGE)
  uint32_t start;       // Начальная частота
  uint32_t end;         // Конечная частота
} __attribute__((packed)) SCMD_Command;

// Заголовок файла
typedef struct {
  uint32_t magic;
  uint16_t version;
  uint16_t cmd_count;
  uint16_t entry_point;
  uint16_t reserved;
  uint32_t crc32;
} __attribute__((packed)) SCMD_Header;

// Контекст выполнения
typedef struct {
  lfs_file_t file;
  uint8_t file_buffer[256];

  SCMD_Command current;
  SCMD_Command next;

  uint32_t file_pos;
  uint16_t cmd_index;
  uint16_t cmd_count;

  bool has_next;

  // Стеки для CALL/RETURN
  uint32_t call_stack[4];
  uint8_t call_ptr;
} SCMD_Context;

// API
bool SCMD_Init(SCMD_Context *ctx, const char *filename);
void SCMD_Close(SCMD_Context *ctx);
bool SCMD_Advance(SCMD_Context *ctx);
void SCMD_Rewind(SCMD_Context *ctx);

SCMD_Command *SCMD_GetCurrent(SCMD_Context *ctx);
SCMD_Command *SCMD_GetNext(SCMD_Context *ctx);
bool SCMD_HasNext(SCMD_Context *ctx);

uint16_t SCMD_GetCurrentIndex(SCMD_Context *ctx);
uint16_t SCMD_GetCommandCount(SCMD_Context *ctx);

// Утилиты
bool SCMD_CreateFile(const char *filename, SCMD_Command *commands,
                     uint16_t count);
void SCMD_CreateExampleScan(void);
void SCMD_DebugDumpFile(const char *filename);

#endif
