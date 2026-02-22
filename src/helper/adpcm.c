// IMA ADPCM codec — embedded implementation
// Standard IMA/DVI ADPCM, совместим с WAV IMA ADPCM chunks
#include "adpcm.h"
#include <string.h>

// ---------------------------------------------------------------------------
// Таблицы (ROM, const → уходят во flash)
// ---------------------------------------------------------------------------

// Таблица шагов квантизации (89 значений)
static const uint16_t STEP_TABLE[89] = {
        7,     8,     9,    10,    11,    12,    13,    14,
       16,    17,    19,    21,    23,    25,    28,    31,
       34,    37,    41,    45,    50,    55,    60,    66,
       73,    80,    88,    97,   107,   118,   130,   143,
      157,   173,   190,   209,   230,   253,   279,   307,
      337,   371,   408,   449,   494,   544,   598,   658,
      724,   796,   876,   963,  1060,  1166,  1282,  1411,
     1552,  1707,  1878,  2066,  2272,  2499,  2749,  3024,
     3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,
     7132,  7845,  8630,  9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767
};

// Таблица изменения индекса шага по 4-bit коду (0..7, зеркально для 8..15)
static const int8_t INDEX_TABLE[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8
};

// ---------------------------------------------------------------------------
// Внутренние функции
// ---------------------------------------------------------------------------

static inline int16_t clamp16(int32_t v) {
    if (v >  32767) return  32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

static inline uint8_t clamp_index(int16_t idx) {
    if (idx < 0)  return 0;
    if (idx > 88) return 88;
    return (uint8_t)idx;
}

// Закодировать один сэмпл → 4-bit nibble, обновить состояние
static uint8_t encode_sample(ADPCM_State *s, int16_t sample) {
    uint16_t step = STEP_TABLE[s->step_index];

    // разность предсказания
    int32_t diff = (int32_t)sample - s->predictor;
    uint8_t nibble = 0;

    if (diff < 0) {
        nibble = 8;
        diff = -diff;
    }

    // квантизация: 3 бита мантиссы
    if (diff >= step)          { nibble |= 4; diff -= step; }
    step >>= 1;
    if (diff >= step)          { nibble |= 2; diff -= step; }
    step >>= 1;
    if (diff >= step)          { nibble |= 1; }

    // обновляем предсказание через декодирование того же nibble
    step = STEP_TABLE[s->step_index];
    int32_t delta = step >> 3;
    if (nibble & 4) delta += step;
    if (nibble & 2) delta += step >> 1;
    if (nibble & 1) delta += step >> 2;

    if (nibble & 8)
        s->predictor = clamp16((int32_t)s->predictor - delta);
    else
        s->predictor = clamp16((int32_t)s->predictor + delta);

    s->step_index = clamp_index((int16_t)s->step_index + INDEX_TABLE[nibble]);

    return nibble & 0x0F;
}

// Декодировать один 4-bit nibble → сэмпл, обновить состояние
static int16_t decode_nibble(ADPCM_State *s, uint8_t nibble) {
    uint16_t step = STEP_TABLE[s->step_index];

    int32_t delta = step >> 3;
    if (nibble & 4) delta += step;
    if (nibble & 2) delta += step >> 1;
    if (nibble & 1) delta += step >> 2;

    if (nibble & 8)
        s->predictor = clamp16((int32_t)s->predictor - delta);
    else
        s->predictor = clamp16((int32_t)s->predictor + delta);

    s->step_index = clamp_index((int16_t)s->step_index + INDEX_TABLE[nibble]);

    return s->predictor;
}

// ---------------------------------------------------------------------------
// Публичный API
// ---------------------------------------------------------------------------

void ADPCM_Reset(ADPCM_State *state) {
    state->predictor  = 0;
    state->step_index = 0;
}

void ADPCM_EncodeBlock(ADPCM_State *state,
                       const int16_t *samples,
                       uint8_t       *out)
{
    // --- Заголовок блока (4 байта): predictor (LE16) + step_index + pad ---
    // Первый сэмпл блока используется как начальный predictor
    state->predictor  = samples[0];
    // step_index сохраняем как есть (продолжаем поток)

    out[0] = (uint8_t)((uint16_t)state->predictor & 0xFF);
    out[1] = (uint8_t)((uint16_t)state->predictor >> 8);
    out[2] = state->step_index;
    out[3] = 0; // зарезервировано

    // --- Данные: первый сэмпл уже в заголовке, кодируем начиная с [1] ---
    // Упаковка: lo nibble = нечётный индекс, hi nibble = чётный
    // (совместимо с Microsoft IMA ADPCM WAV)
    uint8_t *p = out + ADPCM_HEADER_BYTES;

    // Первый сэмпл блока кодируется «вхолостую» — он уже в predictor
    // Кодируем сэмплы 1..127 (127 сэмплов = нечётное, поэтому добавляем фиктивный 0)
    for (int i = 0; i < ADPCM_DATA_BYTES; i++) {
        uint8_t lo = encode_sample(state, (2*i+1 < ADPCM_SAMPLES_PER_BLOCK) ? samples[2*i+1] : 0);
        uint8_t hi = encode_sample(state, (2*i+2 < ADPCM_SAMPLES_PER_BLOCK) ? samples[2*i+2] : 0);
        p[i] = (hi << 4) | lo;
    }
}

void ADPCM_DecodeBlock(ADPCM_State *state,
                       const uint8_t *in,
                       int16_t       *samples)
{
    // --- Читаем заголовок ---
    state->predictor  = (int16_t)((uint16_t)in[0] | ((uint16_t)in[1] << 8));
    state->step_index = clamp_index((int16_t)in[2]);

    // Первый сэмпл блока берём прямо из заголовка
    samples[0] = state->predictor;

    // --- Декодируем данные ---
    const uint8_t *p = in + ADPCM_HEADER_BYTES;

    for (int i = 0; i < ADPCM_DATA_BYTES; i++) {
        uint8_t lo = p[i] & 0x0F;
        uint8_t hi = (p[i] >> 4) & 0x0F;

        int idx_lo = 2*i + 1;
        int idx_hi = 2*i + 2;

        if (idx_lo < ADPCM_SAMPLES_PER_BLOCK)
            samples[idx_lo] = decode_nibble(state, lo);
        if (idx_hi < ADPCM_SAMPLES_PER_BLOCK)
            samples[idx_hi] = decode_nibble(state, hi);
    }
}
