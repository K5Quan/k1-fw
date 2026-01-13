#include "scan.h"
#include "../apps/apps.h"
#include "../driver/st7565.h"
#include "../driver/systick.h"
#include "../driver/uart.h"
#include "../external/printf/printf.h"
#include "../helper/lootlist.h"
#include "../helper/scancommand.h"
#include "../radio.h"
#include "../settings.h"
#include "../ui/spectrum.h"
#include "bands.h"

// ============================================================================
// Состояние сканирования
// ============================================================================

typedef struct {
  uint32_t start;
  uint32_t end;
  uint32_t current;
  uint32_t step;
  bool active;
} ScanRange;

typedef struct {
  ScanMode mode;
  uint32_t scanDelayUs;
  uint32_t stayAtTimeout;
  uint32_t scanListenTimeout;
  uint16_t squelchLevel;

  // Статистика
  uint32_t scanCycles;
  uint32_t lastCpsTime;
  uint32_t currentCps;
  uint32_t cpsUpdateInterval;

  // Состояние
  bool lastListenState;
  bool isMultiband;

  // Единый диапазон для всех режимов
  ScanRange range;

  // Командный режим
  SCMD_Context *cmdCtx;
} ScanState;

static ScanState scan = {
    .mode = SCAN_MODE_SINGLE,
    .scanDelayUs = 1200,
    .squelchLevel = 0,
    .lastListenState = false,
    .isMultiband = false,
    .scanCycles = 0,
    .lastCpsTime = 0,
    .currentCps = 0,
    .cpsUpdateInterval = 1000,
    .cmdCtx = NULL,
};

// ============================================================================
// Вспомогательные функции
// ============================================================================

static void UpdateCPS(void) {
  uint32_t now = Now();
  uint32_t elapsed = now - scan.lastCpsTime;

  if (elapsed >= scan.cpsUpdateInterval) {
    if (elapsed > 0) {
      scan.currentCps = (scan.scanCycles * 1000) / elapsed;
    }
    scan.lastCpsTime = now;
    scan.scanCycles = 0;
  }
}

static uint16_t MeasureSignal(uint32_t frequency, bool precise) {
  RADIO_SetParam(ctx, PARAM_PRECISE_F_CHANGE, precise, false);
  RADIO_SetParam(ctx, PARAM_FREQUENCY, frequency, false);
  RADIO_ApplySettings(ctx);
  SYSTICK_DelayUs(precise ? scan.scanDelayUs : 50);
  return RADIO_GetRSSI(ctx);
}

static void ApplyBandSettings(void) {
  vfo->msm.f = gCurrentBand.start;
  RADIO_SetParam(ctx, PARAM_FREQUENCY, vfo->msm.f, false);
  RADIO_SetParam(ctx, PARAM_STEP, gCurrentBand.step, false);
  RADIO_ApplySettings(ctx);
  SP_Init(&gCurrentBand);

  LogC(LOG_C_BRIGHT_YELLOW, "[SCAN] Bounds: %u .. %u", gCurrentBand.start,
       gCurrentBand.end);

  if (gLastActiveLoot && !BANDS_InRange(gLastActiveLoot->f, &gCurrentBand)) {
    gLastActiveLoot = NULL;
  }
}

static void SetScanRange(uint32_t start, uint32_t end, uint32_t step) {
  scan.range.start = start;
  scan.range.end = end;
  scan.range.current = start;
  scan.range.step = step;
  scan.range.active = true;

  vfo->msm.f = start;
  LOOT_Replace(&vfo->msm, start);
  SetTimeout(&scan.scanListenTimeout, 0);
  SetTimeout(&scan.stayAtTimeout, 0);

  Log("[SCAN] Range set: %u-%u Hz, step=%u", start, end, step);
}

// ============================================================================
// Переход к следующей частоте - единая функция для всех режимов
// ============================================================================

