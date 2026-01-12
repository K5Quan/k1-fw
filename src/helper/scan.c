#include "scan.h"
#include "../apps/apps.h"
#include "../driver/st7565.h"
#include "../driver/systick.h"
#include "../driver/uart.h"
#include "../helper/lootlist.h"
#include "../helper/scancommand.h"
#include "../radio.h"
#include "../settings.h"
#include "../ui/spectrum.h"
#include "bands.h"

// =============================
// Состояние сканирования
// =============================
typedef struct {
  ScanMode mode;
  uint32_t scanDelayUs; // Задержка измерения (микросек)
  uint32_t stayAtTimeout; // Таймаут удержания на частоте
  uint32_t scanListenTimeout; // Таймаут прослушивания
  uint32_t scanCycles; // Количество циклов сканирования
  uint32_t lastCpsTime; // Последнее время замера CPS
  uint32_t currentCps; // Текущее значение CPS (кешированное)
  uint32_t cpsUpdateInterval; // Интервал обновления CPS (мс)
  uint16_t squelchLevel; // Текущий уровень шумоподавления

  bool thinking;           // Думоем
  bool wasThinkingEarlier; // Флаг для корректировки squelch
  bool lastListenState;    // Последнее состояние squelch
  bool isMultiband;        // Мультидиапазонный режим

  // Новые поля для SCMD
  bool commandMode;   // Режим выполнения команд
  bool commandPaused; // Пауза выполнения команд
  uint32_t cmdLastExec; // Время последнего выполнения команды
  SCMD_Context cmdCtx; // Контекст SCMD (если впишется в RAM)
} ScanState;

static ScanState scan = {
    .mode = SCAN_MODE_SINGLE,
    .scanDelayUs = 1200,
    .squelchLevel = 0,
    .thinking = false,
    .wasThinkingEarlier = false,
    .lastListenState = false,
    .isMultiband = false,
    .stayAtTimeout = 0,
    .scanListenTimeout = 0,
    .scanCycles = 0,
    .lastCpsTime = 0,
    .currentCps = 0,
    .cpsUpdateInterval = 1000,

    .commandMode = false,
    .commandPaused = false,
    .cmdLastExec = 0,
};

// =============================
// Вспомогательные функции
// =============================

