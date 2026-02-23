/*
 * ook.c — OOK декодер для демодулированного AM выхода BK4819
 *
 * Принцип детекции несущей:
 *
 *   Noise:   fast ≈ slow  →  fast - slow ≈ 0   →  carrier = 0
 *   OOK=1:   fast → HIGH  →  fast - slow > THR  →  carrier = 1
 *   OOK=0:   fast падает  →  fast - slow < THR  →  carrier = 0
 *
 *   fast (τ≈1мс) следит за мгновенным уровнем.
 *   slow (τ≈100мс) следит за фоновым шумом.
 *   Их разница — и есть "есть сигнал или нет".
 */

#include "ook.h"
#include "../driver/uart.h"
#include <stdint.h>
#include <string.h>

/* ═══════════════════════════════════════════════════════════════
 *  НАСТРОЙКИ
 * ═══════════════════════════════════════════════════════════════ */

#define SAMPLE_RATE 9600u

/* Fast IIR: τ ≈ 2^FAST_SHIFT / Fs
 * FAST_SHIFT=4 → τ ≈ 1.7 мс — быстро следит за уровнем OOK бита  */
#define FAST_SHIFT 1

/* Slow IIR: τ ≈ 2^SLOW_SHIFT / Fs
 * SLOW_SHIFT=10 → τ ≈ 107 мс — медленно следит за фоном/шумом     */
#define SLOW_SHIFT 10

/* Гистерезис в единицах АЦП.
 * carrier=OFF → ON когда (fast - slow) > +HYST_ON
 * carrier=ON  → OFF когда (fast - slow) < -HYST_OFF
 *
 * По логам: шумовой фон 3064..4095, fast≈slow≈3750.
 * Когда OOK=1: fast → 4095, разница ≈ 345.
 * HYST_ON=100 = ~29% от разницы — надёжно выше шумовых флуктуаций.
 *
 * Если ложные срабатывания  → увеличь HYST_ON.
 * Если пропускает биты      → уменьши HYST_ON.                     */
#define HYST_ON 30
#define HYST_OFF 15

/* ── Автодетект битрейта ────────────────────────────────────── */
#define HIST_SIZE 256u
#define HIST_MIN_PULSES 8u
#define GCD_TOLERANCE 2u
#define SPB_MIN 4u
#define SPB_MAX 192u
#define SPB_CONFIRM_VOTES 2u

/* ── Декодер пакетов ────────────────────────────────────────── */
#define MAX_BITS 256u
#define IDLE_BITS 12u

/* ═══════════════════════════════════════════════════════════════
 *  СОСТОЯНИЕ
 * ═══════════════════════════════════════════════════════════════ */

typedef struct {
  uint32_t hist[HIST_SIZE];
  uint32_t pulse_count;
  uint32_t run_cnt;
  bool last_carrier;
  uint32_t spb;
  uint32_t spb_votes;
} baud_detect_t;

typedef struct {
  /* Детектор несущей */
  int32_t fast_acc; /* быстрый IIR (Q FAST_SHIFT)  */
  int32_t slow_acc; /* медленный IIR (Q SLOW_SHIFT) */
  bool carrier;

  /* Декодер */
  uint32_t idle_samples;
  uint32_t bit_sample_cnt;
  uint32_t bit_ones;
  bool in_packet;
  uint8_t packet[MAX_BITS / 8];
  uint32_t bit_idx;
} ook_state_t;

static baud_detect_t g_bd;
static ook_state_t g_ook;

void (*ookHandler)(const uint8_t *data, uint16_t nbytes);

/* ═══════════════════════════════════════════════════════════════
 *  АВТОДЕТЕКТ БИТРЕЙТА
 * ═══════════════════════════════════════════════════════════════ */

static uint32_t gcd_approx(uint32_t a, uint32_t b) {
  while (b > 1u) {
    uint32_t r = a % b;
    if (r > b - GCD_TOLERANCE) {
      a = b;
      b = 0u;
      break;
    }
    if (r < GCD_TOLERANCE) {
      a = b;
      b = 0u;
      break;
    }
    a = b;
    b = r;
  }
  return a;
}

