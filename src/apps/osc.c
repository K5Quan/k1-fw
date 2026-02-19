#include "osc.h"
#include "../board.h"
#include "../driver/systick.h"
#include "../driver/uart.h"
#include "../helper/fft.h"
#include "../helper/regs-menu.h"
#include "../settings.h"
#include "../ui/finput.h"
#include "../ui/graphics.h"
#include "../ui/statusline.h"
#include "apps.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------
#define SMALL_FONT_H 6
#define OSC_TOP_MARGIN 24 // три строки статуса сверху
#define OSC_GRAPH_H (LCD_HEIGHT - OSC_TOP_MARGIN - 1)

#define MAX_VAL 4095u
#define FFT_BINS 64 // используем нижние N/2 бинов

// ---------------------------------------------------------------------------
// Режимы отображения
// ---------------------------------------------------------------------------
typedef enum {
  MODE_WAVE = 0,
  MODE_FFT = 1,
} OscMode;

// ---------------------------------------------------------------------------
// Контекст
// ---------------------------------------------------------------------------
typedef struct {
  // --- Кольцевой дисплейный буфер (WAVE) ---
  // disp_head — позиция следующей записи.
  // Старейший = disp_head, новейший = disp_head-1 (mod LCD_WIDTH).
  uint16_t disp_buf[LCD_WIDTH];
  uint8_t disp_head;

  // --- Накопительный буфер для FFT ---
  // Набираем 128 прореженных сырых семплов, после этого считаем FFT.
  uint16_t fft_acc[128];
  uint8_t fft_acc_pos;

  // Последний посчитанный спектр (магнитуды бинов 0..FFT_BINS-1)
  uint16_t fft_mag[FFT_BINS];
  bool fft_fresh; // true — mag[] актуален

  // --- Общие параметры ---
  OscMode mode;
  uint8_t scale_v; // вертикальный масштаб [1..10] (только WAVE)
  uint8_t scale_t; // прореживание [1..32]: каждый N-й семпл идёт в буфер
  uint16_t trigger_level;

  bool dc_offset;
  bool show_grid;
  bool show_trigger;

  // --- IIR DC-фильтр: dc_iir = mean * 256 ---
  uint32_t dc_iir;

  uint8_t decimate_cnt;

  int32_t lpf_iir;
} OscContext;

static OscContext osc;

static uint16_t dmaMin;
static uint16_t dmaMax;

// ---------------------------------------------------------------------------
// Вспомогательные
// ---------------------------------------------------------------------------
static void tuneTo(uint32_t f, uint32_t _) {
  (void)_;
  RADIO_SetParam(ctx, PARAM_FREQUENCY, f, true);
  RADIO_ApplySettings(ctx);
}

// ---------------------------------------------------------------------------
// Установка параметров
// ---------------------------------------------------------------------------
static void setScaleV(uint32_t v, uint32_t _) {
  (void)_;
  if (v < 1)
    v = 1;
  if (v > 10)
    v = 10;
  osc.scale_v = (uint8_t)v;
}

static void setScaleT(uint32_t v, uint32_t _) {
  (void)_;
  if (v < 1)
    v = 1;
  if (v > 128)
    v = 128;
  osc.scale_t = (uint8_t)v;
}

static void setTriggerLevel(uint32_t v, uint32_t _) {
  (void)_;
  if (v > MAX_VAL)
    v = MAX_VAL;
  osc.trigger_level = (uint16_t)v;
}

static void triggerArm(void) {
  memset(osc.disp_buf, 0, sizeof(osc.disp_buf));
  osc.disp_head = 0;
  osc.decimate_cnt = 0;
  osc.fft_acc_pos = 0;
  osc.fft_fresh = false;
  osc.dc_iir = 2048UL << 8; // начальная оценка DC = середина 12-бит шкалы
  osc.lpf_iir = 2048L << 8;
}

