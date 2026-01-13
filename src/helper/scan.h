// ============================================================================
// scan.h - обновленный заголовочный файл
// ============================================================================

#ifndef SCAN_H
#define SCAN_H

#include "../inc/band.h"
#include "scancommand.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum {
  SCAN_MODE_SINGLE,
  SCAN_MODE_FREQUENCY,
  SCAN_MODE_CHANNEL,
  SCAN_MODE_ANALYSER,
} ScanMode;

void SCAN_Init(bool multiband);
void SCAN_Check(void);
void SCAN_SetMode(ScanMode mode);
ScanMode SCAN_GetMode(void);
uint32_t SCAN_GetCps(void);

// Управление диапазоном
void SCAN_setBand(Band b);
void SCAN_setStartF(uint32_t f);
void SCAN_setEndF(uint32_t f);
void SCAN_setRange(uint32_t fs, uint32_t fe);

// Навигация
void SCAN_Next(void);
void SCAN_NextBlacklist(void);
void SCAN_NextWhitelist(void);

// Задержки
void SCAN_SetDelay(uint32_t delay);
uint32_t SCAN_GetDelay(void);

// Командный режим
void SCAN_LoadCommandFile(const char *filename);
void SCAN_SetCommandMode(bool enabled);
bool SCAN_IsCommandMode(void);
void SCAN_CommandForceNext(void);

SCMD_Command *SCAN_GetCurrentCommand(void);
SCMD_Command *SCAN_GetNextCommand(void);
uint16_t SCAN_GetCommandIndex(void);
uint16_t SCAN_GetCommandCount(void);

#endif