static void NextFrequency(void) {
  if (!scan.range.active)
    return;

  if (vfo->is_open) {
    vfo->is_open = false;
    RADIO_SwitchAudioToVFO(gRadioState, gRadioState->active_vfo_index);
  }

  uint32_t step = scan.range.step;
  if (step == 0) {
    step = StepFrequencyTable[RADIO_GetParam(ctx, PARAM_STEP)];
  }

  scan.range.current += step;

  // Проверка границ
  if (scan.range.current > scan.range.end) {
    if (scan.cmdCtx) {
      // В командном режиме - следующая команда
      if (!SCMD_Advance(scan.cmdCtx)) {
        SCMD_Rewind(scan.cmdCtx);
        Log("[SCAN] Command sequence restarted");
      }
      scan.range.active = false; // Следующая команда установит новый диапазон
      gRedrawScreen = true;
      return;
    } else {
      // В обычном режиме - возврат к началу
      if (scan.isMultiband) {
        ApplyBandSettings();
      }
      scan.range.current = scan.range.start;
      gRedrawScreen = true;
    }
  }

  vfo->msm.f = scan.range.current;
  LOOT_Replace(&vfo->msm, vfo->msm.f);
  SetTimeout(&scan.scanListenTimeout, 0);
  SetTimeout(&scan.stayAtTimeout, 0);
  scan.scanCycles++;
  UpdateCPS();
}

static void UpdateSquelchAndRssi(bool isAnalyserMode) {
  Loot *msm = LOOT_Get(vfo->msm.f);

  // Пропускаем мусорные частоты и black/white list
  if ((gSettings.skipGarbageFrequencies &&
       (vfo->msm.f % GARBAGE_FREQUENCY_MOD == 0)) ||
      (msm && (msm->blacklist || msm->whitelist))) {
    vfo->msm.open = false;
    vfo->msm.rssi = 0;
    SP_AddPoint(&vfo->msm);
    NextFrequency();
    return;
  }

  vfo->msm.rssi = MeasureSignal(vfo->msm.f, !isAnalyserMode);
  scan.scanCycles++;

  // Автоматическая подстройка squelch
  if (!scan.squelchLevel && vfo->msm.rssi) {
    scan.squelchLevel = vfo->msm.rssi - 1;
  }

  if (scan.squelchLevel > vfo->msm.rssi) {
    uint16_t perc = (scan.squelchLevel - vfo->msm.rssi) * 100 /
                    ((scan.squelchLevel + vfo->msm.rssi) / 2);
    if (perc >= 25) {
      scan.squelchLevel = vfo->msm.rssi - 1;
    }
  }

  vfo->msm.open = vfo->msm.rssi >= scan.squelchLevel;
  SP_AddPoint(&vfo->msm);
}

// ============================================================================
// Применение команды - только установка параметров
// ============================================================================

static void ApplyCommand(SCMD_Command *cmd) {
  if (!cmd) {
    Log("[SCAN] No command to apply");
    return;
  }

  Log("[SCAN] Applying command: type=%d, start=%lu, end=%lu", cmd->type,
      cmd->start, cmd->end);

  switch (cmd->type) {
  case SCMD_CHANNEL:
    // Одиночный канал = диапазон из одной точки
    SetScanRange(cmd->start, cmd->start, 0);
    break;

  case SCMD_RANGE: {
    uint32_t step = cmd->step;
    if (step == 0) {
      step = StepFrequencyTable[gCurrentBand.step];
    }
    SetScanRange(cmd->start, cmd->end, step);
    break;
  }

  case SCMD_PAUSE:
    Log("[SCAN] Pause %u ms", cmd->dwell_ms);
    SYSTICK_DelayMs(cmd->dwell_ms);
    if (!SCMD_Advance(scan.cmdCtx)) {
      SCMD_Rewind(scan.cmdCtx);
    }
    break;

  case SCMD_JUMP:
    // GOTO уже обработан в SCMD_Advance
    Log("[SCAN] Jump executed");
    break;

  case SCMD_MARKER:
    // Метки игнорируем
    if (!SCMD_Advance(scan.cmdCtx)) {
      SCMD_Rewind(scan.cmdCtx);
    }
    break;

  default:
    Log("[SCAN] Unknown command type: %d", cmd->type);
    if (!SCMD_Advance(scan.cmdCtx)) {
      SCMD_Rewind(scan.cmdCtx);
    }
    break;
  }
}

// ============================================================================
// Главная функция сканирования
// ============================================================================

