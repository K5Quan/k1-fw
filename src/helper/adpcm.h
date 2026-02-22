#pragma once
// IMA ADPCM codec — embedded, no malloc, no float
// fs=9600Hz, 4bit/sample → 4.8 kbps (vs 153.6 kbps raw 16bit)
// Block size: 128 samples → 65 bytes encoded (4:1 ratio)

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Размер блока (в сэмплах). Должен совпадать с APRS_BUFFER_SIZE.
// 128 сэмплов → 4 байта заголовок + 64 байта данных = 68 байт/блок
// ---------------------------------------------------------------------------
#define ADPCM_SAMPLES_PER_BLOCK 128
#define ADPCM_HEADER_BYTES 4
#define ADPCM_DATA_BYTES (ADPCM_SAMPLES_PER_BLOCK / 2)
#define ADPCM_BLOCK_BYTES (ADPCM_HEADER_BYTES + ADPCM_DATA_BYTES) // 68

// ---------------------------------------------------------------------------
// Состояние кодера/декодера (сохраняется между блоками)
// ---------------------------------------------------------------------------
typedef struct {
  int16_t predictor; // предсказанное значение (−32768..32767)
  uint8_t step_index; // индекс в таблице шагов (0..88)
} ADPCM_State;

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------

// Сбросить состояние (вызвать перед началом потока)
void ADPCM_Reset(ADPCM_State *state);

// Закодировать один блок ADPCM_SAMPLES_PER_BLOCK сэмплов (int16) →
// ADPCM_BLOCK_BYTES байт samples : входной буфер, ADPCM_SAMPLES_PER_BLOCK
// элементов int16_t out     : выходной буфер, минимум ADPCM_BLOCK_BYTES байт
// state   : сохраняемое состояние
void ADPCM_EncodeBlock(ADPCM_State *state, const int16_t *samples,
                       uint8_t *out);

// Декодировать один блок ADPCM_BLOCK_BYTES байт → ADPCM_SAMPLES_PER_BLOCK
// сэмплов (int16) in      : входной буфер ADPCM_BLOCK_BYTES байт samples :
// выходной буфер ADPCM_SAMPLES_PER_BLOCK элементов int16_t state   :
// сохраняемое состояние (синхронизируется из заголовка блока)
void ADPCM_DecodeBlock(ADPCM_State *state, const uint8_t *in, int16_t *samples);

// Утилита: конвертировать 12-bit ADC → int16
// ADC даёт 0..4095, центр ~2048 (если DC смещение правильное)
static inline int16_t ADPCM_ADCtoS16(uint16_t adc_raw) {
  return (int16_t)((int32_t)adc_raw * 16 - 32768);
}

// Утилита: конвертировать int16 → DAC (0..4095)
static inline uint16_t ADPCM_S16toDAC(int16_t sample) {
  int32_t v = (int32_t)sample + 32768;
  v >>= 4; // 16bit → 12bit
  if (v < 0)
    v = 0;
  if (v > 4095)
    v = 4095;
  return (uint16_t)v;
}
