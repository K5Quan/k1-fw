#include "osc.h"
#include "../board.h"
#include "../driver/systick.h"
#include "../driver/uart.h"
#include "../helper/regs-menu.h"
#include "../settings.h"
#include "../ui/finput.h"
#include "../ui/graphics.h"
#include "../ui/statusline.h"
#include "apps.h"
#include <stdbool.h>
#include <string.h>

#define OSC_SAMPLES LCD_WIDTH    // 128 пикселей
#define OSC_TRIGGER_DELAY 500    // мс
#define OSC_ADC_CHANNEL ADC_APRS // канал для APRS сигнала

// Layout константы (текст рисуется по baseline)
#define SMALL_FONT_H 6  // высота маленького шрифта
#define NORMAL_FONT_H 8 // высота обычного шрифта
#define OSC_TOP_MARGIN 24 // отступ сверху для 3 строк статуса
#define OSC_BOTTOM_MARGIN 1 // отступ снизу для подсказок
#define OSC_GRAPH_H (LCD_HEIGHT - OSC_TOP_MARGIN - OSC_BOTTOM_MARGIN)

#define MAX_VAL 4095

// Структура данных осциллографа
typedef struct {
  // --- Дисплейный кольцевой буфер ---
  // Хранит последние LCD_WIDTH нормированных значений [0..4095].
  // disp_head — позиция следующей записи (старейший семпл — disp_head,
  // новейший — disp_head-1 по модулю LCD_WIDTH).
  uint16_t disp_buf[LCD_WIDTH];
  uint8_t  disp_head;   // указатель записи в кольцо

  // --- Параметры триггера ---
  uint16_t trigger_level; // Уровень триггера [0..4095]
  bool     show_trigger;  // Рисовать маркер триггера

  // --- Параметры отображения ---
  uint8_t  scale_v;  // Масштаб по вертикали [1..10]
  uint8_t  scale_t;  // Прореживание: каждый N-й семпл идёт в буфер [1..32]

  // --- Режимы работы ---
  bool     dc_offset;  // IIR-компенсация DC
  bool     show_grid;  // Показывать сетку

  // --- IIR DC-фильтр ---
  // dc_iir хранит <<8 * mean, чтобы не терять дробную часть.
  // Обновляется: dc_iir += (raw - (dc_iir >> 8)), сдвиг = alpha 1/256.
  uint32_t dc_iir;

  // --- Счётчик прореживания ---
  uint8_t  decimate_cnt;
} OscContext;

// === Глобальные переменные ===
static OscContext osc;

// === Вспомогательные функции ===
static void tuneTo(uint32_t f, uint32_t _) {
  (void)_;
  RADIO_SetParam(ctx, PARAM_FREQUENCY, f, true);
  RADIO_ApplySettings(ctx);
}

// === Функции настройки параметров ===

static void setScaleV(uint32_t value, uint32_t _) {
  (void)_;
  if (value < 1)  value = 1;
  if (value > 10) value = 10;
  osc.scale_v = value;
}

// scale_t — это прореживание: в кольцо попадает каждый scale_t-й семпл.
// Чем больше — тем «медленнее» развёртка.
static void setScaleT(uint32_t value, uint32_t _) {
  (void)_;
  if (value < 1)  value = 1;
  if (value > 32) value = 32;
  osc.scale_t = value;
}

static void setTriggerLevel(uint32_t value, uint32_t _) {
  (void)_;
  if (value > MAX_VAL) value = MAX_VAL;
  osc.trigger_level = value;
}

// Сброс кольцевого буфера и DC-аккумулятора
static void triggerArm(void) {
  memset(osc.disp_buf, 0, sizeof(osc.disp_buf));
  osc.disp_head    = 0;
  osc.decimate_cnt = 0;
  osc.dc_iir       = 2048UL << 8; // Начальное значение DC — середина шкалы
}

// === Основные функции приложения ===

