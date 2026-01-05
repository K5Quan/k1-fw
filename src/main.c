#include "board.h"
#include "driver/fat.h"
#include "driver/flash_sync.h"
#include "driver/gpio.h"
#include "driver/py25q16.h"
#include "driver/systick.h"
#include "driver/uart.h"
#include "external/printf/printf.h"
#include "system.h"
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define SECTOR_SIZE 512
#define SECTORS_PER_CLUSTER 8 // 4KB кластеры = размер стирания!
#define CLUSTER_SIZE (SECTORS_PER_CLUSTER * SECTOR_SIZE)
// Комплексный стресс-тест файловой системы
// Исправленная версия stress_test_flash() с правильными задержками
void stress_test_flash(void) {
  printf("\n=== Flash Memory Stress Test ===\n");

  uint32_t start_time = Now();
  uint32_t total_operations = 0;
  uint32_t errors = 0;

  // 1. Тест последовательного чтения/записи
  printf("\n1. Sequential access test...\n");
  uint32_t test_address = 0x010000;

  for (int block = 0; block < 10; block++) {
    printf("  Block %d: ", block);

    uint8_t write_data[512];
    uint8_t read_data[512];

    for (int i = 0; i < 512; i++) {
      write_data[i] = (block * 512 + i) % 256;
    }

    uint32_t sector_addr = test_address + block * 4096;

    // КРИТИЧНО: Стираем
    PY25Q16_SectorErase(sector_addr);

    // КРИТИЧНО: Дополнительная пауза после стирания
    SYSTICK_DelayMs(100);
    total_operations++;

    // Записываем
    PY25Q16_WriteBuffer(sector_addr, write_data, 512, false);

    // КРИТИЧНО: Пауза после записи
    SYSTICK_DelayMs(50);
    total_operations++;

    // Читаем обратно
    PY25Q16_ReadBuffer(sector_addr, read_data, 512);
    total_operations++;

    // Проверяем
    bool ok = true;
    for (int i = 0; i < 512; i++) {
      if (write_data[i] != read_data[i]) {
        printf("Mismatch at byte %d: 0x%02X != 0x%02X\n", i, write_data[i],
               read_data[i]);
        ok = false;
        errors++;
        break;
      }
    }

    printf("%s\n", ok ? "OK" : "FAIL");

    // ВАЖНО: Пауза между блоками
    SYSTICK_DelayMs(200);
  }

  // 2. Тест случайного доступа
  printf("\n2. Random access test...\n");
  uint32_t random_addresses[] = {0x015000, 0x018000, 0x01C000, 0x020000,
                                 0x025000};

  for (int i = 0; i < 5; i++) {
    printf("  Address 0x%06lX: ", random_addresses[i]);

    // Стираем с паузой
    PY25Q16_SectorErase(random_addresses[i]);
    SYSTICK_DelayMs(100);

    // Записываем паттерн
    uint8_t pattern[256];
    for (int j = 0; j < 256; j++) {
      pattern[j] = (i * 256 + j) ^ 0x55;
    }

    PY25Q16_WriteBuffer(random_addresses[i], pattern, 256, false);
    SYSTICK_DelayMs(50);
    total_operations++;

    // Читаем и проверяем
    uint8_t verify[256];
    PY25Q16_ReadBuffer(random_addresses[i], verify, 256);
    total_operations++;

    bool ok = memcmp(pattern, verify, 256) == 0;
    printf("%s\n", ok ? "OK" : "FAIL");
    if (!ok)
      errors++;

    // Пауза между адресами
    SYSTICK_DelayMs(200);
  }

  // 3. Тест граничных условий
  printf("\n3. Boundary conditions test...\n");

  uint32_t page_boundary = 0x030000 - 10;
  PY25Q16_SectorErase(0x030000 - 4096);
  SYSTICK_DelayMs(100);

  uint8_t boundary_data[20];
  for (int i = 0; i < 20; i++) {
    boundary_data[i] = 0x80 + i;
  }

  printf("  Writing across page boundary at 0x%06lX...", page_boundary);
  PY25Q16_WriteBuffer(page_boundary, boundary_data, 20, false);
  SYSTICK_DelayMs(50);
  total_operations++;

  uint8_t boundary_check[20];
  PY25Q16_ReadBuffer(page_boundary, boundary_check, 20);
  total_operations++;

  bool boundary_ok = memcmp(boundary_data, boundary_check, 20) == 0;
  printf("%s\n", boundary_ok ? "OK" : "FAIL");
  if (!boundary_ok)
    errors++;

  SYSTICK_DelayMs(200);

  // 4. Тест производительности
  printf("\n4. Performance test...\n");

  uint32_t perf_size = 2048;
  uint8_t *perf_data = (uint8_t *)malloc(perf_size);
  uint8_t *perf_buffer = (uint8_t *)malloc(perf_size);

  if (perf_data && perf_buffer) {
    for (uint32_t i = 0; i < perf_size; i++) {
      perf_data[i] = i % 256;
    }

    uint32_t perf_addr = 0x040000;
    PY25Q16_SectorErase(perf_addr);
    SYSTICK_DelayMs(100);

    // Тест записи
    uint32_t write_start = Now();
    PY25Q16_WriteBuffer(perf_addr, perf_data, perf_size, false);
    uint32_t write_time = Now() - write_start;
    SYSTICK_DelayMs(50);
    total_operations++;

    // Тест чтения
    uint32_t read_start = Now();
    PY25Q16_ReadBuffer(perf_addr, perf_buffer, perf_size);
    uint32_t read_time = Now() - read_start;
    total_operations++;

    printf("  Write %u bytes: %lu ms (%.1f KB/s)\n", perf_size, write_time,
           (perf_size / 1024.0) / (write_time / 1000.0));
    printf("  Read %u bytes: %lu ms (%.1f KB/s)\n", perf_size, read_time,
           (perf_size / 1024.0) / (read_time / 1000.0));

    bool perf_ok = memcmp(perf_data, perf_buffer, perf_size) == 0;
    printf("  Data integrity: %s\n", perf_ok ? "OK" : "FAIL");
    if (!perf_ok)
      errors++;

    free(perf_data);
    free(perf_buffer);
  }

  // Итоги
  printf("\n=== Flash Test Results ===\n");
  uint32_t total_time = Now() - start_time;

  printf("Total test time: %lu ms\n", total_time);
  printf("Total operations: %lu\n", total_operations);
  printf("Errors detected: %lu\n", errors);

  if (total_operations > 0) {
    printf("Error rate: %.2f%%\n", (errors * 100.0) / total_operations);
    printf("Average operation time: %.1f ms\n",
           total_time / (float)total_operations);
  }

  printf("Flash memory test: %s\n", errors == 0 ? "PASS" : "FAIL");
}