// ---------------------------------------------------------------------------
// Клавиши
// ---------------------------------------------------------------------------
bool OSC_key(KEY_Code_t key, Key_State_t state) {
  if (REGSMENU_Key(key, state)) {
    return true;
  }
  if (state != KEY_RELEASED && state != KEY_LONG_PRESSED_CONT)
    return false;

  switch (key) {
  // Вертикальный масштаб
  case KEY_2:
    setScaleV(osc.scale_v + 1, 0);
    return true;
  case KEY_8:
    setScaleV(osc.scale_v - 1, 0);
    return true;

  // Прореживание (скорость развёртки / разрешение FFT по времени)
  case KEY_1:
    setScaleT(osc.scale_t + 1, 0);
    return true;
  case KEY_7:
    setScaleT(osc.scale_t - 1, 0);
    return true;

  // Уровень триггера (шаг 128 = ~3% шкалы)
  case KEY_3:
    setTriggerLevel(osc.trigger_level + 128, 0);
    return true;
  case KEY_9:
    setTriggerLevel(osc.trigger_level - 128, 0);
    return true;

  // DC компенсация
  case KEY_4:
    osc.dc_offset = !osc.dc_offset;
    triggerArm();
    return true;

  // Сетка
  case KEY_F:
    osc.show_grid = !osc.show_grid;
    return true;

  // Частота
  case KEY_5:
    FINPUT_setup(BK4819_F_MIN, BK4819_F_MAX, UNIT_MHZ, false);
    FINPUT_Show(tuneTo);
    return true;

  // Уровень триггера цифрами
  case KEY_0:
    FINPUT_setup(0, MAX_VAL, UNIT_RAW, false);
    FINPUT_Show(setTriggerLevel);
    return true;

  // Переключение WAVE / FFT
  case KEY_6:
    osc.mode = (osc.mode == MODE_WAVE) ? MODE_FFT : MODE_WAVE;
    return true;

  // Сброс
  case KEY_STAR:
    triggerArm();
    return true;

  default:
    return false;
  }
}

// ---------------------------------------------------------------------------
// Инициализация / деинициализация
// ---------------------------------------------------------------------------
void OSC_init(void) {
  osc.mode = MODE_WAVE;
  osc.scale_v = 5;
  osc.scale_t = 16;
  osc.trigger_level = 2048;
  osc.dc_offset = true;
  osc.show_grid = true;
  osc.show_trigger = true;

  triggerArm();
}

void OSC_deinit(void) {}

// ---------------------------------------------------------------------------
// Захват семплов из DMA-буферов
// ---------------------------------------------------------------------------

static void push_sample(uint16_t raw) {
  // IIR DC-фильтр: alpha = 1/256, dc_iir хранит mean*256
  // osc.dc_iir += (int32_t)raw - (int32_t)(osc.dc_iir >> 8);
  uint16_t dc = (uint16_t)(osc.dc_iir >> 8);

  // --- WAVE: нормированное значение в кольцо ---
  uint16_t val;
  if (osc.dc_offset) {
    int32_t v = ((int32_t)raw - dc) * osc.scale_v / 10 + 2048;
    if (v < 0)
      v = 0;
    if (v > (int32_t)MAX_VAL)
      v = MAX_VAL;
    val = (uint16_t)v;
  } else {
    int32_t v = ((int32_t)raw - 2048) * osc.scale_v / 10 + 2048;
    if (v < 0)
      v = 0;
    if (v > (int32_t)MAX_VAL)
      v = MAX_VAL;
    val = (uint16_t)v;
  }
  osc.disp_buf[osc.disp_head] = val;
  osc.disp_head = (uint8_t)((osc.disp_head + 1) % LCD_WIDTH);

  // --- FFT: накапливаем сырые семплы, DC вычтем перед FFT ---
  if (osc.fft_acc_pos < 128) {
    osc.fft_acc[osc.fft_acc_pos++] = raw;
  }

  // Когда набрали 128 семплов — считаем FFT
  if (osc.fft_acc_pos == 128) {
    // Буферы объявлены static, чтобы не занимать стек (~512 байт)
    static int16_t fft_re[128];
    static int16_t fft_im[128];

    uint16_t dc_snap = (uint16_t)(osc.dc_iir >> 8);
    for (int i = 0; i < 128; i++) {
      int32_t v = (int32_t)osc.fft_acc[i] - dc_snap;
      // Ограничиваем до Q15 диапазона
      if (v > 32767)
        v = 32767;
      if (v < -32767)
        v = -32767;
      fft_re[i] = (int16_t)v;
      fft_im[i] = 0;
    }

    FFT_128(fft_re, fft_im);
    FFT_Magnitude(fft_re, fft_im, osc.fft_mag, FFT_BINS);

    osc.fft_fresh = true;
    osc.fft_acc_pos = 0; // сразу начинаем следующее накопление
  }
}

