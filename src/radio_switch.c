/**
 * @file radio_switch.c
 * @brief Переключение приёмников и управление питанием.
 *
 * Состояние питания каждого чипа отслеживается в RadioSwitchCtx,
 * а не в разрозненных статических переменных. Это гарантирует,
 * что функции включения/выключения всегда вызываются в нужном порядке
 * и не делают лишних операций.
 *
 * Последовательность при переключении (пример BK4819 → SI4732):
 *
 *   1. Закрыть аудио BK4819   (AUDIO_ToggleSpeaker OFF, AF-DAC off)
 *   2. BK4819: ACTIVE → SLEEP (REG_37 power-down, ≈5 мА экономии)
 *   3. SI4732: OFF/SLEEP → ACTIVE (PowerUp + TuneTo)
 *   4. Открыть аудио SI4732   (AUDIO_ToggleSpeaker ON)
 */

#include "radio_switch.h"

#include "driver/audio.h"
#include "driver/bk1080.h"
#include "driver/bk4819-regs.h"
#include "driver/bk4829.h"
#include "driver/si473x.h"
#include "driver/systick.h"
#include "driver/uart.h"
#include "external/printf/printf.h"
#include "settings.h"

/* ── Вспомогательные макросы ─────────────────────────────────────────────── */

/** Пустой источник аудио. */
#define AUDIO_NONE 0xFF

/* ══════════════════════════════════════════════════════════════════════════ */
/*  Низкоуровневое управление BK4819                                         */
/* ══════════════════════════════════════════════════════════════════════════ */

/**
 * Перевести BK4819 из любого состояния в ACTIVE.
 * Если чип уже ACTIVE — только сбросить DSP (BK4819_RX_TurnOn).
 */
static void bk4819_power_up(ReceiverPowerState from, const VFOContext *ctx) {
  if (from == RXPWR_OFF) {
    // Первый старт — полная инициализация чипа
    BK4819_Init();
    BK4819_ToggleGpioOut(BK4819_GPIO0_PIN28_RX_ENABLE, true);
  }

  // OFF и SLEEP физически одинаковы для BK4819 (оба = REG_37 0x1D00)
  // поэтому REG_37 нужно восстанавливать в обоих случаях
  if (from == RXPWR_SLEEP || from == RXPWR_OFF) {
    BK4819_WriteRegister(BK4819_REG_37, 0x9D1F);
    SYSTICK_DelayMs(2);
  }

  BK4819_RX_TurnOn();

  BK4819_ToggleAFDAC(false);
  BK4819_ToggleAFBit(false);

  LogC(LOG_C_BRIGHT_YELLOW, "[RXSW] BK4819 ACTIVE (from %d)", (int)from);
}

/**
 * BK4819: ACTIVE/IDLE → SLEEP.
 * Экономия ~5 мА по сравнению с IDLE при выключенном DSP.
 */
static void bk4819_sleep(void) {
  BK4819_Sleep();
  LogC(LOG_C_BRIGHT_YELLOW, "[RXSW] BK4819 SLEEP");
}

/**
 * BK4819: ACTIVE → IDLE (DSP off, PLL работает, быстрый старт).
 * Используется, когда переключение ожидается скоро (dual-watch scan).
 */
static void bk4819_idle(void) {
  BK4819_Idle(); /* REG_30 = 0x0000 */
  LogC(LOG_C_BRIGHT_YELLOW, "[RXSW] BK4819 IDLE");
}

/* ══════════════════════════════════════════════════════════════════════════ */
/*  Низкоуровневое управление BK1080                                          */
/* ══════════════════════════════════════════════════════════════════════════ */

static void bk1080_power_up(ReceiverPowerState from, const VFOContext *ctx) {
  if (from >= RXPWR_IDLE) {
    /* Уже инициализирован — просто настраиваем частоту. */
    BK4819_SelectFilter(ctx->frequency);
    BK1080_SetFrequency(ctx->frequency);
  } else {
    /* Полная инициализация: сначала Init, потом Mute(false). */
    BK4819_SelectFilter(ctx->frequency);
    BK1080_Init(ctx->frequency, true);
    BK1080_Mute(false);
  }
  LogC(LOG_C_BRIGHT_YELLOW, "[RXSW] BK1080 ACTIVE (from %d)", (int)from);
}

static void bk1080_power_off(void) {
  BK1080_Init(0, false); /* Мьютит и выключает. */
  LogC(LOG_C_BRIGHT_YELLOW, "[RXSW] BK1080 OFF");
}

