#include "fsk.h"
#include "../driver/bk4829.h"
#include "../driver/systick.h"
#include "../driver/uart.h"
#include <string.h>

#define PACKET_SIZE 36 // ← Исправлено с 72 на 36 (как в официальном драйвере)
#define MSG_SIZE (PACKET_SIZE * 2 - 2)

static uint16_t tx_buf[PACKET_SIZE];
static uint16_t rx_buf[PACKET_SIZE];
static char msg[PACKET_SIZE * 2 + 1]; // +1 для '\0'
static uint16_t rx_idx = 0;
static uint16_t tx_idx = 0;

void pack_string(const char *str, uint16_t *packed, size_t *out_len) {
  size_t len = strlen(str);
  size_t packed_len = (len + 1) / 2;

  for (size_t i = 0; i < packed_len; i++) {
    uint16_t val = (uint8_t)str[i * 2];
    if (i * 2 + 1 < len) {
      val |= ((uint16_t)(uint8_t)str[i * 2 + 1]) << 8;
    }
    packed[i] = val;
  }

  *out_len = packed_len;
}

void unpack_string(const uint16_t *packed, size_t packed_len, char *str) {
  size_t max_bytes = packed_len * 2;
  for (size_t i = 0; i < max_bytes; i++) {
    str[i] = (i & 1) ? (packed[i >> 1] >> 8) : (packed[i >> 1] & 0xFF);
  }
  str[max_bytes] = '\0'; // безопасно, msg имеет размер MSG_SIZE+2
}

void FSK_DebugPrintRaw(void) {
  Log("Raw data (%u words):", rx_idx);
  for (uint16_t i = 0; i < rx_idx && i < 20; i++) {
    uint16_t word = rx_buf[i];
    char c1 = (word >> 8) & 0xFF;
    char c2 = word & 0xFF;

    // Преобразуем в печатные символы
    if (c1 < 32)
      c1 = '.';
    if (c1 > 126)
      c1 = '.';
    if (c2 < 32)
      c2 = '.';
    if (c2 > 126)
      c2 = '.';

    Log("  [%02u] = 0x%04X -> '%c%c'", i, word, c1, c2);
  }
}

void FSK_AnalyzePattern(void) {
  Log("=== Pattern Analysis ===");

  // Ищем повторяющиеся последовательности
  for (uint16_t i = 0; i < rx_idx && i < 10; i++) {
    uint16_t word = rx_buf[i];

    // Преобразуем в оба варианта
    char le1 = word & 0xFF;        // Little-endian: первый байт
    char le2 = (word >> 8) & 0xFF; // Little-endian: второй байт
    char be1 = (word >> 8) & 0xFF; // Big-endian: первый байт
    char be2 = word & 0xFF;        // Big-endian: второй байт

    // Проверяем, какой вариант даёт правильные буквы
    Log("[%02u] = 0x%04X | LE: '%c%c' | BE: '%c%c'", i, word,
        (le1 >= 32 && le1 <= 126) ? le1 : '.',
        (le2 >= 32 && le2 <= 126) ? le2 : '.',
        (be1 >= 32 && be1 <= 126) ? be1 : '.',
        (be2 >= 32 && be2 <= 126) ? be2 : '.');
  }

  // Проверяем на дубликаты
  Log("Duplicate check:");
  for (uint16_t i = 1; i < rx_idx && i < 10; i++) {
    if (rx_buf[i] == rx_buf[i - 1]) {
      Log("  [%u] and [%u] are duplicates: 0x%04X", i - 1, i, rx_buf[i]);
    }
  }
}

void FSK_Reset(void) {
  memset(tx_buf, 0, sizeof(tx_buf));
  memset(rx_buf, 0, sizeof(rx_buf));
  memset(msg, 0, sizeof(msg));
  tx_idx = rx_idx = 0;
  BK4819_FskClearFifo();
}

bool FSK_PrepareData(const char *data, size_t len) {
  if (len >= MSG_SIZE)
    return false;

  size_t packed_len;
  pack_string(data, tx_buf, &packed_len);

  if (packed_len < PACKET_SIZE) {
    memset(tx_buf + packed_len, 0,
           (PACKET_SIZE - packed_len) * sizeof(uint16_t));
  }
  tx_idx = PACKET_SIZE;
  return true;
}

