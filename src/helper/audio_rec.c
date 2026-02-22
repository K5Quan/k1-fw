/* audio_rec.c — запись и воспроизведение аудио
 *
 * Поток данных:
 *
 *   ЗАПИСЬ:
 *   ADC DMA → AUDIO_IO_Update() → rec_sink()
 *                                     │ (main loop, НЕ ISR — можно всё)
 *                                     ▼
 *                               stage_buf[]   ← ring staging buffer
 *                                     │
 *                               AREC_Update() ← main loop
 *                                     │
 *                               Storage_SaveMultiple() → LFS → SPI flash
 *
 *   ВОСПРОИЗВЕДЕНИЕ:
 *   AREC_Update() → lfs read → prefetch_buf[]
 *       (main loop)                   │
 *                                     ▼
 *                               play_source()  ← DMA ISR (только memcpy!)
 *                                     │
 *                               DAC DMA → PA4 → speaker
 */

#include "audio_rec.h"
#include "../driver/bk4829.h"
#include "../driver/gpio.h"
#include "../driver/uart.h"
#include "../helper/audio_io.h"
#include "../settings.h"
#include "storage.h"
#include <string.h>

// ---------------------------------------------------------------------------
// Заголовок файла (8 байт, смещение 0)
// ---------------------------------------------------------------------------
#define AREC_MAGIC 0x43455241UL // "AREC" little-endian
#define AREC_HDR_SIZE 8U        // 4 байта magic + 4 байта count
#define AREC_DATA_OFF AREC_HDR_SIZE // сэмплы начинаются с байта 8

// ---------------------------------------------------------------------------
// Staging буфер (запись)
// Синк вызывается из main loop → запись в staging и flash без ограничений.
// ---------------------------------------------------------------------------
static uint8_t stage_buf[AREC_STAGE_SIZE];
static uint32_t stage_head; // следующий свободный индекс в stage_buf
static uint32_t rec_flash_off; // смещение следующей записи в файл (байты от 0)
static uint32_t rec_count;     // всего записано сэмплов

// ---------------------------------------------------------------------------
// Prefetch буфер (воспроизведение)
// play_source() вызывается из DMA ISR — только читает, не трогает lfs!
// AREC_Update() в main loop заполняет слоты заранее.
//
// Структура: кольцевой массив из AREC_PREFETCH_SLOTS слотов по
// AUDIO_IO_DAC_BLOCK байт.
// ---------------------------------------------------------------------------
static uint8_t prefetch_buf[AREC_PREFETCH_SLOTS][AUDIO_IO_DAC_BLOCK];
static volatile uint8_t pf_read_idx;  // ISR читает отсюда
static volatile uint8_t pf_write_idx; // main loop пишет сюда
static volatile uint8_t pf_ready;     // кол-во готовых слотов

static uint32_t play_flash_off; // смещение следующего чтения из файла
static uint32_t play_total; // всего сэмплов в файле
static uint32_t play_done;  // воспроизведено сэмплов

// ---------------------------------------------------------------------------
// Общее состояние
// ---------------------------------------------------------------------------
static ARecState arec_state = AREC_IDLE;

// ---------------------------------------------------------------------------
// Вспомогательные: запись заголовка
// ---------------------------------------------------------------------------
static bool write_header(uint32_t sample_count) {
  uint8_t hdr[AREC_HDR_SIZE];
  hdr[0] = 'A';
  hdr[1] = 'R';
  hdr[2] = 'E';
  hdr[3] = 'C';
  hdr[4] = (uint8_t)(sample_count);
  hdr[5] = (uint8_t)(sample_count >> 8);
  hdr[6] = (uint8_t)(sample_count >> 16);
  hdr[7] = (uint8_t)(sample_count >> 24);
  // Пишем в самое начало файла (item_size=1, start_num=0)
  return Storage_SaveMultiple(AREC_FILENAME, 0, hdr, 1, AREC_HDR_SIZE);
}

static bool read_header(uint32_t *out_count) {
  uint8_t hdr[AREC_HDR_SIZE];
  if (!Storage_LoadMultiple(AREC_FILENAME, 0, hdr, 1, AREC_HDR_SIZE))
    return false;
  if (hdr[0] != 'A' || hdr[1] != 'R' || hdr[2] != 'E' || hdr[3] != 'C')
    return false;
  *out_count = (uint32_t)hdr[4] | ((uint32_t)hdr[5] << 8) |
               ((uint32_t)hdr[6] << 16) | ((uint32_t)hdr[7] << 24);
  return true;
}