/* ══════════════════════════════════════════════════════════════════════════ */
/*  Низкоуровневое управление SI4732                                          */
/* ══════════════════════════════════════════════════════════════════════════ */

static void si4732_power_up(ReceiverPowerState from, const VFOContext *ctx) {
  (void)from;

  bool is_ssb = (ctx->radio_type == RADIO_SI4732) &&
                ((SI47XX_MODE)ctx->modulation == SI47XX_LSB ||
                 (SI47XX_MODE)ctx->modulation == SI47XX_USB);

  if (is_ssb) {
    SI47XX_PatchPowerUp();
  } else {
    SI47XX_PowerUp();
  }

  LogC(LOG_C_BRIGHT_YELLOW, "[RXSW] SI4732 ACTIVE (from %d)", (int)from);
}

static void si4732_power_off(void) {
  SI47XX_PowerDown();
  LogC(LOG_C_BRIGHT_YELLOW, "[RXSW] SI4732 OFF");
}

/* ══════════════════════════════════════════════════════════════════════════ */
/*  Управление аудиотрактом                                                   */
/* ══════════════════════════════════════════════════════════════════════════ */

/**
 * Закрыть аудио со всех чипов и выключить динамик.
 * Вызывается перед любым переключением, чтобы не было щелчков.
 */
static void audio_silence(RadioSwitchCtx *sw) {
  if (sw->audio_source == AUDIO_NONE) {
    return;
  }

  if (sw->audio_source == RADIO_BK4819) {
    BK4819_ToggleAFBit(false);
    BK4819_ToggleAFDAC(false);
  }

  AUDIO_ToggleSpeaker(false);
  SYSTICK_DelayMs(8); /* дать усилителю замолчать */

  sw->audio_source = AUDIO_NONE;
}

/**
 * Открыть аудио для заданного чипа.
 * Всегда вызывайте audio_silence() перед этим.
 */
static void audio_open(RadioSwitchCtx *sw, Radio source) {
  if (source == RADIO_BK4819) {
    BK4819_ToggleAFDAC(true);
    BK4819_ToggleAFBit(true);
    SYSTICK_DelayMs(8);
  }

  AUDIO_ToggleSpeaker(true);
  sw->audio_source = (uint8_t)source;
}

/* ══════════════════════════════════════════════════════════════════════════ */
/*  Перевод конкретного чипа в нужный уровень питания                        */
/* ══════════════════════════════════════════════════════════════════════════ */

/**
 * Установить нужный уровень питания для указанного чипа.
 * Функция знает текущее состояние (from) и целевое (to) и делает
 * минимально необходимые действия для перехода.
 *
 * @param sw  Коммутатор (для обновления поля состояния).
 * @param r   Чип.
 * @param to  Желаемое целевое состояние.
 * @param ctx Контекст VFO (нужен при переходе в ACTIVE).
 */
static void set_power_state(RadioSwitchCtx *sw, Radio r, ReceiverPowerState to,
                            const VFOContext *ctx) {
  ReceiverPowerState *cur = NULL;

  switch (r) {
  case RADIO_BK4819:
    cur = &sw->bk4819;
    break;
  case RADIO_BK1080:
    cur = &sw->bk1080;
    break;
  case RADIO_SI4732:
    cur = &sw->si4732;
    break;
  default:
    return;
  }

  if (*cur == to) {
    return; /* уже в нужном состоянии */
  }

  /* ── Переходы ──────────────────────────────────────────────────────── */
  switch (r) {
  case RADIO_BK4819:
    if (to == RXPWR_ACTIVE) {
      bk4819_power_up(*cur, ctx);
    } else if (to == RXPWR_SLEEP) {
      bk4819_sleep();
    } else if (to == RXPWR_IDLE) {
      bk4819_idle();
    } else { /* OFF */
      bk4819_sleep(); /* через SLEEP, чтобы корректно завершить PLL */
      to = RXPWR_SLEEP;
    }
    break;

  case RADIO_BK1080:
    if (to == RXPWR_ACTIVE) {
      bk1080_power_up(*cur, ctx);
    } else if (to <=
               RXPWR_SLEEP) { /* SLEEP или OFF — для BK1080 одно и то же */
      bk1080_power_off();
      to = RXPWR_OFF; /* приводим к OFF, т.к. нет реального SLEEP */
    }
    break;

  case RADIO_SI4732:
    if (to == RXPWR_ACTIVE) {
      si4732_power_up(*cur, ctx);
    } else if (to <= RXPWR_SLEEP) {
      si4732_power_off();
      to = RXPWR_OFF;
    }
    break;

  default:
    break;
  }

  *cur = to;
}

