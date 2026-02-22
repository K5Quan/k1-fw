#include "osc.h"
#include "../board.h"
#include "../driver/systick.h"
#include "../driver/uart.h"
#include "../helper/audio_io.h"
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
#define PULSE_BUF_SIZE 255

// ---------------------------------------------------------------------------
// Режимы отображения
// ---------------------------------------------------------------------------
typedef enum { MODE_WAVE = 0, MODE_FFT = 1, MODE_OOK = 2 } OscMode;

// ---------------------------------------------------------------------------
// Импульс OOK
// ---------------------------------------------------------------------------
typedef struct {
  uint16_t duration; // длительность в сэмплах ADC
  uint8_t high;      // 1=высокий, 0=низкий
} Pulse;

// ---------------------------------------------------------------------------
// Контекст
// ---------------------------------------------------------------------------
typedef struct {
  // --- WAVE ---
  uint8_t disp_buf[LCD_WIDTH];
  uint8_t disp_head;

  // --- FFT ---
  uint8_t fft_acc_pos;
  uint8_t fft_mag[FFT_BINS];
  bool fft_fresh;

  // --- OOK ---
  Pulse ook_pulses[PULSE_BUF_SIZE];
  uint8_t ook_count;
  uint32_t ook_envelope_iir;
  uint16_t ook_threshold;
  uint8_t ook_state;
  uint16_t ook_counter;

  // --- Общие ---
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
// FFT-буферы
// ---------------------------------------------------------------------------
static int16_t fft_re[128];
static int16_t fft_im[128];

// ---------------------------------------------------------------------------
// Таблица Ханна (Q15, 128 точек, максимум 32767 в центре)
// ---------------------------------------------------------------------------
static const uint16_t hann128[128] = {
    0,     20,    79,    178,   315,   492,   707,   961,   1252,  1580,  1945,
    2345,  2780,  3248,  3749,  4282,  4845,  5438,  6059,  6708,  7382,  8081,
    8803,  9546,  10309, 11090, 11888, 12700, 13526, 14363, 15209, 16063, 16923,
    17787, 18652, 19518, 20381, 21240, 22093, 22937, 23771, 24592, 25398, 26186,
    26955, 27703, 28427, 29125, 29797, 30439, 31050, 31628, 32173, 32682, 32767,
    32767, 32767, 32682, 32173, 31628, 31050, 30439, 29797, 29125, 28427, 27703,
    26955, 26186, 25398, 24592, 23771, 22937, 22093, 21240, 20381, 19518, 18652,
    17787, 16923, 16063, 15209, 14363, 13526, 12700, 11888, 11090, 10309, 9546,
    8803,  8081,  7382,  6708,  6059,  5438,  4845,  4282,  3749,  3248,  2780,
    2345,  1945,  1580,  1252,  961,   707,   492,   315,   178,   79,    20,
    0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
    0,     0,     0,     0,     0,     0,     0,
};

// test_tone
#include "../driver/gpio.h" // AUDIO_AudioPathOn/Off

// LUT синуса, 32 точки, амплитуда ±1800 отсчётов вокруг 2048
static const int16_t sine32[32] = {
    0,     352,  680,  963,  1188, 1340, 1413,  1402,  1308,  1137,  899,
    608,   282,  -60,  -400, -718, -992, -1204, -1341, -1402, -1385, -1293,
    -1133, -916, -655, -370, -78,  208,  471,   693,   856,   945,
};

static uint32_t tone_phase = 0;

// 440 Гц при Fs=9600: шаг фазы = 440 * 32 / 9600 ≈ 1.467
// Используем fixed-point 8.8: шаг = (440 * 32 * 256) / 9600 = 375
#define TONE_PHASE_STEP 375u // Q8

static uint32_t tone_source(uint16_t *buf, uint32_t n) {
  for (uint32_t i = 0; i < n; i++) {
    int16_t s = sine32[(tone_phase >> 8) & 31];
    buf[i] = (uint16_t)(2048 + s);
    tone_phase += TONE_PHASE_STEP;
  }
  return n; // бесконечный источник
}

void TEST_TONE_Start(void) {
  tone_phase = 0;
  GPIO_EnableAudioPath();
  AUDIO_IO_SourceSet(tone_source);
  LogC(LOG_C_BRIGHT_WHITE, "TEST: 440Hz tone started on PA4");
}

void TEST_TONE_Stop(void) {
  AUDIO_IO_SourceClear();
  GPIO_DisableAudioPath();
  LogC(LOG_C_BRIGHT_WHITE, "TEST: tone stopped");
}

// ---------------------------------------------------------------------------
// val_to_y
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
// Вспомогательные / параметры
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
  osc.ook_count = 0;
  osc.ook_state = 0;
  osc.ook_counter = 0;
  osc.ook_envelope_iir = 0;
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
  // Вертикальный масштаб / порог OOK вверх
  case KEY_2:
    if (osc.mode == MODE_OOK) {
      osc.ook_threshold += 20;
    } else {
      setScaleV(osc.scale_v + 1, 0);
    }
    return true;
  // Вертикальный масштаб / порог OOK вниз
  case KEY_8:
    if (osc.mode == MODE_OOK) {
      if (osc.ook_threshold > 20)
        osc.ook_threshold -= 20;
    } else {
      setScaleV(osc.scale_v - 1, 0);
    }
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
  case KEY_SIDE1:
    TEST_TONE_Start();
    SYSTICK_DelayMs(2000); // 2 секунды
    TEST_TONE_Stop();
    return true;
  case KEY_SIDE2: {
    uint16_t reg43 = BK4819_ReadRegister(0x43); // 15
    if ((reg43 >> 15) & 1) {
      reg43 &= ~(1 << 15);
      BK4819_SetAF(BK4819_AF_FM);
    } else {
      reg43 |= 1 << 15;
      BK4819_SetAF(BK4819_AF_MUTE);
    }
    BK4819_WriteRegister(0x43, reg43);
  }
    return true;

  // Переключение режимов: WAVE → FFT → OOK → WAVE
  case KEY_6:
    osc.mode = (OscMode)((osc.mode + 1) % 3);
    osc.ook_count = 0;
    return true;

  case KEY_STAR:
    osc.ook_count = 0;
    triggerArm();
    return true;

  default:
    return false;
  }
}

