#include "analyzer.h"
#include "../driver/st7565.h"
#include "../driver/systick.h"
#include "../driver/uart.h"
#include "../helper/bands.h"
#include "../helper/lootlist.h"
#include "../helper/measurements.h"
#include "../helper/scan.h"
#include "../radio.h"
#include "../settings.h"
#include "../ui/components.h"
#include "../ui/finput.h"
#include "../ui/spectrum.h"
#include "../ui/statusline.h"
#include "apps.h"
#include <stdint.h>

// ============================================================================
// Режимы отображения
// ============================================================================

typedef enum {
  VIEW_MODE_NORMAL,   // Обычный режим - текущий спектр
  VIEW_MODE_MAX_HOLD, // Максимальное удержание
  VIEW_MODE_AVG,      // Усреднение
  VIEW_MODE_DIFF,     // Разница с базой
} ViewMode;

// ============================================================================
// Состояние анализатора
// ============================================================================

static struct {
  ViewMode viewMode;
  bool triggerEnabled;
  uint8_t triggerLevel; // RSSI уровень триггера (0-255)
  bool triggerArmed;    // Триггер взведён
  bool baselineStored;  // База сохранена
  uint8_t baseline[128]; // Сохранённая база (максимум 128 точек)
  uint8_t maxHold[128];    // Максимальные значения
  uint32_t avgBuffer[128]; // Буфер для усреднения
  uint16_t avgCount;       // Счётчик усреднений
  bool paused;             // Пауза сканирования
  VMinMax displayRange;    // Диапазон отображения
} analyzer = {
    .viewMode = VIEW_MODE_NORMAL,
    .triggerEnabled = false,
    .triggerLevel = 100,
    .triggerArmed = true,
    .baselineStored = false,
    .paused = false,
    .avgCount = 0,
    .displayRange = {.vMin = 55, .vMax = RSSI_MAX},
};

// ============================================================================
// Вспомогательные функции
// ============================================================================

static void resetMaxHold(void) {
  memset(analyzer.maxHold, 0, sizeof(analyzer.maxHold));
}

static void resetAverage(void) {
  memset(analyzer.avgBuffer, 0, sizeof(analyzer.avgBuffer));
  analyzer.avgCount = 0;
}

static void storeBaseline(void) {
  // Копируем текущий спектр как базу
  for (uint16_t i = 0; i < LCD_WIDTH; i++) {
    analyzer.baseline[i] = SP_GetPointRSSI(i);
  }
  analyzer.baselineStored = true;
  Log("[ANALYZER] Baseline stored, %u points", LCD_WIDTH);
}

static void clearBaseline(void) {
  memset(analyzer.baseline, 0, sizeof(analyzer.baseline));
  analyzer.baselineStored = false;
  Log("[ANALYZER] Baseline cleared");
}

static bool checkTrigger(void) {
  if (!analyzer.triggerEnabled) {
    return true; // Триггер выключен - всегда разрешаем
  }

  if (!analyzer.triggerArmed) {
    // Проверяем, упал ли сигнал ниже уровня
    VMinMax mm = SP_GetMinMax();
    if (mm.vMax < analyzer.triggerLevel - 5) {
      analyzer.triggerArmed = true;
      Log("[ANALYZER] Trigger armed");
    }
    return false;
  }

  // Триггер взведён - проверяем превышение уровня
  VMinMax mm = SP_GetMinMax();
  if (mm.vMax >= analyzer.triggerLevel) {
    analyzer.triggerArmed = false;
    Log("[ANALYZER] Trigger fired at RSSI=%u", mm.vMax);
    return true;
  }

  return false;
}

static void updateMaxHold(void) {
  for (uint16_t i = 0; i < LCD_WIDTH; i++) {
    uint8_t rssi = SP_GetPointRSSI(i);
    if (rssi > analyzer.maxHold[i]) {
      analyzer.maxHold[i] = rssi;
    }
  }
}

static void updateAverage(void) {
  for (uint16_t i = 0; i < LCD_WIDTH; i++) {
    analyzer.avgBuffer[i] += SP_GetPointRSSI(i);
  }
  analyzer.avgCount++;
}

// ============================================================================
// Инициализация
// ============================================================================

static void setRange(uint32_t fs, uint32_t fe) {
  BANDS_RangeClear();
  SCAN_setRange(fs, fe);
  BANDS_RangePush(gCurrentBand);

  // Сбрасываем данные при смене диапазона
  resetMaxHold();
  resetAverage();
  clearBaseline();
}