static uint32_t histogram_gcd(const uint32_t *hist) {
  uint32_t result = 0u;
  for (uint32_t i = SPB_MIN; i < HIST_SIZE; i++) {
    if (!hist[i])
      continue;
    result = result ? gcd_approx(i, result) : i;
    if (result < SPB_MIN)
      return 0u;
  }
  return result;
}

static uint32_t baud_detect_push(baud_detect_t *bd, bool carrier) {
  if (carrier == bd->last_carrier) {
    bd->run_cnt++;
    return (bd->spb_votes >= SPB_CONFIRM_VOTES) ? bd->spb : 0u;
  }

  uint32_t len = bd->run_cnt;
  bd->run_cnt = 1u;
  bd->last_carrier = carrier;

  if (len >= SPB_MIN && len < HIST_SIZE) {
    bd->hist[len]++;
    bd->pulse_count++;
  }

  if (bd->pulse_count < HIST_MIN_PULSES)
    return 0u;
  if (bd->pulse_count % 4u)
    return (bd->spb_votes >= SPB_CONFIRM_VOTES) ? bd->spb : 0u;

  uint32_t c = histogram_gcd(bd->hist);
  if (c >= SPB_MIN && c <= SPB_MAX) {
    if (c == bd->spb)
      bd->spb_votes++;
    else {
      bd->spb = c;
      bd->spb_votes = 1u;
    }
  }
  return (bd->spb_votes >= SPB_CONFIRM_VOTES) ? bd->spb : 0u;
}

/* ═══════════════════════════════════════════════════════════════
 *  ДЕКОДЕР ПАКЕТОВ
 * ═══════════════════════════════════════════════════════════════ */

static void push_bit(ook_state_t *s, bool one) {
  if (s->bit_idx >= MAX_BITS)
    return;
  uint32_t bi = s->bit_idx >> 3u;
  uint32_t bp = 7u - (s->bit_idx & 7u);
  if (one)
    s->packet[bi] |= (uint8_t)(1u << bp);
  else
    s->packet[bi] &= ~(uint8_t)(1u << bp);
  s->bit_idx++;
}

static void flush_packet(ook_state_t *s) {
  if (s->bit_idx >= 8u && ookHandler)
    ookHandler(s->packet, (uint16_t)((s->bit_idx + 7u) >> 3u));
  memset(s->packet, 0, sizeof(s->packet));
  s->bit_idx = 0u;
  s->bit_sample_cnt = 0u;
  s->bit_ones = 0u;
  s->idle_samples = 0u;
  s->in_packet = false;
}

uint32_t ook_get_bitrate(void) {
  if (g_bd.spb_votes < SPB_CONFIRM_VOTES || !g_bd.spb)
    return 0u;
  return SAMPLE_RATE / g_bd.spb;
}

/* ═══════════════════════════════════════════════════════════════
 *  ОСНОВНОЙ КОЛБЕК
 * ═══════════════════════════════════════════════════════════════ */