void SCAN_Check(void) {
  RADIO_UpdateMultiwatch(gRadioState);

  // В командном режиме применяем команду если нужно
  if (scan.cmdCtx && !scan.range.active) {
    SCMD_Command *cmd = SCMD_GetCurrent(scan.cmdCtx);
    if (cmd) {
      ApplyCommand(cmd);
    }
    return;
  }

  // Режим анализатора - быстрое сканирование
  if (scan.mode == SCAN_MODE_ANALYSER) {
    vfo->msm.rssi = MeasureSignal(vfo->msm.f, false);
    SP_AddPoint(&vfo->msm);
    NextFrequency();
    return;
  }

  // Одиночная частота - только мониторинг
  if (scan.mode == SCAN_MODE_SINGLE) {
    RADIO_UpdateSquelch(gRadioState);
    vfo->msm.rssi = MeasureSignal(vfo->msm.f, true);
    vfo->msm.open = vfo->is_open;

    if (scan.lastListenState != vfo->is_open) {
      scan.lastListenState = vfo->is_open;
      gRedrawScreen = true;
    }

    static uint32_t radioTimer;
    RADIO_CheckAndSaveVFO(gRadioState);
    if (Now() - radioTimer >= SQL_DELAY) {
      RADIO_UpdateSquelch(gRadioState);
      SP_ShiftGraph(-1);
      SP_AddGraphPoint(&vfo->msm);
      radioTimer = Now();
    }
    return;
  }

  // ========================================================================
  // ОБЩАЯ ЛОГИКА СКАНИРОВАНИЯ (частотный и канальный режимы)
  // ========================================================================

  if (vfo->msm.open) {
    RADIO_UpdateSquelch(gRadioState);
    vfo->msm.open = vfo->is_open;
    gRedrawScreen = true;
  } else {
    UpdateSquelchAndRssi(false);
  }

  // Проверка на "думание" о squelch
  if (vfo->msm.open && !vfo->is_open) {
    SYSTICK_DelayMs(SQL_DELAY);
    RADIO_UpdateSquelch(gRadioState);
    vfo->msm.open = vfo->is_open;

    if (!vfo->msm.open) {
      scan.squelchLevel++;
    }
  }

  LOOT_Update(&vfo->msm);

  // Автокоррекция squelch
  if (vfo->is_open && !vfo->msm.open) {
    scan.squelchLevel = SP_GetNoiseFloor();
  }

  // Логика таймаутов
  if (scan.lastListenState != vfo->is_open) {
    scan.lastListenState = vfo->is_open;
    gRedrawScreen = true;

    if (vfo->is_open) {
      SetTimeout(&scan.scanListenTimeout,
                 SCAN_TIMEOUTS[gSettings.sqOpenedTimeout]);
      SetTimeout(&scan.stayAtTimeout, UINT32_MAX);

      // В командном режиме - проверяем флаги
      if (scan.cmdCtx) {
        SCMD_Command *cmd = SCMD_GetCurrent(scan.cmdCtx);
        if (cmd && (cmd->flags & SCMD_FLAG_AUTO_WHITELIST)) {
          // LOOT_Whitelist(&vfo->msm);
          Log("[SCAN] Auto-whitelisted (NO) %u Hz", vfo->msm.f);
        }
      }
    } else {
      SetTimeout(&scan.stayAtTimeout, SCAN_TIMEOUTS[gSettings.sqClosedTimeout]);
    }
  }

  // Проверка таймаутов и переход
  if ((CheckTimeout(&scan.scanListenTimeout) && vfo->is_open) ||
      CheckTimeout(&scan.stayAtTimeout)) {
    NextFrequency();
  }
}

// ============================================================================
// API функций
// ============================================================================

const char *SCAN_MODE_NAMES[] = {
    [SCAN_MODE_SINGLE] = "VFO",
    [SCAN_MODE_FREQUENCY] = "Scan",
    [SCAN_MODE_CHANNEL] = "CH Scan",
    [SCAN_MODE_ANALYSER] = "Band scan",
};

SCMD_Context cmdctx;

void SCAN_LoadCommandFile(const char *filename) {
  if (!scan.cmdCtx) {
    scan.cmdCtx = &cmdctx;
    if (!scan.cmdCtx) {
      Log("[SCAN] Failed to allocate command context");
      return;
    }
    memset(scan.cmdCtx, 0, sizeof(SCMD_Context));
  }

  SCMD_DebugDumpFile(filename);

  if (SCMD_Init(scan.cmdCtx, filename)) {
    scan.mode = SCAN_MODE_FREQUENCY;
    scan.range.active = false; // Команда установит диапазон
    Log("[SCAN] Loaded command file: %s", filename);
  } else {
    scan.cmdCtx = NULL;
    Log("[SCAN] Failed to load: %s", filename);
  }
}

void SCAN_SetCommandMode(bool enabled) {
  if (!enabled && scan.cmdCtx) {
    SCMD_Close(scan.cmdCtx);
    scan.cmdCtx = NULL;
    scan.range.active = false;
    Log("[SCAN] Command mode disabled");
  }
}

bool SCAN_IsCommandMode(void) { return scan.cmdCtx != NULL; }