static void initBand(void) {
  if (gCurrentBand.detached) {
    gCurrentBand = BANDS_ByFrequency(RADIO_GetParam(ctx, PARAM_FREQUENCY));
    gCurrentBand.detached = true;
  } else {
    if (!gCurrentBand.start && !gCurrentBand.end) {
      gCurrentBand = DEFAULT_BAND;
    }
  }

  if (gCurrentBand.start == DEFAULT_BAND.start &&
      gCurrentBand.end == DEFAULT_BAND.end) {
    uint32_t step = StepFrequencyTable[RADIO_GetParam(ctx, PARAM_STEP)];
    gCurrentBand.start = ctx->frequency - 64 * step;
    gCurrentBand.end = gCurrentBand.start + 128 * step;
  }
}

void ANALYZER_init(void) {
  gMonitorMode = false;
  SPECTRUM_Y = 8;
  SPECTRUM_H = 44;

  initBand();

  gCurrentBand.step = RADIO_GetParam(ctx, PARAM_STEP);
  BANDS_RangeClear();
  BANDS_RangePush(gCurrentBand);

  SCAN_SetDelay(800); // Быстрее для анализатора
  SCAN_SetMode(SCAN_MODE_ANALYSER);
  SCAN_Init(false);

  // Сброс данных
  resetMaxHold();
  resetAverage();
  analyzer.paused = false;
  analyzer.triggerArmed = true;

  Log("[ANALYZER] Initialized");
}

// ============================================================================
// Обработка клавиш
// ============================================================================

static bool handleLongPress(KEY_Code_t key) {
  switch (key) {
  case KEY_6:
    // Сохранить текущий спектр как базу
    storeBaseline();
    return true;

  case KEY_PTT:
    // Переход в VFO на центральной частоте
    if (gLastActiveLoot) {
      RADIO_SetParam(ctx, PARAM_FREQUENCY, gLastActiveLoot->f, true);
    } else {
      uint32_t centerF = (gCurrentBand.start + gCurrentBand.end) / 2;
      RADIO_SetParam(ctx, PARAM_FREQUENCY, centerF, true);
    }
    RADIO_ApplySettings(ctx);
    RADIO_SaveCurrentVFO(gRadioState);
    APPS_run(APP_VFO1);
    return true;

  default:
    return false;
  }
}

static bool handleRepeatableKeys(KEY_Code_t key) {
  switch (key) {
  case KEY_1:
  case KEY_7:
    // Скорость сканирования
    SCAN_SetDelay(
        AdjustU(SCAN_GetDelay(), 100, 10000, key == KEY_1 ? 100 : -100));
    return true;

  case KEY_3:
  case KEY_9:
    // Шаг частоты
    RADIO_IncDecParam(ctx, PARAM_STEP, key == KEY_3, false);
    gCurrentBand.step = RADIO_GetParam(ctx, PARAM_STEP);
    SCAN_setBand(gCurrentBand);
    resetMaxHold();
    resetAverage();
    return true;

  case KEY_UP:
  case KEY_DOWN:
    // Уровень триггера
    if (analyzer.triggerEnabled) {
      analyzer.triggerLevel =
          AdjustU(analyzer.triggerLevel, 40, 200, key == KEY_UP ? 5 : -5);
      analyzer.triggerArmed = true;
    }
    return true;

  default:
    return false;
  }
}

