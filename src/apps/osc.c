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
  // Буфер семплов
  uint16_t buffer[OSC_SAMPLES]; // Буфер захваченных значений (0-255)
  uint8_t write_pos; // Текущая позиция записи в буфер

  // Параметры триггера
  uint16_t trigger_level; // Уровень срабатывания триггера (0-255)
  uint8_t trigger_pos; // Позиция триггера в буфере
  bool triggered;      // Флаг: триггер сработал
  bool running;        // Флаг: захват активен

  // Параметры отображения
  uint8_t scale_v;       // Масштаб по вертикали (1-10)
  uint8_t scale_t;       // Масштаб по времени (1-10)
  uint16_t sample_delay; // Задержка между семплами (мкс)

  // Режимы работы
  bool dc_offset;    // Компенсация DC составляющей
  bool show_grid;    // Отображать сетку
  uint16_t dc_level; // Вычисленный уровень DC
} OscContext;

// === Глобальные переменные ===

static OscContext osc; // Контекст осциллографа
static uint32_t last_sample_time; // Время последнего семпла (для timing)

// === Вспомогательные функции ===
static void tuneTo(uint32_t f, uint32_t _) {
  (void)_;
  RADIO_SetParam(ctx, PARAM_FREQUENCY, f, true);
  RADIO_ApplySettings(ctx);
}

// === Функции настройки параметров ===

// Регулировка масштаба по вертикали (1-10)
static void setScaleV(uint32_t value, uint32_t _) {
  (void)_; // Неиспользуемый параметр

  osc.scale_v = value;
  if (osc.scale_v < 1)
    osc.scale_v = 1;
  if (osc.scale_v > 10)
    osc.scale_v = 10;
}

// Регулировка масштаба по времени (1-10)
static void setScaleT(uint32_t value, uint32_t _) {
  (void)_; // Неиспользуемый параметр

  osc.scale_t = value;
  if (osc.scale_t < 1)
    osc.scale_t = 1;
  if (osc.scale_t > 100)
    osc.scale_t = 100;

  // Обновляем задержку между семплами: 50-500 мкс
  osc.sample_delay = osc.scale_t;
}

// Установка уровня триггера (0-255)
static void setTriggerLevel(uint32_t value, uint32_t _) {
  (void)_; // Неиспользуемый параметр

  osc.trigger_level = value;
  if (osc.trigger_level > MAX_VAL)
    osc.trigger_level = MAX_VAL;
}

// Сброс триггера и запуск нового цикла захвата
static void triggerArm(void) {
  osc.triggered = false;
  osc.write_pos = 0;
  osc.trigger_pos = 0;
  osc.running = true;
  osc.dc_level = 0;
  memset(osc.buffer, 0, sizeof(osc.buffer));
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
  // Настройки по умолчанию
  osc.scale_v = 5; // Средний масштаб по вертикали
  osc.scale_t = 3; // Масштаб по времени (150 мкс)
  osc.trigger_level = 128;        // Триггер на середине (50%)
  osc.sample_delay = osc.scale_t; // Задержка между семплами
  osc.dc_offset = true;           // DC компенсация включена
  osc.show_grid = true;           // Сетка включена
  osc.dc_level = 0;

  // Очистка буфера и запуск
  memset(osc.buffer, 0, sizeof(osc.buffer));
  triggerArm();
}

void OSC_deinit(void) {}

// === Функции захвата и триггера ===

// Поиск точки срабатывания триггера в буфере
static void findTrigger(void) {
  if (osc.write_pos < 10)
    return; // Недостаточно данных

  // Ищем пересечение уровня триггера снизу вверх (rising edge)
  for (int i = 1; i < osc.write_pos - 1; i++) {
    if (osc.buffer[i - 1] < osc.trigger_level &&
        osc.buffer[i] >= osc.trigger_level) {
      osc.trigger_pos = i;
      osc.triggered = true;
      osc.running = false;
      break;
    }
  }
}

// Захват одного семпла с ADC
static void sampleADC(void) {
  if (!osc.running)
    return;

  // Проверяем заполненность буфера
  if (osc.write_pos >= OSC_SAMPLES) {
    // Буфер заполнен - ищем триггер если ещё не нашли
    if (!osc.triggered) {
      findTrigger();
    }
    osc.running = false;
    return;
  }

  // Считываем значение с ADC
  uint16_t value = BOARD_ADC_GetAPRS();

  // Обработка значения в зависимости от режима
  if (osc.dc_offset) {
    // === Режим DC компенсации ===
    static uint32_t dc_sum = 0;
    static uint8_t dc_count = 0;

    // Накапливаем среднее за первые 64 семпла
    if (dc_count < 64) {
      dc_sum += value;
      dc_count++;
      if (dc_count == 64) {
        osc.dc_level = dc_sum / 64;
      }
      return; // Пропускаем первые семплы
    }

    // Вычитаем DC составляющую и применяем масштаб
    if (value > osc.dc_level) {
      value = 128 + ((value - osc.dc_level) * osc.scale_v / 10);
    } else {
      value = 128 - ((osc.dc_level - value) * osc.scale_v / 10);
    }

    // Ограничение диапазона
    if (value > MAX_VAL)
      value = MAX_VAL;
  } else {
    // === Режим RAW (без DC компенсации) ===
    value = value * osc.scale_v / 10;
    if (value > MAX_VAL)
      value = MAX_VAL;
  }

  // Сохраняем семпл в буфер
  osc.buffer[osc.write_pos++] = value;

  // Пытаемся найти триггер при накоплении данных
  if (!osc.triggered && osc.write_pos > 10) {
    findTrigger();
  }
}