void SCAN_CommandForceNext(void) {
  if (!scan.cmdCtx)
    return;

  Log("[SCAN] Force advancing to next command");
  if (!SCMD_Advance(scan.cmdCtx)) {
    SCMD_Rewind(scan.cmdCtx);
    Log("[SCAN] Restarted command sequence");
  }

  scan.range.active = false;
  SetTimeout(&scan.scanListenTimeout, 0);
  SetTimeout(&scan.stayAtTimeout, 0);
  scan.lastListenState = false;
  gRedrawScreen = true;
}

void SCAN_SetMode(ScanMode mode) {
  if (scan.cmdCtx && mode != scan.mode) {
    SCAN_SetCommandMode(false);
  }

  scan.mode = mode;
  Log("[SCAN] mode=%s", SCAN_MODE_NAMES[scan.mode]);

  scan.scanCycles = 0;
  scan.squelchLevel = 0;
  SetTimeout(&scan.stayAtTimeout, 0);
  SetTimeout(&scan.scanListenTimeout, 0);

  switch (mode) {
  case SCAN_MODE_SINGLE:
    scan.range.active = false;
    break;

  case SCAN_MODE_FREQUENCY:
  case SCAN_MODE_ANALYSER:
    ApplyBandSettings();
    SetScanRange(gCurrentBand.start, gCurrentBand.end,
                 StepFrequencyTable[gCurrentBand.step]);
    break;

  case SCAN_MODE_CHANNEL:
    // TODO: channel mode
    break;
  }
}

ScanMode SCAN_GetMode(void) { return scan.mode; }

uint32_t SCAN_GetCps(void) { return scan.currentCps; }

void SCAN_setBand(Band b) {
  gCurrentBand = b;
  ApplyBandSettings();
  if (scan.mode == SCAN_MODE_FREQUENCY || scan.mode == SCAN_MODE_ANALYSER) {
    SetScanRange(gCurrentBand.start, gCurrentBand.end,
                 StepFrequencyTable[gCurrentBand.step]);
  }
}

void SCAN_setStartF(uint32_t f) {
  gCurrentBand.start = f;
  ApplyBandSettings();
  if (scan.mode == SCAN_MODE_FREQUENCY || scan.mode == SCAN_MODE_ANALYSER) {
    SetScanRange(f, gCurrentBand.end, StepFrequencyTable[gCurrentBand.step]);
  }
}

void SCAN_setEndF(uint32_t f) {
  gCurrentBand.end = f;
  ApplyBandSettings();
  if (scan.mode == SCAN_MODE_FREQUENCY || scan.mode == SCAN_MODE_ANALYSER) {
    SetScanRange(gCurrentBand.start, f, StepFrequencyTable[gCurrentBand.step]);
  }
}

void SCAN_setRange(uint32_t fs, uint32_t fe) {
  gCurrentBand.start = fs;
  gCurrentBand.end = fe;
  ApplyBandSettings();
  if (scan.mode == SCAN_MODE_FREQUENCY || scan.mode == SCAN_MODE_ANALYSER) {
    SetScanRange(fs, fe, StepFrequencyTable[gCurrentBand.step]);
  }
}

void SCAN_Next(void) { NextFrequency(); }

void SCAN_NextBlacklist(void) {
  LOOT_BlacklistLast();
  SCAN_Next();
}

void SCAN_NextWhitelist(void) {
  LOOT_WhitelistLast();
  SCAN_Next();
}

void SCAN_Init(bool multiband) {
  scan.isMultiband = multiband;
  vfo->msm.snr = 0;
  scan.lastCpsTime = Now();
  scan.scanCycles = 0;
  scan.currentCps = 0;

  ApplyBandSettings();
  BK4819_WriteRegister(BK4819_REG_3F, 0);
}

void SCAN_SetDelay(uint32_t delay) { scan.scanDelayUs = delay; }

uint32_t SCAN_GetDelay(void) { return scan.scanDelayUs; }

SCMD_Command *SCAN_GetCurrentCommand(void) {
  return scan.cmdCtx ? SCMD_GetCurrent(scan.cmdCtx) : NULL;
}

SCMD_Command *SCAN_GetNextCommand(void) {
  return scan.cmdCtx ? SCMD_GetNext(scan.cmdCtx) : NULL;
}

uint16_t SCAN_GetCommandIndex(void) {
  return scan.cmdCtx ? SCMD_GetCurrentIndex(scan.cmdCtx) : 0;
}

uint16_t SCAN_GetCommandCount(void) {
  return scan.cmdCtx ? SCMD_GetCommandCount(scan.cmdCtx) : 0;
}