static void UpdateCPS() {
  uint32_t now = Now();
  uint32_t elapsed = now - scan.lastCpsTime;

  // Обновляем CPS только если прошел достаточный интервал
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

static void ApplyBandSettings() {
  vfo->msm.f = gCurrentBand.start;

  RADIO_SetParam(ctx, PARAM_FREQUENCY, vfo->msm.f, false);
  RADIO_SetParam(ctx, PARAM_STEP, gCurrentBand.step, false);
  RADIO_ApplySettings(ctx);
  SP_Init(&gCurrentBand);
  LogC(LOG_C_BRIGHT_YELLOW, "[SCANER] Bounds: %u .. %u", gCurrentBand.start,
       gCurrentBand.end);
  if (gLastActiveLoot && !BANDS_InRange(gLastActiveLoot->f, &gCurrentBand)) {
    gLastActiveLoot = NULL;
  }
}

// В scan.c, обновляем NextFrequency() для работы с командным режимом
static void NextFrequency(void) {
  // TODO: priority cooldown scan
  uint32_t step = StepFrequencyTable[RADIO_GetParam(ctx, PARAM_STEP)];
  vfo->msm.f += step;
  if (vfo->is_open) {
    vfo->is_open = false;
    RADIO_SwitchAudioToVFO(gRadioState, gRadioState->active_vfo_index);
  }

  // В командном режиме используем диапазон из текущей команды
  if (scan.commandMode) {
    SCMD_Command *cmd = SCMD_GetCurrent();
    if (cmd && (cmd->type == SCMD_RANGE || cmd->type == SCMD_CHANNEL)) {
      // Для команд диапазона проверяем границы
      if (cmd->type == SCMD_RANGE) {
        if (vfo->msm.f > cmd->end) {
          vfo->msm.f = cmd->start;
          gRedrawScreen = true;
        }
      }
      // Для одиночного канала остаемся на той же частоте
    } else {
      // Для других команд используем стандартную логику
      if (vfo->msm.f > gCurrentBand.end) {
        if (scan.isMultiband) {
          ApplyBandSettings();
        }
        vfo->msm.f = gCurrentBand.start;
        gRedrawScreen = true;
      }
    }
  } else {
    // Стандартная логика для некомандного режима
    if (vfo->msm.f > gCurrentBand.end) {
      if (scan.isMultiband) {
        ApplyBandSettings();
      }
      vfo->msm.f = gCurrentBand.start;
      gRedrawScreen = true;
    }
  }

  LOOT_Replace(&vfo->msm, vfo->msm.f);
  SetTimeout(&scan.scanListenTimeout, 0);
  SetTimeout(&scan.stayAtTimeout, 0);
  UpdateCPS();
}

static void UpdateSquelchAndRssi(bool isAnalyserMode) {
  Loot *msm = LOOT_Get(vfo->msm.f);
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

void SCAN_CommandForceNext(void) {
  if (!scan.commandMode)
    return;

  Log("[SCAN] Force advancing to next command");
  if (!SCMD_Advance()) {
    SCMD_Rewind();
    Log("[SCAN] Restarted command sequence");
  }

  // Сбрасываем состояние
  SetTimeout(&scan.scanListenTimeout, 0);
  SetTimeout(&scan.stayAtTimeout, 0);
  scan.lastListenState = false;
  gRedrawScreen = true;
}

static void NextStep() {
  switch (scan.mode) {
  case SCAN_MODE_SINGLE:
    // Остаемся на текущей частоте
    // Только обновляем squelch
    break;

  case SCAN_MODE_CHANNEL:
    // Переход к следующему каналу
    /* CHANNELS_Next(true);
    CHANNELS_LoadCurrentScanlistCH();
    LOOT_Replace(&vfo->msm, vfo->msm.f); */
    break;

  case SCAN_MODE_FREQUENCY:
  case SCAN_MODE_ANALYSER:
    // Переход к следующей частоте (шаг)
    NextFrequency();
    break;
  }

  SetTimeout(&scan.scanListenTimeout, 0);
  SetTimeout(&scan.stayAtTimeout, 0);
  scan.scanCycles++;
  UpdateCPS();
}

static void NextWithTimeout() {
  if (scan.lastListenState != vfo->is_open) {
    scan.lastListenState = vfo->is_open;
    gRedrawScreen = true;

    if (vfo->is_open) {
      SetTimeout(&scan.scanListenTimeout,
                 SCAN_TIMEOUTS[gSettings.sqOpenedTimeout]);
      SetTimeout(&scan.stayAtTimeout, UINT32_MAX);
    } else {
      SetTimeout(&scan.stayAtTimeout, SCAN_TIMEOUTS[gSettings.sqClosedTimeout]);
    }
  }

  if ((CheckTimeout(&scan.scanListenTimeout) && vfo->is_open) ||
      CheckTimeout(&scan.stayAtTimeout)) {
    NextFrequency();
  }
}

// =============================
// API функций
// =============================
const char *SCAN_MODE_NAMES[] = {
    [SCAN_MODE_SINGLE] = "VFO",
    [SCAN_MODE_FREQUENCY] = "Scan",
    [SCAN_MODE_CHANNEL] = "CH Scan",
    [SCAN_MODE_ANALYSER] = "Band scan",
};

// =====
// Новые функции для SCMD:
void SCAN_LoadCommandFile(const char *filename) {
  // Сначала проверяем файл
  SCMD_DebugDumpFile(filename);

  if (SCMD_Init(filename)) {
    scan.commandMode = true;
    scan.mode = SCAN_MODE_SINGLE; // Переключаем в VFO режим
    Log("[SCAN] Loaded command file: %s", filename);

    // Дамп первой команды
    SCMD_Command *cmd = SCMD_GetCurrent();
    if (cmd) {
      Log("[SCAN] First command: type=%d, start=%lu", cmd->type, cmd->start);
    }
  } else {
    Log("[SCAN] Failed to load: %s", filename);
  }
}

void SCAN_SetCommandMode(bool enabled) {
  scan.commandMode = enabled;
  if (!enabled) {
    SCMD_Close();
  }
}

bool SCAN_IsCommandMode(void) { return scan.commandMode; }

void SCAN_CommandNext(void) {
  if (!scan.commandMode)
    return;

  SCMD_Command *cmd = SCMD_GetCurrent();
  if (cmd && !SCMD_ShouldSkip()) {
    SCMD_ExecuteCurrent();
  }

  if (!SCMD_Advance()) {
    SCMD_Rewind(); // Циклическое выполнение
  }
}

void SCAN_CommandRewind(void) { SCMD_Rewind(); }

// Добавим функцию для дампа команды
static void DumpCommand(SCMD_Command *cmd, const char *context) {
  Log("[SCAN] %s: type=%d, prio=%d, flags=0x%02X, start=%lu, end=%lu, "
      "dwell=%u, goto=%u",
      context, cmd->type, cmd->priority, cmd->flags, cmd->start, cmd->end,
      cmd->dwell_ms, cmd->goto_offset);
}
// В scan.c, новая функция для командного сканирования с использованием
// существующей логики
static void SCAN_ExecuteCommandWithLogic(void) {
  SCMD_Command *cmd = SCMD_GetCurrent();
  if (!cmd)
    return;

  static enum {
    CMD_STATE_START,
    CMD_STATE_SCANNING,
    CMD_STATE_WAITING,
    CMD_STATE_COMPLETE
  } cmd_state = CMD_STATE_START;

  static uint32_t scan_start_freq = 0;
  static uint32_t scan_end_freq = 0;
  static uint32_t current_freq = 0;
  static uint32_t step = 0;
  static uint32_t dwell_start = 0;

  switch (cmd->type) {
  case SCMD_RANGE:
    switch (cmd_state) {
    case CMD_STATE_START:
      // Инициализация сканирования диапазона
      scan_start_freq = cmd->start;
      scan_end_freq = cmd->end;
      current_freq = scan_start_freq;
      step = gCurrentBand.step;
      if (step == 0)
        step = 25000; // По умолчанию 25 kHz

      // Сохраняем исходные настройки
      static uint32_t original_start, original_end;
      original_start = gCurrentBand.start;
      original_end = gCurrentBand.end;

      // Устанавливаем диапазон для сканирования
      gCurrentBand.start = scan_start_freq;
      gCurrentBand.end = scan_end_freq;

      // Устанавливаем начальную частоту
      vfo->msm.f = current_freq;
      RADIO_SetParam(ctx, PARAM_FREQUENCY, current_freq, false);
      RADIO_ApplySettings(ctx);

      // Сбрасываем таймауты
      SetTimeout(&scan.scanListenTimeout, 0);
      SetTimeout(&scan.stayAtTimeout, 0);

      cmd_state = CMD_STATE_SCANNING;
      Log("[SCAN] Starting range scan: %lu-%lu Hz, step=%lu", scan_start_freq,
          scan_end_freq, step);
      break;

    case CMD_STATE_SCANNING:
      // Используем существующую логику сканирования из SCAN_Check()

      // Если частота вышла за пределы диапазона - завершаем
      if (current_freq > scan_end_freq) {
        cmd_state = CMD_STATE_COMPLETE;
        Log("[SCAN] Range scan complete");
        break;
      }

      // Устанавливаем текущую частоту
      vfo->msm.f = current_freq;
      RADIO_SetParam(ctx, PARAM_FREQUENCY, current_freq, false);
      RADIO_ApplySettings(ctx);

      // Даем время на установление частоты
      SYSTICK_DelayUs(scan.scanDelayUs);

      // Измеряем RSSI и проверяем squelch (как в обычном сканировании)
      UpdateSquelchAndRssi(false); // false = не режим анализатора

      // Обновляем экран
      gRedrawScreen = true;
      SP_AddPoint(&vfo->msm);

      // Проверяем таймауты (как в NextWithTimeout())
      if (scan.lastListenState != vfo->is_open) {
        scan.lastListenState = vfo->is_open;
        gRedrawScreen = true;

        if (vfo->is_open) {
          SetTimeout(&scan.scanListenTimeout,
                     SCAN_TIMEOUTS[gSettings.sqOpenedTimeout]);
          SetTimeout(&scan.stayAtTimeout, UINT32_MAX);

          // Сигнал обнаружен - переходим в режим ожидания
          cmd_state = CMD_STATE_WAITING;
          dwell_start = Now();
          Log("[SCAN] Signal detected at %lu Hz, waiting...", current_freq);
        } else {
          SetTimeout(&scan.stayAtTimeout,
                     SCAN_TIMEOUTS[gSettings.sqClosedTimeout]);
        }
      }

      // Проверяем, нужно ли переходить к следующей частоте
      if ((CheckTimeout(&scan.scanListenTimeout) && vfo->is_open) ||
          CheckTimeout(&scan.stayAtTimeout)) {

        // Переходим к следующей частоте
        current_freq += step;
        scan.scanCycles++;
        UpdateCPS();

        // Сбрасываем таймауты
        SetTimeout(&scan.scanListenTimeout, 0);
        SetTimeout(&scan.stayAtTimeout, 0);

        // Логируем прогресс каждые 10 точек
        static uint32_t last_log = 0;
        if ((current_freq - scan_start_freq) % (10 * step) == 0) {
          uint32_t progress = ((current_freq - scan_start_freq) * 100) /
                              (scan_end_freq - scan_start_freq);
          Log("[SCAN] Progress: %lu%% (%lu/%lu Hz)", progress, current_freq,
              scan_end_freq);
        }
      }
      break;

    case CMD_STATE_WAITING:
      // Ожидание на частоте с сигналом
      if (vfo->is_open) {
        // Сигнал все еще есть
        uint32_t elapsed = Now() - dwell_start;

        // Обновляем RSSI периодически
        static uint32_t last_rssi_update = 0;
        if (Now() - last_rssi_update > 100) {
          vfo->msm.rssi = MeasureSignal(current_freq, true);
          last_rssi_update = Now();
          gRedrawScreen = true;
        }

        // Проверяем, прошло ли время dwell
        if (cmd->dwell_ms > 0 && elapsed >= cmd->dwell_ms) {
          // Время ожидания истекло - идем дальше
          Log("[SCAN] Dwell time completed at %lu Hz", current_freq);

          // Автоматически добавляем в белый список если включен флаг
          if ((cmd->flags & SCMD_FLAG_AUTO_WHITELIST) && vfo->is_open) {
            AddToWhitelist(current_freq);
            Log("[SCAN] Added %lu Hz to whitelist", current_freq);
          }

          // Переходим к следующей частоте
          current_freq += step;
          cmd_state = CMD_STATE_SCANNING;

          // Сбрасываем таймауты
          SetTimeout(&scan.scanListenTimeout, 0);
          SetTimeout(&scan.stayAtTimeout,
                     SCAN_TIMEOUTS[gSettings.sqClosedTimeout]);
        }
      } else {
        // Сигнал пропал - идем дальше
        Log("[SCAN] Signal lost at %lu Hz", current_freq);
        current_freq += step;
        cmd_state = CMD_STATE_SCANNING;

        // Сбрасываем таймауты
        SetTimeout(&scan.scanListenTimeout, 0);
        SetTimeout(&scan.stayAtTimeout,
                   SCAN_TIMEOUTS[gSettings.sqClosedTimeout]);
      }
      break;

    case CMD_STATE_COMPLETE:
      // Восстанавливаем исходный диапазон
      gCurrentBand.start = original_start;
      gCurrentBand.end = original_end;

      // Завершаем команду
      if (!SCMD_Advance()) {
        SCMD_Rewind();
      }
      cmd_state = CMD_STATE_START;
      break;
    }
    break;

  case SCMD_CHANNEL:
    // Аналогичная логика для одиночного канала
    switch (cmd_state) {
    case CMD_STATE_START:
      if (cmd->start == 0) {
        // Пропускаем некорректную команду
        if (!SCMD_Advance())
          SCMD_Rewind();
        return;
      }

      current_freq = cmd->start;

      // Устанавливаем частоту
      vfo->msm.f = current_freq;
      RADIO_SetParam(ctx, PARAM_FREQUENCY, current_freq, false);
      RADIO_ApplySettings(ctx);

      // Даем время на установление
      SYSTICK_DelayUs(scan.scanDelayUs);

      // Сбрасываем таймауты
      SetTimeout(&scan.scanListenTimeout, 0);
      SetTimeout(&scan.stayAtTimeout, 0);

      cmd_state = CMD_STATE_WAITING;
      dwell_start = Now();
      Log("[SCAN] Monitoring channel %lu Hz, dwell=%u ms", current_freq,
          cmd->dwell_ms);
      break;

    case CMD_STATE_WAITING:
      // Обновляем RSSI и проверяем squelch
      vfo->msm.rssi = MeasureSignal(current_freq, true);
      vfo->msm.open = vfo->msm.rssi >= scan.squelchLevel;

      // Обновляем экран
      gRedrawScreen = true;
      SP_AddPoint(&vfo->msm);

      // Проверяем таймауты (как в обычном сканировании)
      if (scan.lastListenState != vfo->is_open) {
        scan.lastListenState = vfo->is_open;
        gRedrawScreen = true;

        if (vfo->is_open) {
          SetTimeout(&scan.scanListenTimeout,
                     SCAN_TIMEOUTS[gSettings.sqOpenedTimeout]);
          SetTimeout(&scan.stayAtTimeout, UINT32_MAX);
          Log("[SCAN] Signal detected on channel %lu Hz", current_freq);
        } else {
          SetTimeout(&scan.stayAtTimeout,
                     SCAN_TIMEOUTS[gSettings.sqClosedTimeout]);
        }
      }

      // Проверяем условия завершения
      uint32_t elapsed = Now() - dwell_start;
      bool dwell_complete = (cmd->dwell_ms > 0 && elapsed >= cmd->dwell_ms);
      bool timeout_reached =
          (CheckTimeout(&scan.scanListenTimeout) && vfo->is_open) ||
          CheckTimeout(&scan.stayAtTimeout);

      if (dwell_complete || timeout_reached) {
        // Автоматически добавляем в белый список если нужно
        if ((cmd->flags & SCMD_FLAG_AUTO_WHITELIST) && vfo->is_open) {
          AddToWhitelist(current_freq);
          Log("[SCAN] Added %lu Hz to whitelist", current_freq);
        }

        // Завершаем команду
        if (!SCMD_Advance()) {
          SCMD_Rewind();
        }
        cmd_state = CMD_STATE_START;
        Log("[SCAN] Channel monitoring complete");
      }
      break;
    }
    break;

  default:
    // Для других типов команд используем простую логику
    cmd_state = CMD_STATE_START;
    SCMD_ExecuteCurrent();
    if (!SCMD_Advance()) {
      SCMD_Rewind();
    }
    break;
  }
}

static void SCAN_CheckCommandMode(void) {
  static uint32_t lastCmdCheck = 0;
  uint32_t now = Now();

  // Проверяем команды каждые 50мс (как обычное сканирование)
  if (now - lastCmdCheck < 50) {
    return;
  }
  lastCmdCheck = now;

  // Получаем текущую команду
  SCMD_Command *cmd = SCMD_GetCurrent();
  if (!cmd) {
    SCMD_Rewind();
    return;
  }

  // Пропускаем команды с низким приоритетом
  if (SCMD_ShouldSkip()) {
    SCMD_Advance();
    return;
  }

  // Используем улучшенную логику с поддержкой squelch и таймаутов
  SCAN_ExecuteCommandWithLogic();

  // Обновляем CPS
  UpdateCPS();
}

// =====

// API для установки режима
void SCAN_SetMode(ScanMode mode) {
  // Если переключаемся из командного режима - закрываем файл
  if (scan.commandMode && mode != scan.mode) {
    SCMD_Close();
    scan.commandMode = false;
  }

  scan.mode = mode;
  Log("[SCAN] mode=%s", SCAN_MODE_NAMES[scan.mode]);

  // Сброс состояния при смене режима
  scan.scanCycles = 0;
  scan.squelchLevel = 0;
  scan.thinking = false;
  SetTimeout(&scan.stayAtTimeout, 0);
  SetTimeout(&scan.scanListenTimeout, 0);

  // Специфичная инициализация для режима
  switch (mode) {
  case SCAN_MODE_SINGLE:
    // Не двигаемся с частоты, только мониторим
    break;
  case SCAN_MODE_CHANNEL:
    // Загрузим первый канал из списка
    // CHANNELS_LoadCurrentScanlistCH();
    break;
  case SCAN_MODE_FREQUENCY:
  case SCAN_MODE_ANALYSER:
    // Установим границы диапазона
    ApplyBandSettings();
    break;
  }
}

ScanMode SCAN_GetMode(void) { return scan.mode; }

uint32_t SCAN_GetCps() { return scan.currentCps; }

void SCAN_setBand(Band b) {
  gCurrentBand = b;
  ApplyBandSettings();
}

void SCAN_setStartF(uint32_t f) {
  gCurrentBand.start = f;
  ApplyBandSettings();
}

void SCAN_setEndF(uint32_t f) {
  gCurrentBand.end = f;
  ApplyBandSettings();
}

void SCAN_setRange(uint32_t fs, uint32_t fe) {
  gCurrentBand.start = fs;
  gCurrentBand.end = fe;
  ApplyBandSettings();
}

void SCAN_Next() { NextFrequency(); }

void SCAN_NextBlacklist() {
  LOOT_BlacklistLast();
  SCAN_Next();
}

void SCAN_NextWhitelist() {
  LOOT_BlacklistLast();
  SCAN_Next();
}

void SCAN_Init(bool multiband) {
  scan.isMultiband = multiband;
  vfo->msm.snr = 0;
  scan.lastCpsTime = Now();
  scan.scanCycles = 0;
  scan.currentCps = 0;

  // CHANNELS_LoadBlacklistToLoot();

  ApplyBandSettings();
  BK4819_WriteRegister(BK4819_REG_3F, 0);
}

// =============================
// Обработка сканирования
// =============================
static void HandleAnalyserMode() {
  vfo->msm.rssi = MeasureSignal(vfo->msm.f, false);
  scan.scanCycles++;
  SP_AddPoint(&vfo->msm);
  NextFrequency();
}

void SCAN_Check() {
  // Если включен режим команд, обрабатываем их
  if (scan.commandMode) {
    SCAN_CheckCommandMode();
    return;
  }

  RADIO_UpdateMultiwatch(gRadioState);

  // Режим анализатора — упрощенная логика
  if (scan.mode == SCAN_MODE_ANALYSER) {
    vfo->msm.rssi = MeasureSignal(vfo->msm.f, false);
    SP_AddPoint(&vfo->msm);
    NextStep();
    return;
  }

  // Одиночная частота — только мониторинг
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
      /* Log("SQL? %u RNG %u %u %u", vfo->msm.open, vfo->msm.rssi,
         vfo->msm.noise, vfo->msm.glitch); */
      SP_ShiftGraph(-1);
      SP_AddGraphPoint(&vfo->msm);
      radioTimer = Now();
    }

    return;
  }

  // Общая логика для канального и частотного режимов
  if (vfo->msm.open) {
    RADIO_UpdateSquelch(gRadioState);
    vfo->msm.open = vfo->is_open;
    gRedrawScreen = true;
  } else {
    UpdateSquelchAndRssi(scan.mode == SCAN_MODE_ANALYSER);
  }

  // Проверка на "думание" о squelch
  if (vfo->msm.open && !vfo->is_open) {
    // LogC(LOG_C_YELLOW, "MSM OPEN at %u, thinking", vfo->msm.f);
    scan.thinking = true;
    scan.wasThinkingEarlier = true;
    SYSTICK_DelayMs(SQL_DELAY);
    RADIO_UpdateSquelch(gRadioState);
    vfo->msm.open = vfo->is_open;
    scan.thinking = false;

    if (!vfo->msm.open) {
      scan.squelchLevel++;
    }
  }

  LOOT_Update(&vfo->msm);

  // Автокоррекция squelch
  if (vfo->is_open && !vfo->msm.open) {
    scan.squelchLevel = SP_GetNoiseFloor();
  }

  // Переход к следующей частоте/каналу с учетом таймаутов
  NextWithTimeout();
}

void SCAN_SetDelay(uint32_t delay) { scan.scanDelayUs = delay; }
uint32_t SCAN_GetDelay() { return scan.scanDelayUs; }

void SCAN_SetCommandPaused(bool paused) { scan.commandPaused = paused; }

bool SCAN_IsCommandPaused(void) { return scan.commandPaused; }
