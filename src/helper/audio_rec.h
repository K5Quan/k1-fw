/* audio_rec.h — запись и воспроизведение аудио через storage + audio_io
 *
 * Формат файла (item_size=1, сырые байты):
 *   Байты  0..3  — магик "AREC"
 *   Байты  4..7  — uint32_t sample_count (little-endian)
 *   Байты  8..   — 8-bit samples (12-bit ADC >> 4), Fs = 9600 Hz
 *
 * Таким образом один байт = один сэмпл, Storage_SaveMultiple(name, offset, ...)
 * с item_size=1 отлично подходит.
 *
 * Ёмкость: 9600 байт/сек → 512 КБ ≈ 54 секунды максимум.
 *
 * Использование:
 *
 *   // Инициализация (один раз, после AUDIO_IO_Init)
 *   AREC_Init();
 *
 *   // В main loop — обязательно!
 *   AREC_Update();
 *
 *   // Запись
 *   AREC_StartRecording();
 *   ...
 *   AREC_StopRecording();
 *
 *   // Воспроизведение
 *   AREC_StartPlayback();
 *   ...
 *   AREC_StopPlayback();   // или само остановится по концу файла
 */

#ifndef AUDIO_REC_H
#define AUDIO_REC_H

#include <stdbool.h>
#include <stdint.h>

// Имя файла на LFS (можно переопределить)
#ifndef AREC_FILENAME
#define AREC_FILENAME "voice.raw"
#endif

// Максимальная длительность записи в сэмплах (54 сек при 9600 Гц)
#define AREC_MAX_SAMPLES (9600U * 5U)

// Размер staging-буфера (запись): сбрасываем на флеш каждые N байт.
// 256 байт = 26 мс. Меньше → чаще прерываемся на SPI флеш.
#define AREC_STAGE_SIZE 256U

// Размер prefetch-буфера (воспроизведение): два слота по DAC_BLOCK байт.
// Должен совпадать с AUDIO_IO_DAC_BLOCK из audio_io.h.
#ifndef AUDIO_IO_DAC_BLOCK
#define AUDIO_IO_DAC_BLOCK 64U
#endif
#define AREC_PREFETCH_SLOTS 4U
#define AREC_PREFETCH_BYTES (AREC_PREFETCH_SLOTS * AUDIO_IO_DAC_BLOCK)

typedef enum {
  AREC_IDLE,
  AREC_RECORDING,
  AREC_PLAYING,
} ARecState;

typedef struct {
  ARecState state;
  uint32_t sample_count; // записано/воспроизведено сэмплов
  uint32_t duration_samples; // всего сэмплов в файле (для плеера)
  bool file_exists; // есть ли файл на флеше
} ARecInfo;

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------

/** Инициализация. Вызвать один раз после AUDIO_IO_Init(). */
void AREC_Init(void);

/**
 * Вызывать из main loop на каждой итерации.
 * Выполняет: сброс staging-буфера на флеш (запись) и prefetch
 * (воспроизведение).
 */
void AREC_Update(void);

/** Начать запись. Предыдущий файл перезаписывается. */
bool AREC_StartRecording(void);

/** Остановить запись и сбросить хвост буфера на флеш. */
void AREC_StopRecording(void);

/** Начать воспроизведение записанного файла. */
bool AREC_StartPlayback(void);

/** Остановить воспроизведение досрочно. */
void AREC_StopPlayback(void);

/** Текущее состояние и статистика. */
ARecInfo AREC_GetInfo(void);

/** Длительность записанного файла в миллисекундах. */
uint32_t AREC_GetDurationMs(void);

#endif // AUDIO_REC_H