bool OSC_key(KEY_Code_t key, Key_State_t state) {

  // Обрабатываем только отпускание и продолжительное нажатие
  if (state != KEY_RELEASED && state != KEY_LONG_PRESSED_CONT) {
    return false;
  }

  switch (key) {
  // === Управление масштабом по вертикали ===
  case KEY_2: // Уменьшить V
    setScaleV(osc.scale_v + 1, 0);
    return true;
  case KEY_8: // Увеличить V
    setScaleV(osc.scale_v - 1, 0);
    return true;

  // === Управление масштабом по времени ===
  case KEY_1: // Уменьшить T
    setScaleT(osc.scale_t + 1, 0);
    return true;
  case KEY_7: // Увеличить T
    setScaleT(osc.scale_t - 1, 0);
    return true;

  // === Управление триггером ===
  case KEY_3: // Уровень вниз
    setTriggerLevel(osc.trigger_level + 8, 0);
    return true;
  case KEY_9: // Уровень вверх
    setTriggerLevel(osc.trigger_level - 8, 0);
    return true;

  // === Режимы и функции ===
  case KEY_4: // Переключить DC компенсацию
    osc.dc_offset = !osc.dc_offset;
    triggerArm(); // Перезапускаем после смены режима
    return true;

  case KEY_F: // Переключить сетку
    osc.show_grid = !osc.show_grid;
    return true;

  case KEY_5:
    FINPUT_setup(BK4819_F_MIN, BK4819_F_MAX, UNIT_MHZ, false);
    FINPUT_Show(tuneTo);
    return true;

  case KEY_0:
    FINPUT_setup(0, MAX_VAL, UNIT_RAW, false);
    FINPUT_Show(setTriggerLevel);
    return true;

  case KEY_STAR: // Запуск захвата
    triggerArm();
    return true;

    /* case KEY_STAR: // Полный сброс
      memset(osc.buffer, 0, sizeof(osc.buffer));
      osc.triggered = false;
      osc.running = false;
      return true; */

  default:
    return false;
  }
}

void OSC_init(void) {
  osc.scale_v       = 5;
  osc.scale_t       = 4;        // прореживание: каждый 4-й семпл
  osc.trigger_level = 2048;     // середина шкалы 12-бит
  osc.dc_offset     = true;
  osc.show_grid     = true;
  osc.show_trigger  = true;
  triggerArm();
}

void OSC_deinit(void) {}

// === Функции захвата ===

// Обработать один сырой семпл: DC-фильтр, масштаб, запись в кольцо.
static void push_sample(uint16_t raw) {
  // --- IIR DC-фильтр (alpha ≈ 1/256) ---
  // dc_iir хранит значение * 256, чтобы не терять дробную часть.
  osc.dc_iir += (int32_t)raw - (int32_t)(osc.dc_iir >> 8);
  uint16_t dc = osc.dc_iir >> 8;  // текущая оценка постоянной составляющей

  uint16_t val;
  if (osc.dc_offset) {
    // Вычитаем DC, центрируем вокруг 2048, масштабируем
    int32_t v = ((int32_t)raw - dc) * osc.scale_v / 10 + 2048;
    if (v < 0)      v = 0;
    if (v > MAX_VAL) v = MAX_VAL;
    val = (uint16_t)v;
  } else {
    // RAW: просто масштабируем от 0
    uint32_t v = (uint32_t)raw * osc.scale_v / 10;
    val = v > MAX_VAL ? MAX_VAL : (uint16_t)v;
  }

  osc.disp_buf[osc.disp_head] = val;
  osc.disp_head = (osc.disp_head + 1) % LCD_WIDTH;
}

// Обработать массив сырых семплов с прореживанием scale_t.
static void process_block(const uint16_t *src, uint32_t len) {
  for (uint32_t i = 0; i < len; i++) {
    osc.decimate_cnt++;
    if (osc.decimate_cnt >= osc.scale_t) {
      osc.decimate_cnt = 0;
      push_sample(src[i]);
    }
  }
}

// OSC_update вызывается из main loop каждые ~2 мс.
// Когда DMA-полубуфер готов — забираем его и раскладываем в кольцо.
void OSC_update(void) {
  // Забираем оба флага атомарно (читаем, потом сбрасываем)
  if (aprs_ready1) {
    aprs_ready1 = false;  // сброс ДО обработки, чтобы не пропустить следующий
    process_block(aprs_process_buffer1, APRS_BUFFER_SIZE);
  }
  if (aprs_ready2) {
    aprs_ready2 = false;
    process_block(aprs_process_buffer2, APRS_BUFFER_SIZE);
  }
}

