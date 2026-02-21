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
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define SMALL_FONT_H 6
#define OSC_TOP_MARGIN 24
#define OSC_GRAPH_H (LCD_HEIGHT - OSC_TOP_MARGIN - 1)
#define MAX_VAL 4095u
#define FFT_BINS 64

typedef enum { MODE_WAVE = 0, MODE_FFT = 1 } OscMode;

// ---------------------------------------------------------------------------
// Контекст — оптимизированный
//
//  ДО                               ПОСЛЕ
//  uint16_t disp_buf[128]  256 B    uint8_t  disp_buf[128]  128 B   (-128)
//  uint16_t fft_acc[128]   256 B    удалён, алиас fft_re[]          (-256)
//  uint16_t fft_mag[64]    128 B    uint8_t  fft_mag[64]     64 B   (-64)
//  Итого экономия: 448 байт
// ---------------------------------------------------------------------------
typedef struct {
  // Хранит Y-координату пикселя (0..LCD_HEIGHT-1), вычисленную в push_sample.
  uint8_t disp_buf[LCD_WIDTH];
  uint8_t disp_head;

  // fft_acc удалён. Накопление идёт напрямую в fft_re[] через uint16_t-алиас.
  uint8_t fft_acc_pos;

  // Нормировано в 0..127 при записи. drawSpectrum всё равно ищет пик сам.
  uint8_t fft_mag[FFT_BINS];
  bool fft_fresh;

  OscMode mode;
  uint8_t scale_v;
  uint8_t scale_t;
  uint16_t trigger_level;

  bool dc_offset;
  bool show_grid;
  bool show_trigger;

  uint32_t dc_iir;
  uint8_t decimate_cnt;
  int32_t lpf_iir;
} OscContext;

static OscContext osc;
static uint16_t dmaMin;
static uint16_t dmaMax;

// ---------------------------------------------------------------------------
// FFT-буферы на уровне модуля (были static внутри push_sample, эффект тот же).
//
// fft_re[] — двойное назначение:
//   • фаза накопления: хранит сырые uint16_t-семплы (через aliasd каст)
//   • фаза FFT: int16_t-данные с вычтенным DC и наложенным окном
// fft_im[] — только для FFT, обнуляется перед каждым расчётом.
// ---------------------------------------------------------------------------
static int16_t fft_re[128];
static int16_t fft_im[128];

// ---------------------------------------------------------------------------
// val_to_y — объявлена до push_sample, которая её использует
// ---------------------------------------------------------------------------
static inline uint8_t val_to_y(uint16_t val) {
  int y = OSC_TOP_MARGIN + OSC_GRAPH_H - 1 -
          (int32_t)val * (OSC_GRAPH_H - 1) / MAX_VAL;
  if (y < OSC_TOP_MARGIN)
    y = OSC_TOP_MARGIN;
  if (y >= OSC_TOP_MARGIN + OSC_GRAPH_H)
    y = OSC_TOP_MARGIN + OSC_GRAPH_H - 1;
  return (uint8_t)y;
}

// ---------------------------------------------------------------------------
// Вспомогательные / установка параметров
// ---------------------------------------------------------------------------
static void tuneTo(uint32_t f, uint32_t _) {
  (void)_;
  RADIO_SetParam(ctx, PARAM_FREQUENCY, f, true);
  RADIO_ApplySettings(ctx);
}

static void setScaleV(uint32_t v, uint32_t _) {
  (void)_;
  if (v < 1)
    v = 1;
  if (v > 64)
    v = 64;
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
  memset(osc.disp_buf, val_to_y(2048), sizeof(osc.disp_buf));
  osc.disp_head = 0;
  osc.decimate_cnt = 0;
  osc.fft_acc_pos = 0;
  osc.fft_fresh = false;
  osc.dc_iir = 2048UL << 8;
  osc.lpf_iir = 2048L << 8;
}

// ---------------------------------------------------------------------------
// Клавиши
// ---------------------------------------------------------------------------
bool OSC_key(KEY_Code_t key, Key_State_t state) {
  if (REGSMENU_Key(key, state))
    return true;
  if (state != KEY_RELEASED && state != KEY_LONG_PRESSED_CONT)
    return false;

  switch (key) {
  case KEY_2:
    setScaleV(osc.scale_v + 1, 0);
    return true;
  case KEY_8:
    setScaleV(osc.scale_v - 1, 0);
    return true;
  case KEY_1:
    setScaleT(osc.scale_t + 1, 0);
    return true;
  case KEY_7:
    setScaleT(osc.scale_t - 1, 0);
    return true;
  case KEY_3:
    setTriggerLevel(osc.trigger_level + 128, 0);
    return true;
  case KEY_9:
    setTriggerLevel(osc.trigger_level - 128, 0);
    return true;
  case KEY_4:
    osc.dc_offset = !osc.dc_offset;
    triggerArm();
    return true;
  case KEY_F:
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
  case KEY_6:
    osc.mode = (osc.mode == MODE_WAVE) ? MODE_FFT : MODE_WAVE;
    return true;
  case KEY_STAR:
    triggerArm();
    return true;
  default:
    return false;
  }
}