/* ══════════════════════════════════════════════════════════════════════════ */
/*  Public API */
/* ══════════════════════════════════════════════════════════════════════════ */

void RXSW_Init(RadioSwitchCtx *sw) {
  sw->bk4819 = RXPWR_OFF;
  sw->bk1080 = RXPWR_OFF;
  sw->si4732 = RXPWR_OFF;
  sw->audio_source = AUDIO_NONE;

  /* BK4819 всегда поднимаем при старте. */
  set_power_state(sw, RADIO_BK4819, RXPWR_ACTIVE, NULL);

  /* BK1080 инициализируем, но сразу выключаем. */
  BK1080_Init(0, false);

  /* SI4732 не трогаем — включится по требованию. */

  Log("[RXSW] Init done");
}

void RXSW_SwitchTo(RadioSwitchCtx *sw, const VFOContext *ctx,
                   bool vfo_is_open) {
  Radio target = ctx->radio_type;

  Log("[RXSW] Switch -> %s, open=%d",
      (target == RADIO_BK4819)   ? "BK4819"
      : (target == RADIO_BK1080) ? "BK1080"
                                 : "SI4732",
      (int)vfo_is_open);

  /* 1. Заглушить аудио — до любых переключений питания. */
  audio_silence(sw);

  /* 2. Ненужные чипы — в экономичный режим.
   *
   *  Стратегия питания:
   *  - BK4819: уходит в SLEEP (не просто IDLE), если другой чип становится
   *    активным. SLEEP экономит ~5 мА и не мешает быстрому старту (~2 мс).
   *  - BK1080/SI4732: выключаем полностью (OFF), т.к. у них нет SLEEP.
   *    Реинициализация занимает ~50–100 мс, что приемлемо.
   */
  if (target != RADIO_BK4819) {
    set_power_state(sw, RADIO_BK4819, RXPWR_SLEEP, NULL);
  }
  if (target != RADIO_BK1080) {
    set_power_state(sw, RADIO_BK1080, RXPWR_OFF, NULL);
  }
  if (target != RADIO_SI4732) {
    set_power_state(sw, RADIO_SI4732, RXPWR_OFF, NULL);
  }

  /* 3. Нужный чип — в ACTIVE. */
  set_power_state(sw, target, RXPWR_ACTIVE, ctx);

  /* 4. Открыть аудио, если сигнал есть. */
  if (vfo_is_open) {
    audio_open(sw, target);
  }
}

void RXSW_SetAudio(RadioSwitchCtx *sw, Radio source, bool open) {
  if (!open) {
    if (sw->audio_source == (uint8_t)source) {
      audio_silence(sw);
    }
    return;
  }

  /* Не открывать заново, если уже открыто для того же источника. */
  if (sw->audio_source == (uint8_t)source) {
    return;
  }

  audio_silence(sw);
  audio_open(sw, source);
}

void RXSW_SuspendAll(RadioSwitchCtx *sw) {
  Log("[RXSW] Suspend all");
  audio_silence(sw);

  /* BK4819 — в глубокий SLEEP. */
  if (sw->bk4819 > RXPWR_SLEEP) {
    set_power_state(sw, RADIO_BK4819, RXPWR_SLEEP, NULL);
  }

  /* BK1080/SI4732 — полностью выключить. */
  set_power_state(sw, RADIO_BK1080, RXPWR_OFF, NULL);
  set_power_state(sw, RADIO_SI4732, RXPWR_OFF, NULL);
}

void RXSW_WakeReceiver(RadioSwitchCtx *sw, Radio r, const VFOContext *ctx) {
  Log("[RXSW] Wake %d", (int)r);
  set_power_state(sw, r, RXPWR_ACTIVE, ctx);
}

ReceiverPowerState RXSW_GetPowerState(const RadioSwitchCtx *sw, Radio r) {
  switch (r) {
  case RADIO_BK4819:
    return sw->bk4819;
  case RADIO_BK1080:
    return sw->bk1080;
  case RADIO_SI4732:
    return sw->si4732;
  default:
    return RXPWR_OFF;
  }
}

uint8_t RXSW_GetAudioSource(const RadioSwitchCtx *sw) {
  return sw->audio_source;
}