// ---------------------------------------------------------------------------
// Синк записи (main loop — можно делать что угодно)
// ---------------------------------------------------------------------------
static void rec_sink(const uint16_t *buf, uint32_t n) {
  if (arec_state != AREC_RECORDING)
    return;

  for (uint32_t i = 0; i < n; i++) {
    if (rec_count >= AREC_MAX_SAMPLES) {
      // Достигли лимита — автостоп
      AREC_StopRecording();
      return;
    }
    // 12-bit → 8-bit: просто сдвиг вправо на 4
    stage_buf[stage_head++] = (uint8_t)(buf[i] >> 4);
    rec_count++;

    // Staging буфер заполнен — сбросить на флеш
    if (stage_head >= AREC_STAGE_SIZE) {
      Storage_SaveMultiple(AREC_FILENAME,
                           rec_flash_off, // смещение в байтах
                           stage_buf, 1, AREC_STAGE_SIZE);
      rec_flash_off += AREC_STAGE_SIZE;
      stage_head = 0;
    }
  }
}

// ---------------------------------------------------------------------------
// Источник воспроизведения (DMA ISR — только memcpy из готового слота!)
// ---------------------------------------------------------------------------
static uint32_t play_source(uint16_t *buf, uint32_t n) {
  if (pf_ready == 0) {
    // Prefetch underrun — данных нет, тишина
    for (uint32_t i = 0; i < n; i++)
      buf[i] = 2048;
    LogC(LOG_C_YELLOW, "AREC: prefetch underrun");
    return n;
  }

  // Распаковываем 8-bit → 12-bit из текущего слота
  const uint8_t *src = prefetch_buf[pf_read_idx];
  uint32_t copy = (n < AUDIO_IO_DAC_BLOCK) ? n : AUDIO_IO_DAC_BLOCK;

  for (uint32_t i = 0; i < copy; i++)
    buf[i] = (uint16_t)src[i] << 4; // 8-bit → 12-bit

  // Дополнить если n > AUDIO_IO_DAC_BLOCK (не должно быть, но на всякий)
  for (uint32_t i = copy; i < n; i++)
    buf[i] = 2048;

  pf_read_idx = (pf_read_idx + 1) % AREC_PREFETCH_SLOTS;
  pf_ready--;
  play_done += copy;

  // Конец файла
  if (play_done >= play_total)
    return 0; // сигнал audio_io: источник завершён

  return copy;
}

// ---------------------------------------------------------------------------
// AREC_Init
// ---------------------------------------------------------------------------
void AREC_Init(void) {
  arec_state = AREC_IDLE;
  stage_head = 0;
  rec_flash_off = 0;
  rec_count = 0;
  pf_read_idx = 0;
  pf_write_idx = 0;
  pf_ready = 0;
  play_flash_off = 0;
  play_total = 0;
  play_done = 0;
}

// ---------------------------------------------------------------------------
// AREC_Update (main loop)
// ---------------------------------------------------------------------------
void AREC_Update(void) {
  if (arec_state == AREC_PLAYING) {
    // Заполняем свободные prefetch-слоты
    while (pf_ready < AREC_PREFETCH_SLOTS &&
           play_flash_off < (AREC_DATA_OFF + play_total)) {

      uint32_t bytes_left = (AREC_DATA_OFF + play_total) - play_flash_off;
      uint32_t to_read =
          (bytes_left < AUDIO_IO_DAC_BLOCK) ? bytes_left : AUDIO_IO_DAC_BLOCK;

      if (to_read == 0)
        break;

      bool ok = Storage_LoadMultiple(AREC_FILENAME, play_flash_off,
                                     prefetch_buf[pf_write_idx], 1, to_read);
      if (!ok) {
        LogC(LOG_C_RED, "AREC: prefetch read error at %lu", play_flash_off);
        AREC_StopPlayback();
        return;
      }

      // Если прочитали меньше блока — добить тишиной
      if (to_read < AUDIO_IO_DAC_BLOCK)
        memset(prefetch_buf[pf_write_idx] + to_read, 128,
               AUDIO_IO_DAC_BLOCK - to_read); // 128 = 0x80 → 2048 после <<4

      play_flash_off += to_read;
      pf_write_idx = (pf_write_idx + 1) % AREC_PREFETCH_SLOTS;
      pf_ready++;
    }

    // Проверяем: source вернул 0 → audio_io уже снял источник
    if (!AUDIO_IO_SourceActive() && arec_state == AREC_PLAYING) {
      LogC(LOG_C_BRIGHT_WHITE, "AREC: playback finished (%lu samples)",
           play_done);
      GPIO_DisableAudioPath();
      // BK4819_SetAF(gRxVfo->Modulation); // восстановить AF радио
      BK4819_ToggleAFDAC(true);
      BK4819_ToggleAFBit(true);
      arec_state = AREC_IDLE;
    }
  }
  // Запись: staging сбрасывается прямо в rec_sink(), AREC_Update не нужен.
  // Но при StopRecording хвост сбрасывается явно (см. ниже).
}