static void process_block(const uint16_t *src, uint32_t len) {
  dmaMin = UINT16_MAX;
  dmaMax = 0;
  for (uint32_t i = 0; i < len; i++) {
    if (src[i] > dmaMax) {
      dmaMax = src[i];
    }
    if (src[i] < dmaMin) {
      dmaMin = src[i];
    }
    // Антиалиасинговый LPF: a = 1/32, срез ~Fs/(2π×32)
    // При Fs=48кГц: ~240 Гц — ниже Найквиста для scale_t=72
    osc.lpf_iir += ((int32_t)((uint32_t)src[i] << 8) - osc.lpf_iir) >> 5;

    if (++osc.decimate_cnt >= osc.scale_t) {
      osc.decimate_cnt = 0;
      // push_sample((uint16_t)(osc.lpf_iir >> 8));
      push_sample(src[i]);
      if (osc.mode == MODE_FFT) {
        gRedrawScreen = true;
      }
    }
  }
}

// Вызывается из main loop ~каждые 2 мс
void OSC_update(void) {
  if (aprs_ready1) {
    aprs_ready1 = false;
    process_block(aprs_process_buffer1, APRS_BUFFER_SIZE);
  }
  if (aprs_ready2) {
    aprs_ready2 = false;
    process_block(aprs_process_buffer2, APRS_BUFFER_SIZE);
  }
}

// ---------------------------------------------------------------------------
// Отрисовка — общие
// ---------------------------------------------------------------------------
static void drawGrid(void) {
  if (!osc.show_grid)
    return;
  for (int i = 0; i <= 4; i++) {
    int y = OSC_TOP_MARGIN + (OSC_GRAPH_H * i) / 4;
    for (int x = 0; x < LCD_WIDTH; x += 4)
      PutPixel(x, y, C_FILL);
  }
  for (int i = 0; i <= 8; i++) {
    int x = (LCD_WIDTH * i) / 8;
    for (int y = OSC_TOP_MARGIN; y < OSC_TOP_MARGIN + OSC_GRAPH_H; y += 4)
      PutPixel(x, y, C_FILL);
  }
}

// val [0..MAX_VAL] → Y в области графика
static inline int val_to_y(uint16_t val) {
  int y = OSC_TOP_MARGIN + OSC_GRAPH_H - 1 -
          (int32_t)val * (OSC_GRAPH_H - 1) / MAX_VAL;
  if (y < OSC_TOP_MARGIN)
    y = OSC_TOP_MARGIN;
  if (y >= OSC_TOP_MARGIN + OSC_GRAPH_H)
    y = OSC_TOP_MARGIN + OSC_GRAPH_H - 1;
  return y;
}

// ---------------------------------------------------------------------------
// Отрисовка — WAVE
// ---------------------------------------------------------------------------
static void drawWaveform(void) {
  // Кольцо: идём от disp_head (старейший) до disp_head-1 (новейший).
  // DrawLine между каждой парой — разрывов нет никогда.
  int prev_y = val_to_y(osc.disp_buf[osc.disp_head]);
  for (int x = 1; x < LCD_WIDTH; x++) {
    uint8_t idx = (uint8_t)((osc.disp_head + x) % LCD_WIDTH);
    int y = val_to_y(osc.disp_buf[idx]);
    DrawLine(x - 1, prev_y, x, y, C_FILL);
    prev_y = y;
  }
}

static void drawTriggerMarker(void) {
  if (!osc.show_trigger)
    return;
  int y = val_to_y(osc.trigger_level);
  for (int i = 0; i < 3; i++) {
    PutPixel(i, y, C_FILL);
    PutPixel(LCD_WIDTH - 1 - i, y, C_FILL);
  }
  PutPixel(1, y - 1, C_FILL);
  PutPixel(1, y + 1, C_FILL);
  PutPixel(LCD_WIDTH - 2, y - 1, C_FILL);
  PutPixel(LCD_WIDTH - 2, y + 1, C_FILL);
}