void OSC_init(void) {
  osc.mode = MODE_WAVE;
  osc.scale_v = 10;
  osc.scale_t = 4;
  osc.trigger_level = 2048;
  osc.dc_offset = false;
  osc.show_grid = false;
  osc.show_trigger = true;
  triggerArm();
}

void OSC_deinit(void) {}

// ---------------------------------------------------------------------------
// Таблица Ханна (Q15, 128 точек)
// ---------------------------------------------------------------------------
static const uint16_t hann128[128] = {
    0,     20,    79,    178,   315,   492,   707,   961,   1252,  1580,  1945,
    2345,  2780,  3248,  3749,  4282,  4845,  5438,  6059,  6708,  7382,  8081,
    8803,  9546,  10309, 11090, 11888, 12700, 13526, 14363, 15209, 16063, 16923,
    17787, 18652, 19518, 20381, 21240, 22093, 22937, 23771, 24592, 25398, 26186,
    26955, 27703, 28427, 29125, 29797, 30439, 31050, 31628, 32173, 32682, 33155,
    33590, 33986, 34341, 34656, 34929, 35160, 35347, 35491, 35591, 35647, 35591,
    35491, 35347, 35160, 34929, 34656, 34341, 33986, 33590, 33155, 32682, 32173,
    31628, 31050, 30439, 29797, 29125, 28427, 27703, 26955, 26186, 25398, 24592,
    23771, 22937, 22093, 21240, 20381, 19518, 18652, 17787, 16923, 16063, 15209,
    14363, 13526, 12700, 11888, 11090, 10309, 9546,  8803,  8081,  7382,  6708,
    6059,  5438,  4845,  4282,  3749,  3248,  2780,  2345,  1945,  1580,  1252,
    961,   707,   492,   315,   178,   79,    20,
};

// ---------------------------------------------------------------------------
// push_sample
// ---------------------------------------------------------------------------
static void push_sample(uint16_t raw) {
  // IIR DC-фильтр
  osc.dc_iir += (int32_t)raw - (int32_t)(osc.dc_iir >> 8);
  uint16_t dc = (uint16_t)(osc.dc_iir >> 8);

  // --- WAVE: вычисляем Y-пиксель и кладём в кольцо как uint8_t ---
  {
    int32_t v = osc.dc_offset ? ((int32_t)raw - dc) * osc.scale_v / 10 + 2048
                              : ((int32_t)raw - 2048) * osc.scale_v / 10 + 2048;
    if (v < 0)
      v = 0;
    if (v > (int32_t)MAX_VAL)
      v = MAX_VAL;
    osc.disp_buf[osc.disp_head] = val_to_y((uint16_t)v);
    osc.disp_head = (uint8_t)((osc.disp_head + 1) % LCD_WIDTH);
  }

  // --- FFT: накапливаем сырые uint16_t прямо в память fft_re[] ---
  // Это безопасно: оба типа 16-битные, буфер не используется для FFT
  // пока fft_acc_pos < 128.
  if (osc.fft_acc_pos < 128) {
    ((uint16_t *)fft_re)[osc.fft_acc_pos++] = raw;
  }

  if (osc.fft_acc_pos == 128) {
    uint16_t dc_snap = (uint16_t)(osc.dc_iir >> 8);

    // In-place конвертация: uint16_t → int16_t (DC вычитание + clamp).
    // Чтение и запись в ту же ячейку i — корректно (значение читается до
    // записи).
    for (int i = 0; i < 128; i++) {
      int32_t v = (int32_t)((uint16_t *)fft_re)[i] - dc_snap;
      if (v > 32767)
        v = 32767;
      if (v < -32767)
        v = -32767;
      fft_re[i] = (int16_t)v;
      fft_im[i] = 0;
    }

    // Окно Ханна
    for (int i = 0; i < 128; i++) {
      fft_re[i] = (int16_t)((int32_t)fft_re[i] * hann128[i] >> 15);
    }

    FFT_128(fft_re, fft_im);

    // Магнитуды → нормируем в 0..127 (>>1) чтобы гарантированно влезть в
    // uint8_t. Относительные величины не теряются — drawSpectrum нормирует по
    // пику.
    {
      static uint16_t mag_tmp[FFT_BINS]; // static: не занимает стек при вызове
      FFT_Magnitude(fft_re, fft_im, mag_tmp, FFT_BINS);
      for (int k = 0; k < FFT_BINS; k++) {
        uint16_t v = mag_tmp[k] >> 1;
        osc.fft_mag[k] = v > 255 ? 255 : (uint8_t)v;
      }
    }

    osc.fft_fresh = true;
    osc.fft_acc_pos = 0;
  }
}