// === Функции отрисовки ===

// Отрисовка сетки
static void drawGrid(void) {
  if (!osc.show_grid)
    return;

  // Горизонтальные линии (5 уровней: 0%, 25%, 50%, 75%, 100%)
  for (int i = 0; i <= 4; i++) {
    int ypos = OSC_TOP_MARGIN + (OSC_GRAPH_H * i) / 4;
    for (int x = 0; x < LCD_WIDTH; x += 4) {
      PutPixel(x, ypos, C_FILL);
    }
  }

  // Вертикальные линии (8 делений по времени)
  for (int i = 0; i <= 8; i++) {
    int xpos = (LCD_WIDTH * i) / 8;
    for (int y = OSC_TOP_MARGIN; y < OSC_TOP_MARGIN + OSC_GRAPH_H; y += 4) {
      PutPixel(xpos, y, C_FILL);
    }
  }
}

// Перевести значение [0..MAX_VAL] в Y-координату в области графика
static inline int val_to_y(uint16_t val) {
  int y = OSC_TOP_MARGIN + OSC_GRAPH_H - 1 -
          ((int32_t)val * (OSC_GRAPH_H - 1) / MAX_VAL);
  if (y < OSC_TOP_MARGIN)            y = OSC_TOP_MARGIN;
  if (y >= OSC_TOP_MARGIN + OSC_GRAPH_H) y = OSC_TOP_MARGIN + OSC_GRAPH_H - 1;
  return y;
}

// Отрисовка формы сигнала из кольцевого буфера.
// Обходим от disp_head (старейший) до disp_head-1 (новейший),
// соединяем соседние точки линиями — разрывов нет.
static void drawWaveform(void) {
  int prev_y = val_to_y(osc.disp_buf[osc.disp_head]);

  for (int x = 1; x < LCD_WIDTH; x++) {
    uint8_t idx = (osc.disp_head + x) % LCD_WIDTH;
    int y = val_to_y(osc.disp_buf[idx]);
    DrawLine(x - 1, prev_y, x, y, C_FILL);
    prev_y = y;
  }
}

// Отрисовка маркера уровня триггера (стрелки слева и справа)
static void drawTriggerLevel(void) {
  if (!osc.show_trigger) return;

  int y = val_to_y(osc.trigger_level);

  // Маленькие стрелки: ▷ слева, ◁ справа
  for (int i = 0; i < 3; i++) {
    PutPixel(i, y, C_FILL);
    PutPixel(LCD_WIDTH - 1 - i, y, C_FILL);
  }
  PutPixel(1, y - 1, C_FILL);
  PutPixel(1, y + 1, C_FILL);
  PutPixel(LCD_WIDTH - 2, y - 1, C_FILL);
  PutPixel(LCD_WIDTH - 2, y + 1, C_FILL);
}

// Отрисовка статуса
static void drawStatus(void) {
  // Строка 1: частота в центре
  char buf[16];
  mhzToS(buf, RADIO_GetParam(ctx, PARAM_FREQUENCY));
  PrintSmallEx(LCD_XCENTER, SMALL_FONT_H * 2, POS_C, C_FILL, "%s", buf);

  // Строка 1 справа: масштабы
  PrintSmallEx(LCD_WIDTH, SMALL_FONT_H * 2, POS_R, C_FILL, "V:%d T:%d",
               osc.scale_v, osc.scale_t);

  // Строка 2: режим DC | уровень DC (в мВ или просто raw)
  PrintSmallEx(0, SMALL_FONT_H * 3, POS_L, C_FILL, "%s",
               osc.dc_offset ? "DC" : "RAW");

  // Строка 3: уровень триггера
  PrintSmallEx(0, SMALL_FONT_H * 4, POS_L, C_FILL, "Trig:%d",
               osc.trigger_level);
}

void OSC_render(void) {
  FillRect(0, OSC_TOP_MARGIN, LCD_WIDTH, OSC_GRAPH_H, C_CLEAR);

  drawGrid();
  drawWaveform();
  drawTriggerLevel();
  drawStatus();
}