bool FSK_Transmit(void) {
  if (tx_idx < PACKET_SIZE) {
    return false;
  }

  Log("TX START");

  // Подготовка
  BK4819_WriteRegister(BK4819_REG_3F, BK4819_REG_3F_FSK_TX_FINISHED);
  BK4819_WriteRegister(BK4819_REG_59, 0x8068); // Clear TX FIFO
  BK4819_WriteRegister(BK4819_REG_59, 0x0068); // Normal mode

  // Загрузка данных
  for (uint16_t i = 0; i < PACKET_SIZE; i++) {
    BK4819_WriteRegister(BK4819_REG_5F, tx_buf[i]);
  }

  SYSTICK_DelayMs(20);

  // Запуск передачи
  BK4819_WriteRegister(BK4819_REG_59, 0x2868); // Enable TX

  // Ждем завершения
  uint16_t timeout = 200; // 1 секунда
  while (timeout--) {
    if (BK4819_ReadRegister(BK4819_REG_0C) & 1u) {
      BK4819_WriteRegister(BK4819_REG_02, 0);
      if (BK4819_ReadRegister(BK4819_REG_02) & BK4819_REG_02_FSK_TX_FINISHED) {
        Log("TX DONE");
        break;
      }
    }
    SYSTICK_DelayMs(5);
  }

  // Сброс
  BK4819_ResetFSK();
  tx_idx = 0;

  return true;
}

bool FSK_ReadFifo(uint16_t irq) {
  static uint16_t rx_idx = 0;

  bool sync = (irq & BK4819_REG_02_FSK_RX_SYNC) != 0;
  bool fifo = (irq & BK4819_REG_02_FSK_FIFO_ALMOST_FULL) != 0;
  bool done = (irq & BK4819_REG_02_FSK_RX_FINISHED) != 0;

  if (sync) {
    rx_idx = 0;
    memset(rx_buf, 0, sizeof(rx_buf));
    Log("RX START");
    return false;
  }

  if (fifo) {
    // Читаем threshold количество слов (64 слова из REG_5E)
    uint16_t count = 64; // или читайте из регистра
    for (uint16_t i = 0; i < count && rx_idx < PACKET_SIZE; i++) {
      uint16_t word = BK4819_ReadRegister(BK4819_REG_5F);
      rx_buf[rx_idx++] = word;
    }
    return false;
  }

  if (done) {
    Log("RX DONE, got %u words", rx_idx);
    BK4819_FskClearFifo();

    // Проверка CRC
    uint16_t reg0b = BK4819_ReadRegister(BK4819_REG_0B);
    if (!(reg0b & 0x10)) {
      Log("CRC ERROR");
      rx_idx = 0;
      return false;
    }

    return (rx_idx >= 4); // минимум 4 слова (2 синка + данные)
  }

  return false;
}

bool FSK_ProcessPacket(void) {
  if (rx_idx < 4) // Минимум 2 слова
    return false;

  FSK_DebugPrintRaw();
  FSK_AnalyzePattern();

  BK4819_FskClearFifo();

  // Декодируем с правильным порядком байтов
  uint16_t msg_idx = 0;

  for (uint16_t i = 0; i < rx_idx && msg_idx < MSG_SIZE; i++) {
    uint16_t word = rx_buf[i];

    // Пропускаем нули и мусор
    if (word == 0x0000 || word == 0xFFFF || word == 0x012A) {
      continue;
    }

    // ВАЖНО: попробуем оба варианта порядка байтов!

    // Вариант 1: Little-endian (скорее всего правильный)
    // 0x4554 → 'T' (0x54), 'E' (0x45)
    msg[msg_idx++] = word & 0xFF; // Младший байт первый
    if (msg_idx < MSG_SIZE && (word >> 8) != 0) {
      msg[msg_idx++] = (word >> 8) & 0xFF; // Старший байт
    }

    // ИЛИ Вариант 2: Big-endian (то что сейчас)
    // msg[msg_idx++] = (word >> 8) & 0xFF;   // Старший байт
    // if (msg_idx < MSG_SIZE && (word & 0xFF) != 0) {
    //   msg[msg_idx++] = word & 0xFF;        // Младший байт
    // }
  }

  msg[msg_idx] = '\0';

  // Удаляем мусор и непечатные символы
  for (uint16_t i = 0; i < msg_idx; i++) {
    if (msg[i] < 32 || msg[i] > 126) {
      msg[i] = ' ';
    }
  }

  // Обрезаем пробелы в конце
  while (msg_idx > 0 && msg[msg_idx - 1] == ' ') {
    msg[--msg_idx] = '\0';
  }

  Log("Decoded: '%s' (len=%u)", msg, msg_idx);

  rx_idx = 0;
  BK4819_PrepareFSKReceive();

  return true;
}
/* bool FSK_ProcessPacket(void) {
  if (rx_idx < PACKET_SIZE)
    return false;

  BK4819_FskClearFifo();

  unpack_string(rx_buf, PACKET_SIZE, msg);
  rx_idx = 0;

  BK4819_PrepareFSKReceive();

  return true;
} */

const char *FSK_GetMessage(void) { return msg; }

uint16_t FSK_GetMessageLen(void) {
  for (uint16_t i = 0; i < MSG_SIZE; i++) {
    if (msg[i] == '\n' || msg[i] == '\r' || msg[i] == '\0') {
      return i;
    }
  }
  return MSG_SIZE;
}

const uint16_t *FSK_GetRawData(void) { return rx_buf; }

bool FSK_IsTxReady(void) { return tx_idx >= PACKET_SIZE; }

bool FSK_IsRxFull(void) { return rx_idx >= PACKET_SIZE; }