// Также добавьте задержки в stress_test_fat()
void stress_test_fat(void) {
  printf("\n=== FAT Stress Test ===\n");

  uint32_t start_time = Now();
  uint32_t total_written = 0;
  uint32_t total_read = 0;
  int failures = 0;
  int successes = 0;

  // 1. Тест с множеством мелких файлов
  printf("\n1. Multiple small files test...\n");
  for (int i = 0; i < 10; i++) {
    char filename[16];
    sprintf(filename, "SMALL%03d.TXT", i);

    uint8_t data[50];
    for (int j = 0; j < sizeof(data); j++) {
      data[j] = (i * 10 + j) % 256;
    }

    if (usb_fs_write_file(filename, data, sizeof(data), false) == 0) {
      successes++;
      total_written += sizeof(data);
    } else {
      failures++;
      printf("  Failed to write %s\n", filename);
    }

    // КРИТИЧНО: Пауза между файлами
    SYSTICK_DelayMs(100);
  }
  printf("  Created %d small files, %d failures\n", successes, failures);

  // Остальной код теста аналогично - добавьте SYSTICK_DelayMs()
  // после каждой операции записи/стирания

  // ... (остальные тесты с паузами)
}

void fat_debug_lock_status(void) {
  printf("Flash lock status:\n");
  printf("  Busy: %s\n", flash_is_locked() ? "YES" : "NO");
}

void safe_file_operation_test(void) {
  printf("\n=== Safe File Operation Test ===\n");

  const char *filename = "SAFE.TXT";
  const char *test_data = "Test data for safe operations";

  // Проверяем блокировку перед операцией
  if (flash_is_locked()) {
    printf("Flash is locked, waiting...\n");
    uint32_t start = Now();
    while (flash_is_locked() && (Now() - start < 1000)) {
      SYSTICK_DelayMs(10);
    }
  }

  // Записываем файл
  printf("Writing file '%s'...\n", filename);
  int result = usb_fs_write_file(filename, (uint8_t *)test_data,
                                 strlen(test_data), false);

  if (result == 0) {
    printf("Write successful\n");

    // Читаем обратно
    uint8_t buffer[100];
    uint32_t size = sizeof(buffer);
    result = usb_fs_read_file(filename, buffer, &size);

    if (result == 0) {
      printf("Read successful: %lu bytes\n", size);
      printf("Data: '%.*s'\n", size, buffer);
    }
  }

  // Показываем статус блокировок
  fat_debug_lock_status();
}

// В main() замените вызовы:
int main(void) {
  SYSTICK_Init();
  BOARD_Init();
  UART_Init();

  usb_fs_init();

  printf("System initialized\n\n");

  while (1) {
    __WFI();
  }
}