static bool handleRelease(KEY_Code_t key) {
  uint32_t step = StepFrequencyTable[RADIO_GetParam(ctx, PARAM_STEP)];

  switch (key) {
  case KEY_0:
    // Пауза/возобновление
    analyzer.paused = !analyzer.paused;
    if (analyzer.paused) {
      SCAN_SetMode(SCAN_MODE_SINGLE);
    } else {
      SCAN_SetMode(SCAN_MODE_ANALYSER);
    }
    Log("[ANALYZER] %s", analyzer.paused ? "Paused" : "Running");
    return true;

  case KEY_2:
    // Переключение режима отображения
    analyzer.viewMode = (analyzer.viewMode + 1) % 4;
    if (analyzer.viewMode == VIEW_MODE_AVG) {
      resetAverage();
    }
    Log("[ANALYZER] View mode: %d", analyzer.viewMode);
    return true;

  case KEY_4:
    // Включение/выключение триггера
    analyzer.triggerEnabled = !analyzer.triggerEnabled;
    analyzer.triggerArmed = true;
    if (!analyzer.triggerEnabled) {
      analyzer.paused = false;
      SCAN_SetMode(SCAN_MODE_ANALYSER);
    }
    Log("[ANALYZER] Trigger %s", analyzer.triggerEnabled ? "ON" : "OFF");
    return true;

  case KEY_5:
    // Ввод диапазона
    gFInputCallback = setRange;
    FINPUT_setup(0, BK4819_F_MAX, UNIT_MHZ, true);
    gFInputValue1 = 0;
    gFInputValue2 = 0;
    FINPUT_init();
    gFInputActive = true;
    return true;

  case KEY_6:
    // Очистить базу (короткое нажатие)
    clearBaseline();
    return true;

  case KEY_8:
    // Сброс Max Hold / Average
    if (analyzer.viewMode == VIEW_MODE_MAX_HOLD) {
      resetMaxHold();
    } else if (analyzer.viewMode == VIEW_MODE_AVG) {
      resetAverage();
    }
    return true;

  case KEY_STAR:
    // Список найденных сигналов
    APPS_run(APP_LOOTLIST);
    return true;

  case KEY_SIDE1:
    // Масштаб вниз
    analyzer.displayRange.vMax =
        AdjustU(analyzer.displayRange.vMax, 100, 255, -10);
    return true;

  case KEY_SIDE2:
    // Масштаб вверх
    analyzer.displayRange.vMax =
        AdjustU(analyzer.displayRange.vMax, 100, 255, 10);
    return true;

  default:
    return false;
  }
}

bool ANALYZER_key(KEY_Code_t key, Key_State_t state) {
  if (state == KEY_LONG_PRESSED) {
    return handleLongPress(key);
  }

  if (state == KEY_RELEASED || state == KEY_LONG_PRESSED_CONT) {
    if (handleRepeatableKeys(key)) {
      return true;
    }
  }

  if (state == KEY_RELEASED) {
    return handleRelease(key);
  }

  return false;
}

// ============================================================================
// Обновление
// ============================================================================

void ANALYZER_update(void) {
  // Проверка триггера
  if (analyzer.triggerEnabled && !analyzer.paused) {
    if (!checkTrigger()) {
      // Триггер не сработал - не обновляем данные
      return;
    }
    // Триггер сработал - ставим на паузу
    analyzer.paused = true;
    SCAN_SetMode(SCAN_MODE_SINGLE);
    gRedrawScreen = true;
  }

  // Обновление данных в зависимости от режима
  if (!analyzer.paused) {
    switch (analyzer.viewMode) {
    case VIEW_MODE_MAX_HOLD:
      updateMaxHold();
      break;
    case VIEW_MODE_AVG:
      updateAverage();
      break;
    default:
      break;
    }
  }
}

// ============================================================================
// Отрисовка
// ============================================================================

static const char *VIEW_MODE_NAMES[] = {
    [VIEW_MODE_NORMAL] = "Normal",
    [VIEW_MODE_MAX_HOLD] = "Max",
    [VIEW_MODE_AVG] = "Avg",
    [VIEW_MODE_DIFF] = "Diff",
};

static void renderSpectrum(void) {
  uint16_t pointCount = LCD_WIDTH;

  // Копируем данные для отображения
  static Measurement displayData[128];

  switch (analyzer.viewMode) {
  case VIEW_MODE_NORMAL:
    // Обычный режим - показываем текущий спектр
    SP_Render(&gCurrentBand, analyzer.displayRange);
    break;

  case VIEW_MODE_MAX_HOLD:
    // Max Hold - показываем максимумы
    for (uint16_t i = 0; i < pointCount; i++) {
      displayData[i].f = SP_X2F(i);
      displayData[i].rssi = analyzer.maxHold[i];
      displayData[i].open = false;
    }
    // Рисуем max hold
    for (uint16_t i = 0; i < pointCount; i++) {
      SP_RenderPoint(&displayData[i], i, pointCount, &gCurrentBand,
                     analyzer.displayRange, C_FILL);
    }
    // Поверх рисуем текущий спектр полупрозрачным
    SP_Render(&gCurrentBand, analyzer.displayRange);
    break;

  case VIEW_MODE_AVG:
    // Average - показываем усреднённый спектр
    if (analyzer.avgCount > 0) {
      for (uint16_t i = 0; i < pointCount; i++) {
        displayData[i].f = SP_X2F(i);
        displayData[i].rssi = analyzer.avgBuffer[i] / analyzer.avgCount;
        displayData[i].open = false;
      }
      for (uint16_t i = 0; i < pointCount; i++) {
        SP_RenderPoint(&displayData[i], i, pointCount, &gCurrentBand,
                       analyzer.displayRange, C_FILL);
      }
    }
    break;

  case VIEW_MODE_DIFF:
    // Diff - показываем разницу с базой
    if (analyzer.baselineStored) {
      for (uint16_t i = 0; i < pointCount; i++) {
        uint8_t current = SP_GetPointRSSI(i);
        uint8_t base = analyzer.baseline[i];
        displayData[i].f = SP_X2F(i);
        // Показываем только превышение над базой
        displayData[i].rssi = (current > base) ? (current - base) : 0;
        displayData[i].open = false;
      }
      for (uint16_t i = 0; i < pointCount; i++) {
        SP_RenderPoint(&displayData[i], i, pointCount, &gCurrentBand,
                       analyzer.displayRange, C_FILL);
      }
    } else {
      // База не сохранена - показываем обычный спектр
      SP_Render(&gCurrentBand, analyzer.displayRange);
    }
    break;
  }
}