void OSC_update(void) {
  // Выполняем сэмплирование с заданной частотой
  uint32_t now = Now();
  if (now - last_sample_time >= osc.sample_delay) {
    sampleADC();
    last_sample_time = now;
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
    // Пунктирная линия (точка через 3 пикселя)
    for (int x = 0; x < LCD_WIDTH; x += 4) {
      PutPixel(x, ypos, C_FILL);
    }
  }

  // Вертикальные линии (8 делений по времени)
  for (int i = 0; i <= 8; i++) {
    int xpos = (LCD_WIDTH * i) / 8;
    // Пунктирная линия (точка через 3 пикселя)
    for (int y = OSC_TOP_MARGIN; y < OSC_TOP_MARGIN + OSC_GRAPH_H; y += 4) {
      PutPixel(xpos, y, C_FILL);
    }
  }
}

// Отрисовка формы сигнала
static void drawWaveform(void) {
  if (osc.write_pos < 2)
    return; // Нечего рисовать

  int prev_y = -1;
  int start_idx = 0;

  // Если сработал триггер - центрируем сигнал относительно точки триггера
  if (osc.triggered && osc.trigger_pos > 0) {
    start_idx = osc.trigger_pos - LCD_WIDTH / 2;
    if (start_idx < 0)
      start_idx = 0;
    if (start_idx + LCD_WIDTH > osc.write_pos)
      start_idx = osc.write_pos - LCD_WIDTH;
    if (start_idx < 0)
      start_idx = 0;
  }

  // Рисуем форму волны
  for (int x = 0; x < LCD_WIDTH; x++) {
    int idx = start_idx + x;
    if (idx >= OSC_SAMPLES || idx >= osc.write_pos)
      break;

    // Преобразуем значение 0-255 в координату Y на графике
    int y = OSC_TOP_MARGIN + OSC_GRAPH_H -
            (osc.buffer[idx] * OSC_GRAPH_H / (MAX_VAL + 1));

    // Ограничиваем область графика
    if (y < OSC_TOP_MARGIN)
      y = OSC_TOP_MARGIN;
    if (y >= OSC_TOP_MARGIN + OSC_GRAPH_H)
      y = OSC_TOP_MARGIN + OSC_GRAPH_H - 1;

    // Рисуем точку
    PutPixel(x, y, C_FILL);

    // Соединяем с предыдущей точкой линией (если изменение не слишком резкое)
    if (prev_y >= 0 && abs(y - prev_y) < OSC_GRAPH_H / 2) {
      DrawLine(x - 1, prev_y, x, y, C_FILL);
    }
    prev_y = y;
  }
}

// Отрисовка уровня триггера
static void drawTriggerLevel(void) {
  // Вычисляем Y-позицию линии триггера в области графика
  int y =
      OSC_TOP_MARGIN + OSC_GRAPH_H - (osc.trigger_level * OSC_GRAPH_H / 256);

  // Ограничиваем область графика
  if (y < OSC_TOP_MARGIN)
    y = OSC_TOP_MARGIN;
  if (y > OSC_TOP_MARGIN + OSC_GRAPH_H - 1)
    y = OSC_TOP_MARGIN + OSC_GRAPH_H - 1;

  // Рисуем маркеры слева и справа (треугольники из точек)
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
  // Строка 1 (baseline y=5): статус захвата
  if (osc.triggered) {
    PrintSmallEx(0, SMALL_FONT_H * 2, POS_L, C_FILL, "TRIG");
  } else if (osc.running) {
    PrintSmallEx(0, SMALL_FONT_H * 2, POS_L, C_FILL, "RUN %d%%",
                 osc.write_pos * 100 / OSC_SAMPLES);
  } else {
    PrintSmallEx(0, SMALL_FONT_H * 2, POS_L, C_FILL, "STOP");
  }

  // Строка 1 справа: масштаб
  PrintSmallEx(LCD_WIDTH, SMALL_FONT_H * 2, POS_R, C_FILL, "V:%d T:%d",
               osc.scale_v, osc.scale_t);

  // Строка 2 (baseline y=10): режим и задержка
  PrintSmallEx(0, SMALL_FONT_H * 3, POS_L, C_FILL, "%s",
               osc.dc_offset ? "DC" : "RAW");
  PrintSmallEx(LCD_WIDTH, SMALL_FONT_H * 3, POS_R, C_FILL, "%u ms",
               osc.sample_delay);

  // Строка 3 (baseline y=15): уровень триггера
  PrintSmallEx(0, SMALL_FONT_H * 4, POS_L, C_FILL, "Trig:%d",
               osc.trigger_level);
  char buf[16];
  mhzToS(buf, RADIO_GetParam(ctx, PARAM_FREQUENCY));
  PrintSmallEx(LCD_XCENTER, SMALL_FONT_H * 2, POS_C, C_FILL, "%s", buf);
}

void OSC_render(void) {
  // Очистка экрана
  FillRect(0, 0, LCD_WIDTH, LCD_HEIGHT, C_CLEAR);

  // Рисуем радио-статус (верхняя строка, если есть)
  STATUSLINE_RenderRadioSettings();

  // === Область графика ===

  // Рисуем сетку (если включена)
  drawGrid();

  // Рисуем форму сигнала
  drawWaveform();

  // Рисуем маркеры уровня триггера
  drawTriggerLevel();

  // === Информационные панели ===

  // Статусная информация сверху
  drawStatus();
}