// ---------------------------------------------------------------------------
// Отрисовка — FFT (64 бина × 2 px = 128 px ширина)
// ---------------------------------------------------------------------------
static void drawSpectrum(void) {
  if (!osc.fft_fresh) {
    PrintSmallEx(LCD_XCENTER, OSC_TOP_MARGIN + OSC_GRAPH_H / 2, POS_C, C_FILL,
                 "FFT...");
    return;
  }

  // Найти максимум среди бинов 1..63 (пропускаем DC)
  uint16_t peak_mag = 1;
  uint8_t peak_bin = 1;
  for (int k = 1; k < FFT_BINS; k++) {
    if (osc.fft_mag[k] > peak_mag) {
      peak_mag = osc.fft_mag[k];
      peak_bin = (uint8_t)k;
    }
  }

  // Рисуем столбики: 2 px на бин
  for (int k = 0; k < FFT_BINS; k++) {
    uint32_t h = (uint32_t)osc.fft_mag[k] * (OSC_GRAPH_H - 1) / peak_mag;
    if (h > (uint32_t)(OSC_GRAPH_H - 1))
      h = OSC_GRAPH_H - 1;
    int x0 = k * 2;
    int y_top = OSC_TOP_MARGIN + OSC_GRAPH_H - 1 - (int)h;
    DrawLine(x0, y_top, x0, OSC_TOP_MARGIN + OSC_GRAPH_H - 1, C_FILL);
    DrawLine(x0 + 1, y_top, x0 + 1, OSC_TOP_MARGIN + OSC_GRAPH_H - 1, C_FILL);
  }

  // Маленькая стрелка над пиком
  int px = peak_bin * 2 + 1;
  if (px > 0 && px < LCD_WIDTH - 1) {
    PutPixel(px, OSC_TOP_MARGIN, C_FILL);
    PutPixel(px - 1, OSC_TOP_MARGIN + 1, C_FILL);
    PutPixel(px + 1, OSC_TOP_MARGIN + 1, C_FILL);
  }
}

// ---------------------------------------------------------------------------
// Отрисовка — статус (общий)
// ---------------------------------------------------------------------------
static void drawStatus(void) {
  // Строка 1: режим | частота
  char buf[16];
  mhzToS(buf, RADIO_GetParam(ctx, PARAM_FREQUENCY));
  PrintSmallEx(0, SMALL_FONT_H * 2, POS_L, C_FILL,
               osc.mode == MODE_FFT ? "FFT" : "OSC");
  PrintSmallEx(LCD_XCENTER, SMALL_FONT_H * 2, POS_C, C_FILL, "%s", buf);
  PrintSmallEx(LCD_XCENTER, SMALL_FONT_H * 3, POS_C, C_FILL, "%4u / %4u",
               dmaMin, dmaMax);

  // Строка 2: DC/RAW | масштаб
  PrintSmallEx(0, SMALL_FONT_H * 3, POS_L, C_FILL,
               osc.dc_offset ? "DC" : "RAW");
  if (osc.mode == MODE_WAVE) {
    PrintSmallEx(LCD_WIDTH, SMALL_FONT_H * 3, POS_R, C_FILL, "V:%d T:%d",
                 osc.scale_v, osc.scale_t);
  } else {
    PrintSmallEx(LCD_WIDTH, SMALL_FONT_H * 3, POS_R, C_FILL, "T:%d",
                 osc.scale_t);
  }

  // Строка 3: батарея | триггер (только WAVE)
  if (osc.mode == MODE_WAVE) {
    PrintSmallEx(LCD_WIDTH, SMALL_FONT_H * 4, POS_R, C_FILL, "T:%d",
                 osc.trigger_level);
  }
}

// ---------------------------------------------------------------------------
// Главная функция отрисовки
// ---------------------------------------------------------------------------
void OSC_render(void) {
  FillRect(0, OSC_TOP_MARGIN, LCD_WIDTH, OSC_GRAPH_H, C_CLEAR);

  drawGrid();

  if (osc.mode == MODE_WAVE) {
    drawWaveform();
    drawTriggerMarker();
  } else {
    drawSpectrum();
  }

  drawStatus();
  REGSMENU_Draw();
}