static void renderInfo(void) {
  const uint32_t step = StepFrequencyTable[RADIO_GetParam(ctx, PARAM_STEP)];
  VMinMax mm = SP_GetMinMax();

  // Верхняя строка - режим и статус
  PrintSmallEx(0, 12, POS_L, C_FILL, "%s", VIEW_MODE_NAMES[analyzer.viewMode]);

  if (analyzer.paused) {
    PrintSmallEx(LCD_XCENTER, 12, POS_C, C_FILL, "PAUSED");
  } else if (analyzer.triggerEnabled && analyzer.triggerArmed) {
    PrintSmallEx(LCD_XCENTER, 12, POS_C, C_FILL, "ARMED");
  }

  // Скорость сканирования и шаг
  PrintSmallEx(LCD_WIDTH, 12, POS_R, C_FILL, "%uus", SCAN_GetDelay());
  PrintSmallEx(LCD_WIDTH, 18, POS_R, C_FILL, "%u.%02uk", step / 100,
               step % 100);

  // Min/Max значения
  PrintSmallEx(0, 18, POS_L, C_FILL, "%3u %+3d", mm.vMax, Rssi2DBm(mm.vMax));
  PrintSmallEx(0, 24, POS_L, C_FILL, "%3u %+3d", mm.vMin, Rssi2DBm(mm.vMin));

  // CPS
  PrintSmallEx(LCD_XCENTER, 18, POS_C, C_FILL, "%u cps", SCAN_GetCps());

  // Счётчик усреднений
  if (analyzer.viewMode == VIEW_MODE_AVG && analyzer.avgCount > 0) {
    PrintSmallEx(LCD_XCENTER, 24, POS_C, C_FILL, "n=%u", analyzer.avgCount);
  }

  // Индикатор базы
  if (analyzer.baselineStored) {
    PrintSmallEx(LCD_WIDTH, 24, POS_R, C_FILL, "BASE");
  }
}

static void renderTriggerLine(void) {
  if (!analyzer.triggerEnabled) {
    return;
  }

  // Рисуем линию триггера
  SP_RenderLine(analyzer.triggerLevel, analyzer.displayRange);

  // Уровень триггера
  PrintSmallEx(LCD_WIDTH - 1, SPECTRUM_Y + 2, POS_R, C_FILL, "T:%u",
               analyzer.triggerLevel);
}

static void renderFrequencies(void) {
  FSmall(1, LCD_HEIGHT - 2, POS_L, gCurrentBand.start);

  uint32_t centerF = (gCurrentBand.start + gCurrentBand.end) / 2;
  FSmall(LCD_XCENTER, LCD_HEIGHT - 2, POS_C, centerF);

  FSmall(LCD_WIDTH - 1, LCD_HEIGHT - 2, POS_R, gCurrentBand.end);
}

void ANALYZER_render(void) {
  STATUSLINE_RenderRadioSettings();

  renderSpectrum();
  renderInfo();
  renderTriggerLine();
  renderFrequencies();

  // Маркер последнего активного сигнала
  if (gLastActiveLoot) {
    SP_RenderArrow(gLastActiveLoot->f);
  }
}

void ANALYZER_deinit(void) {
  // Очистка при выходе
  resetMaxHold();
  resetAverage();
  clearBaseline();
}