static void process_block(const volatile uint16_t *src, uint32_t len) {
  dmaMin = UINT16_MAX;
  dmaMax = 0;
  for (uint32_t i = 0; i < len; i++) {
    uint16_t s = src[i];
    if (s > dmaMax)
      dmaMax = s;
    if (s < dmaMin)
      dmaMin = s;
    if (++osc.decimate_cnt >= osc.scale_t) {
      osc.decimate_cnt = 0;
      push_sample(s);
      if (osc.mode == MODE_FFT)
        gRedrawScreen = true;
    }
  }
}

void OSC_update(void) {
  if (aprs_ready1) {
    aprs_ready1 = false;
    process_block(&adc_dma_buffer[0], APRS_BUFFER_SIZE);
  }
  if (aprs_ready2) {
    aprs_ready2 = false;
    process_block(&adc_dma_buffer[APRS_BUFFER_SIZE], APRS_BUFFER_SIZE);
  }
}

// ---------------------------------------------------------------------------
// Отрисовка
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

static void drawWaveform(void) {
  // disp_buf хранит Y напрямую — DrawLine без дополнительных вычислений
  int prev_y = osc.disp_buf[osc.disp_head];
  for (int x = 1; x < LCD_WIDTH; x++) {
    uint8_t idx = (uint8_t)((osc.disp_head + x) % LCD_WIDTH);
    int y = osc.disp_buf[idx];
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

static void drawSpectrum(void) {
  if (!osc.fft_fresh) {
    PrintSmallEx(LCD_XCENTER, OSC_TOP_MARGIN + OSC_GRAPH_H / 2, POS_C, C_FILL,
                 "FFT...");
    return;
  }

  uint8_t peak_mag = 1;
  uint8_t peak_bin = 1;
  for (int k = 1; k < FFT_BINS; k++) {
    if (osc.fft_mag[k] > peak_mag) {
      peak_mag = osc.fft_mag[k];
      peak_bin = (uint8_t)k;
    }
  }

  for (int k = 0; k < FFT_BINS; k++) {
    uint32_t h = (uint32_t)osc.fft_mag[k] * (OSC_GRAPH_H - 1) / peak_mag;
    if (h > (uint32_t)(OSC_GRAPH_H - 1))
      h = OSC_GRAPH_H - 1;
    int x0 = k * 2;
    int y_top = OSC_TOP_MARGIN + OSC_GRAPH_H - 1 - (int)h;
    DrawLine(x0, y_top, x0, OSC_TOP_MARGIN + OSC_GRAPH_H - 1, C_FILL);
    DrawLine(x0 + 1, y_top, x0 + 1, OSC_TOP_MARGIN + OSC_GRAPH_H - 1, C_FILL);
  }

  int px = peak_bin * 2 + 1;
  if (px > 0 && px < LCD_WIDTH - 1) {
    PutPixel(px, OSC_TOP_MARGIN, C_FILL);
    PutPixel(px - 1, OSC_TOP_MARGIN + 1, C_FILL);
    PutPixel(px + 1, OSC_TOP_MARGIN + 1, C_FILL);
  }
}

static void drawStatus(void) {
  char buf[16];
  mhzToS(buf, RADIO_GetParam(ctx, PARAM_FREQUENCY));
  PrintSmallEx(0, SMALL_FONT_H * 2, POS_L, C_FILL,
               osc.mode == MODE_FFT ? "FFT" : "OSC");
  PrintSmallEx(LCD_XCENTER, SMALL_FONT_H * 2, POS_C, C_FILL, "%s", buf);
  PrintSmallEx(LCD_XCENTER, SMALL_FONT_H * 3, POS_C, C_FILL, "%4u / %4u",
               dmaMin, dmaMax);
  PrintSmallEx(0, SMALL_FONT_H * 3, POS_L, C_FILL,
               osc.dc_offset ? "DC" : "RAW");
  if (osc.mode == MODE_WAVE) {
    PrintSmallEx(LCD_WIDTH, SMALL_FONT_H * 3, POS_R, C_FILL, "V:%d T:%d",
                 osc.scale_v, osc.scale_t);
    PrintSmallEx(LCD_WIDTH, SMALL_FONT_H * 4, POS_R, C_FILL, "T:%d",
                 osc.trigger_level);
  } else {
    PrintSmallEx(LCD_WIDTH, SMALL_FONT_H * 3, POS_R, C_FILL, "T:%d",
                 osc.scale_t);
  }
}

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