void ook_sink(const uint16_t *buf, uint32_t n) {
  ook_state_t *s = &g_ook;
  baud_detect_t *bd = &g_bd;

  static uint32_t dbg_samples = 0;
  static int32_t dbg_diff_max = INT32_MIN;
  static int32_t dbg_diff_min = INT32_MAX;

  for (uint32_t i = 0u; i < n; i++) {
    int32_t x = (int32_t)(uint32_t)buf[i];

    /* ── 1. Fast IIR (мгновенный уровень) ──────────────────────
     *
     *  fast_acc хранит fast * 2^FAST_SHIFT.
     *  Шаг: fast_acc += x - (fast_acc >> FAST_SHIFT)
     *  При равновесии: fast_acc >> FAST_SHIFT = среднее(x) за ~τ_fast
     *
     *  Во время OOK=1 (клипинг у 4095):
     *    fast быстро поднимается к 4095.
     *  Во время OOK=0 (шум, ~3200):
     *    fast быстро падает к 3200.                              */
    s->fast_acc += x - (s->fast_acc >> FAST_SHIFT);
    int32_t fast = s->fast_acc >> FAST_SHIFT;

    /* ── 2. Slow IIR (фоновый шум) ─────────────────────────────
     *
     *  slow_acc хранит slow * 2^SLOW_SHIFT.
     *  Медленно отслеживает фоновый уровень (шум + DC).
     *  За время OOK-пакета (десятки мс) slow почти не меняется.  */
    s->slow_acc += x - (s->slow_acc >> SLOW_SHIFT);
    int32_t slow = s->slow_acc >> SLOW_SHIFT;

    /* ── 3. Гистерезисный компаратор на разности ────────────────
     *
     *  diff = fast - slow:
     *    Шум в эфире: fast ≈ slow → diff ≈ 0      → carrier = 0
     *    OOK=1:       fast >> slow → diff > HYST_ON → carrier = 1
     *    OOK=0:       fast ≈ slow → diff < HYST_OFF → carrier = 0
     *
     *  Это работает при любом абсолютном уровне сигнала:
     *  не важно, 2000 или 4000, важна только разница.            */
    int32_t diff = fast - slow;

    if (!s->carrier && diff > HYST_ON)
      s->carrier = true;
    if (s->carrier && diff < -HYST_OFF)
      s->carrier = false;
    bool carrier = s->carrier;

    /* ── 4. Автодетект битрейта ─────────────────────────────── */
    uint32_t spb = baud_detect_push(bd, carrier);
    if (!spb)
      goto dbg;

    /* ── 5. Конец пакета ────────────────────────────────────── */
    if (!carrier) {
      if (s->in_packet && ++s->idle_samples >= IDLE_BITS * spb)
        flush_packet(s);
    } else {
      s->idle_samples = 0u;
    }

    /* ── 6. Битовый семплер ─────────────────────────────────── */
    s->bit_sample_cnt++;
    if (carrier)
      s->bit_ones++;
    if (s->bit_sample_cnt < spb)
      goto dbg;

    {
      bool bit_val = (s->bit_ones > spb / 2u);
      s->bit_sample_cnt = 0u;
      s->bit_ones = 0u;

      if (!s->in_packet) {
        if (bit_val) {
          s->in_packet = true;
          push_bit(s, 1u);
        }
      } else {
        if (s->bit_idx >= MAX_BITS)
          flush_packet(s);
        else
          push_bit(s, bit_val);
      }
    }

  dbg:
    if (diff > dbg_diff_max)
      dbg_diff_max = diff;
    if (diff < dbg_diff_min)
      dbg_diff_min = diff;
  }

  /* Лог раз в секунду:
   * Смотри на diff_max при нажатии кнопки — это и есть рабочая разница.
   * Выставь HYST_ON ≈ 30-50% от diff_max.                        */
  dbg_samples += n;
  if (dbg_samples >= SAMPLE_RATE) {
    dbg_samples = 0;
    int32_t fast = s->fast_acc >> FAST_SHIFT;
    int32_t slow = s->slow_acc >> SLOW_SHIFT;
    Log("fast=%ld slow=%ld diff=%ld | diff_max=%ld diff_min=%ld | carrier=%d "
        "spb=%lu",
        (long)fast, (long)slow, (long)(fast - slow), (long)dbg_diff_max,
        (long)dbg_diff_min, (int)s->carrier, (unsigned long)ook_get_bitrate());
    dbg_diff_max = INT32_MIN;
    dbg_diff_min = INT32_MAX;
  }
}

/* ═══════════════════════════════════════════════════════════════
 *  API
 * ═══════════════════════════════════════════════════════════════ */

void ook_reset(void) {
  /* Сохраняем fast/slow — уже сошлись */
  int32_t fast_acc = g_ook.fast_acc;
  int32_t slow_acc = g_ook.slow_acc;
  memset(&g_bd, 0, sizeof(g_bd));
  memset(&g_ook, 0, sizeof(g_ook));
  g_ook.fast_acc = fast_acc;
  g_ook.slow_acc = slow_acc;
}
