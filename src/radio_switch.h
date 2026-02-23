#pragma once

/**
 * @file radio_switch.h
 * @brief Управление переключением приёмников и режимами их питания.
 *
 * Вся логика переключения BK4819 / BK1080 / SI4732 живёт здесь,
 * чтобы radio.c не знал деталей работы с железом.
 *
 * Модель питания для каждого чипа:
 *
 *  OFF  <──────────────────────────────────> ACTIVE
 *         (только при смене VFO / sleep)      (принимает, аудио открыто)
 *
 *   OFF ──init──> IDLE ──tune──> ACTIVE
 *    ^             │
 *    └──shutdown───┘
 *
 *  BK4819 дополнительно поддерживает SLEEP (REG37 power-down),
 *  что экономит ~5 мА по сравнению с IDLE.
 */

#include "inc/vfo.h" /* VFOContext, Radio */
#include <stdbool.h>
#include <stdint.h>


/* ── Public API ──────────────────────────────────────────────────────────── */

/**
 * Однократная инициализация при старте.
 * Поднимает BK4819, BK1080 оставляет спать, SI4732 не трогает.
 */
void RXSW_Init(RadioSwitchCtx *sw);

/**
 * Главная точка входа: переключить активный приёмник на тот, что нужен VFO.
 *
 * Алгоритм:
 *  1. Определяем целевой чип из ctx->radio_type.
 *  2. Выключаем аудио со всех чипов.
 *  3. Ненужные чипы переводим в экономичный режим (SLEEP или OFF).
 *  4. Нужный чип переводим в ACTIVE (инициализируем при необходимости).
 *  5. Если vfo_is_open — открываем аудио на нужном чипе.
 *
 * @param sw          Состояние коммутатора.
 * @param ctx         Контекст VFO (частота, тип модуляции и т.д.).
 * @param vfo_is_open Признак открытого сигнала (squelch open).
 */
void RXSW_SwitchTo(RadioSwitchCtx *sw, const VFOContext *ctx, bool vfo_is_open);

/**
 * Открыть / закрыть аудио для уже активного приёмника.
 * Безопасно вызывать повторно с тем же значением (идемпотентна).
 *
 * @param sw     Состояние коммутатора.
 * @param source Чип-источник звука.
 * @param open   true = включить динамик, false = выключить.
 */
void RXSW_SetAudio(RadioSwitchCtx *sw, Radio source, bool open);

/**
 * Перевести все чипы в максимально экономичный режим.
 * Вызывается перед отключением экрана или входом в deep-sleep MCU.
 */
void RXSW_SuspendAll(RadioSwitchCtx *sw);

/**
 * Вернуть конкретный чип из SLEEP/IDLE в ACTIVE.
 * Используется, например, при пробуждении из suspend.
 *
 * @param sw  Состояние коммутатора.
 * @param r   Целевой чип.
 * @param ctx Контекст VFO для настройки частоты.
 */
void RXSW_WakeReceiver(RadioSwitchCtx *sw, Radio r, const VFOContext *ctx);

/**
 * Текущий уровень питания заданного чипа.
 */
ReceiverPowerState RXSW_GetPowerState(const RadioSwitchCtx *sw, Radio r);

/**
 * Текущий источник аудио (RADIO_* или 0xFF если никто).
 */
uint8_t RXSW_GetAudioSource(const RadioSwitchCtx *sw);