// ---------------------------------------------------------------------------
// push_sample — WAVE + FFT
// ---------------------------------------------------------------------------
static void push_sample(uint16_t raw) {
  // IIR DC-фильтр
  osc.dc_iir += (int32_t)raw - (int32_t)(osc.dc_iir >> 8);
  uint16_t dc = (uint16_t)(osc.dc_iir >> 8);

  // WAVE: вычисляем Y-пиксель
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

  // FFT: накапливаем сырые uint16_t напрямую в память fft_re[]
  if (osc.fft_acc_pos < 128) {
    ((uint16_t *)fft_re)[osc.fft_acc_pos++] = raw;
  }

  if (osc.fft_acc_pos == 128) {
    uint16_t dc_snap = (uint16_t)(osc.dc_iir >> 8);

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

    {
      static uint16_t mag_tmp[FFT_BINS];
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

// ---------------------------------------------------------------------------
// ook_push_sample — детектор огибающей + фронтов
// ---------------------------------------------------------------------------
static void ook_push_sample(uint16_t raw) {
  // Огибающая: быстрая атака (1/4), медленный спад (1/32)
  int32_t v = (int32_t)raw - 2048;
  uint32_t av = (uint32_t)((v < 0 ? -v : v) << 8);
  if (av > osc.ook_envelope_iir)
    osc.ook_envelope_iir += (av - osc.ook_envelope_iir) >> 2;
  else
    osc.ook_envelope_iir += (av - osc.ook_envelope_iir) >> 3;

  uint16_t env = (uint16_t)(osc.ook_envelope_iir >> 8);
  uint8_t bit = env > osc.ook_threshold ? 1 : 0;

  if (bit == osc.ook_state) {
    if (osc.ook_counter < 65535)
      osc.ook_counter++;
  } else {
    // Фронт — сохранить импульс (фильтр дребезга: минимум 3 сэмпла)
    if (osc.ook_counter > 3 && osc.ook_count < PULSE_BUF_SIZE) {
      osc.ook_pulses[osc.ook_count].duration = osc.ook_counter;
      osc.ook_pulses[osc.ook_count].high = osc.ook_state;
      osc.ook_count++;
    }
    osc.ook_state = bit;
    osc.ook_counter = 1;
  }
}

// ---------------------------------------------------------------------------
// process_block
// ---------------------------------------------------------------------------
static void process_block(const volatile uint16_t *src, uint32_t len) {
  dmaMin = UINT16_MAX;
  dmaMax = 0;
  for (uint32_t i = 0; i < len; i++) {
    uint16_t s = src[i];
    if (s > dmaMax)
      dmaMax = s;
    if (s < dmaMin)
      dmaMin = s;

    if (osc.mode == MODE_OOK) {
      ook_push_sample(s);
      /* if (osc.ook_count >= PULSE_BUF_SIZE)
        gRedrawScreen = true; */
    }

    if (++osc.decimate_cnt >= osc.scale_t) {
      osc.decimate_cnt = 0;
      push_sample(s);
      if (osc.mode == MODE_FFT)
        gRedrawScreen = true;
    }
  }
}

static void osc_sink(const uint16_t *buf, uint32_t n) {
  process_block(buf, n); // та же логика, но buf уже валиден
}

// ---------------------------------------------------------------------------
// Инициализация
// ---------------------------------------------------------------------------
void OSC_init(void) {
  osc.mode = MODE_WAVE;
  osc.scale_v = 10;
  osc.scale_t = 2;
  osc.trigger_level = 2048;
  osc.dc_offset = false;
  osc.show_grid = false;
  osc.show_trigger = true;
  osc.ook_threshold = 200;
  triggerArm();
  AUDIO_IO_SinkRegister(osc_sink);
}

void OSC_deinit(void) { AUDIO_IO_SinkUnregister(osc_sink); }

void OSC_update(void) {}

// ---------------------------------------------------------------------------
// Отрисовка — сетка
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

// ---------------------------------------------------------------------------
// Отрисовка — WAVE
// ---------------------------------------------------------------------------
static void drawWaveform(void) {
  int prev_y = osc.disp_buf[osc.disp_head];
  for (int x = 1; x < LCD_WIDTH; x++) {
    uint8_t idx = (uint8_t)((osc.disp_head + x) % LCD_WIDTH);
    int y = osc.disp_buf[idx];
    DrawLine(x - 1, prev_y, x, y, C_FILL);
    prev_y = y;
  }
  PrintSmallEx(LCD_XCENTER, SMALL_FONT_H * 3, POS_C, C_FILL, "%4u / %4u",
               dmaMin, dmaMax);
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
// Отрисовка — FFT
// ---------------------------------------------------------------------------
static void drawSpectrum(void) {
  if (!osc.fft_fresh) {
    PrintSmallEx(LCD_XCENTER, OSC_TOP_MARGIN + OSC_GRAPH_H / 2, POS_C, C_FILL,
                 "FFT...");
    return;
  }

  uint8_t peak_mag = 8;
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

  // Стрелка над пиком
  int px = peak_bin * 2 + 1;
  if (px > 0 && px < LCD_WIDTH - 1) {
    PutPixel(px, OSC_TOP_MARGIN, C_FILL);
    PutPixel(px - 1, OSC_TOP_MARGIN + 1, C_FILL);
    PutPixel(px + 1, OSC_TOP_MARGIN + 1, C_FILL);
  }

#define ADC_FS_HZ 9600U
  // Частота пика.
  // Эффективная Fs после децимации = ADC_FS_HZ / scale_t
  // Разрешение по частоте = Fs_eff / 128
  // F_peak = peak_bin * ADC_FS_HZ / (scale_t * 128)
  uint32_t fs_eff = ADC_FS_HZ / (uint32_t)osc.scale_t; // Гц
  uint32_t peak_hz = (uint32_t)peak_bin * fs_eff / 128U;

  if (peak_hz < 1000) {
    PrintSmallEx(LCD_XCENTER, SMALL_FONT_H * 3, POS_C, C_FILL, "Pk:%luHz",
                 peak_hz);
  } else {
    // Одна цифра после запятой: X.Y kHz
    uint32_t khz_int = peak_hz / 1000;
    uint32_t khz_frac = (peak_hz % 1000) / 100;
    PrintSmallEx(LCD_XCENTER, SMALL_FONT_H * 3, POS_C, C_FILL, "Pk:%lu.%lukHz",
                 khz_int, khz_frac);
  }
}

// ---------------------------------------------------------------------------
// Отрисовка — OOK
// ---------------------------------------------------------------------------
static void drawOOK(void) {
  if (osc.ook_count == 0) {
    PrintSmallEx(LCD_XCENTER, OSC_TOP_MARGIN + OSC_GRAPH_H / 2, POS_C, C_FILL,
                 "OOK...");
    return;
  }

  // Найти максимальную длительность для масштаба по X
  uint16_t max_dur = 1;
  for (int i = 0; i < osc.ook_count; i++)
    if (osc.ook_pulses[i].duration > max_dur)
      max_dur = osc.ook_pulses[i].duration;

  // Уровни Y для осциллограммы (верхняя половина экрана)
  int y_hi = OSC_TOP_MARGIN + 2;
  int y_lo = OSC_TOP_MARGIN + OSC_GRAPH_H / 2 - 6;

  // Рисуем импульсы
  int x = 0;
  for (int i = 0; i < osc.ook_count && x < LCD_WIDTH; i++) {
    int w =
        (int)((uint32_t)osc.ook_pulses[i].duration * (LCD_WIDTH - 1) / max_dur);
    if (w < 1)
      w = 1;
    int y = osc.ook_pulses[i].high ? y_hi : y_lo;

    // Горизонтальная линия импульса
    DrawLine(x, y, x + w, y, C_FILL);

    // Вертикальный фронт к следующему импульсу
    if (i + 1 < osc.ook_count) {
      int y_next = osc.ook_pulses[i + 1].high ? y_hi : y_lo;
      DrawLine(x + w, y, x + w, y_next, C_FILL);
    }
    x += w;
  }

  // Разделитель между осциллограммой и таблицей
  int y_sep = OSC_TOP_MARGIN + OSC_GRAPH_H / 2 - 2;
  for (int xi = 0; xi < LCD_WIDTH; xi += 2)
    PutPixel(xi, y_sep, C_FILL);

  // Таблица длительностей (нижняя половина): мкс при Fs~31 кГц → 32 мкс/сэмпл
  // Показываем первые 8 импульсов, по 16 пикселей на столбец
  int y_text = OSC_TOP_MARGIN + OSC_GRAPH_H / 2;
  int cols = LCD_WIDTH / 16; // сколько влезает
  if (cols > 8)
    cols = 8;
  for (int i = 0; i < osc.ook_count && i < cols; i++) {
    uint32_t us = (uint32_t)osc.ook_pulses[i].duration * 32;
    // Если больше 9999 мкс — показываем в мс
    if (us < 10000) {
      PrintSmallEx(i * 16, y_text, POS_L, C_FILL, "%c%lu",
                   osc.ook_pulses[i].high ? 'H' : 'L', us);
    } else {
      PrintSmallEx(i * 16, y_text, POS_L, C_FILL, "%c%lum",
                   osc.ook_pulses[i].high ? 'H' : 'L', us / 1000);
    }
  }

  // Счётчик и порог
  PrintSmallEx(LCD_WIDTH, y_text + SMALL_FONT_H, POS_R, C_FILL, "N:%d",
               osc.ook_count);

  PrintSmallEx(0, LCD_HEIGHT - 8, POS_L, C_FILL, "E:%d S:%d C:%d",
               (uint16_t)(osc.ook_envelope_iir >> 8), osc.ook_state,
               osc.ook_counter);
}

// ---------------------------------------------------------------------------
// Отрисовка — статус
// ---------------------------------------------------------------------------
static void drawStatus(void) {
  char buf[16];
  mhzToS(buf, RADIO_GetParam(ctx, PARAM_FREQUENCY));

  const char *mode_str = osc.mode == MODE_FFT   ? "FFT"
                         : osc.mode == MODE_OOK ? "OOK"
                                                : "OSC";
  PrintSmallEx(0, SMALL_FONT_H * 2, POS_L, C_FILL, "%s", mode_str);
  PrintSmallEx(LCD_XCENTER, SMALL_FONT_H * 2, POS_C, C_FILL, "%s", buf);
  PrintSmallEx(0, SMALL_FONT_H * 3, POS_L, C_FILL,
               osc.dc_offset ? "DC" : "RAW");

  if (osc.mode == MODE_WAVE) {
    PrintSmallEx(LCD_WIDTH, SMALL_FONT_H * 3, POS_R, C_FILL, "V:%d T:%d",
                 osc.scale_v, osc.scale_t);
    PrintSmallEx(LCD_WIDTH, SMALL_FONT_H * 4, POS_R, C_FILL, "T:%d",
                 osc.trigger_level);
  } else if (osc.mode == MODE_OOK) {
    PrintSmallEx(LCD_WIDTH, SMALL_FONT_H * 3, POS_R, C_FILL, "THR:%d",
                 osc.ook_threshold);
  } else {
    PrintSmallEx(LCD_WIDTH, SMALL_FONT_H * 3, POS_R, C_FILL, "T:%d",
                 osc.scale_t);
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
  } else if (osc.mode == MODE_FFT) {
    drawSpectrum();
  } else {
    drawOOK();
  }

  drawStatus();
  REGSMENU_Draw();
}