// ---------------------------------------------------------------------------
// AREC_StartRecording
// ---------------------------------------------------------------------------
bool AREC_StartRecording(void) {
  if (arec_state != AREC_IDLE)
    return false;

  stage_head = 0;
  rec_count = 0;
  // Данные начинаются после заголовка
  rec_flash_off = AREC_DATA_OFF;

  // Инициализируем файл: заголовок с count=0, место под MAX_SAMPLES байт
  // Storage_Init создаёт файл нужного размера нулями (показывает экран
  // создания)
  if (!Storage_Exists(AREC_FILENAME)) {
    Storage_Init(AREC_FILENAME, 1, AREC_HDR_SIZE + AREC_MAX_SAMPLES);
  }

  // Сброс заголовка (count = 0, перезаписываем)
  if (!write_header(0)) {
    LogC(LOG_C_RED, "AREC: cannot write header");
    return false;
  }

  arec_state = AREC_RECORDING;
  AUDIO_IO_SinkRegister(rec_sink);

  LogC(LOG_C_BRIGHT_WHITE, "AREC: recording started (max %lu sec)",
       (uint32_t)(AREC_MAX_SAMPLES / 9600));
  return true;
}

// ---------------------------------------------------------------------------
// AREC_StopRecording
// ---------------------------------------------------------------------------
void AREC_StopRecording(void) {
  if (arec_state != AREC_RECORDING)
    return;

  arec_state = AREC_IDLE;
  AUDIO_IO_SinkUnregister(rec_sink);

  // Сбросить хвост staging-буфера (неполный блок)
  if (stage_head > 0) {
    Storage_SaveMultiple(AREC_FILENAME, rec_flash_off, stage_buf, 1,
                         stage_head);
    rec_flash_off += stage_head;
    stage_head = 0;
  }

  // Обновить заголовок с реальным количеством сэмплов
  write_header(rec_count);

  uint32_t dur_ms = rec_count * 1000U / 9600U;
  LogC(LOG_C_BRIGHT_WHITE, "AREC: recording stopped. %lu samples, %lu.%lu sec",
       rec_count, dur_ms / 1000, (dur_ms % 1000) / 100);
}

// ---------------------------------------------------------------------------
// AREC_StartPlayback
// ---------------------------------------------------------------------------
bool AREC_StartPlayback(void) {
  if (arec_state != AREC_IDLE)
    return false;

  uint32_t count = 0;
  if (!read_header(&count) || count == 0) {
    LogC(LOG_C_RED, "AREC: no valid recording found");
    return false;
  }

  play_total = count;
  play_done = 0;
  play_flash_off = AREC_DATA_OFF; // читаем с начала данных
  pf_read_idx = 0;
  pf_write_idx = 0;
  pf_ready = 0;

  arec_state = AREC_PLAYING;

  // Мутируем BK4819 чтобы не мешал
  // BK4819_SetAF(BK4819_AF_MUTE);
  BK4819_ToggleAFDAC(false);
  BK4819_ToggleAFBit(false);
  GPIO_EnableAudioPath();

  // Prefetch первые слоты до старта DMA (без задержки в ISR)
  AREC_Update();

  AUDIO_IO_SourceSet(play_source);

  uint32_t dur_ms = count * 1000U / 9600U;
  LogC(LOG_C_BRIGHT_WHITE, "AREC: playback started. %lu samples, %lu.%lu sec",
       count, dur_ms / 1000, (dur_ms % 1000) / 100);
  return true;
}

// ---------------------------------------------------------------------------
// AREC_StopPlayback
// ---------------------------------------------------------------------------
void AREC_StopPlayback(void) {
  if (arec_state != AREC_PLAYING)
    return;

  AUDIO_IO_SourceClear();
  GPIO_DisableAudioPath();
  // BK4819_SetAF(gRxVfo->Modulation);
  BK4819_ToggleAFDAC(true);
  BK4819_ToggleAFBit(true);

  arec_state = AREC_IDLE;
  LogC(LOG_C_BRIGHT_WHITE, "AREC: playback stopped at %lu/%lu samples",
       play_done, play_total);
}

// ---------------------------------------------------------------------------
// AREC_GetInfo / AREC_GetDurationMs
// ---------------------------------------------------------------------------
ARecInfo AREC_GetInfo(void) {
  ARecInfo info;
  info.state = arec_state;
  info.file_exists = Storage_Exists(AREC_FILENAME);

  if (arec_state == AREC_RECORDING) {
    info.sample_count = rec_count;
    info.duration_samples = AREC_MAX_SAMPLES;
  } else if (arec_state == AREC_PLAYING) {
    info.sample_count = play_done;
    info.duration_samples = play_total;
  } else {
    info.sample_count = 0;
    info.duration_samples = 0;
    // Попробовать прочитать из файла
    if (info.file_exists) {
      uint32_t c = 0;
      if (read_header(&c))
        info.duration_samples = c;
    }
  }
  return info;
}

uint32_t AREC_GetDurationMs(void) {
  uint32_t c = 0;
  if (read_header(&c))
    return c * 1000U / 9600U;
  return 0;
}
