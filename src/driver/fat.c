#include "fat.h"
#include "../board.h"
#include "../external/printf/printf.h"
#include "py25q16.h"
#include "systick.h"
#include "uart.h"
#include <assert.h>
#include <stdbool.h>
#include <string.h>

#define _VOLUME_CREATE_DATE FAT_MK_DATE(2026, 1, 2)
#define _VOLUME_CREATE_TIME FAT_MK_TIME(12, 0, 0)

#define VOLUME_ID ((_VOLUME_CREATE_DATE) << 16) | (_VOLUME_CREATE_TIME)
#define VOLUME_LABEL "STORAGE    "
#define BPB_MEDIA 0xf8

// ВАЖНО: Размер стирания SPI флешки = 4096 байт (команда 0x20)
#define FLASH_ERASE_SIZE 4096

// Параметры файловой системы
#define FLASH_SIZE (2 * 1024 * 1024)             // 2MB
#define TOTAL_SECTORS (FLASH_SIZE / SECTOR_SIZE) // 4096 секторов
#define SECTORS_PER_CLUSTER 8 // 4KB кластеры = размер стирания!
#define RESERVED_SECTORS 8 // Измените с 1 на 8! (8 секторов = 4KB)
#define FAT_COPIES 2
#define ROOT_ENTRIES 512
#define SECTORS_PER_FAT 16 // 16 секторов = 8KB на FAT
#define CLUSTER_SIZE (SECTORS_PER_CLUSTER * SECTOR_SIZE)

// Вычисляемые значения
#define FAT_START_SECTOR RESERVED_SECTORS                       // 8
#define FAT2_START_SECTOR (FAT_START_SECTOR + SECTORS_PER_FAT)  // 24
#define ROOT_START_SECTOR (FAT2_START_SECTOR + SECTORS_PER_FAT) // 40
#define ROOT_SECTORS ((ROOT_ENTRIES * 32) / SECTOR_SIZE)        // 32
#define DATA_START_SECTOR (ROOT_START_SECTOR + ROOT_SECTORS)    // 72
#define DATA_SECTORS (TOTAL_SECTORS - DATA_START_SECTOR)

// Смещения в SPI флешке (выравнены по 4KB)
#define FLASH_FAT_OFFSET                                                       \
  (FAT_START_SECTOR * SECTOR_SIZE) // 8 * 512 = 0x1000 (4096)
#define FLASH_ROOT_OFFSET                                                      \
  (ROOT_START_SECTOR * SECTOR_SIZE) // 40 * 512 = 0x5000 (20480)
#define FLASH_DATA_OFFSET                                                      \
  (DATA_START_SECTOR * SECTOR_SIZE) // 72 * 512 = 0x9000 (36864)

// Проверка выравнивания
static_assert((FLASH_FAT_OFFSET % FLASH_ERASE_SIZE) == 0, "FAT not aligned");
static_assert((FLASH_ROOT_OFFSET % FLASH_ERASE_SIZE) == 0, "Root not aligned");
static_assert((FLASH_DATA_OFFSET % FLASH_ERASE_SIZE) == 0, "Data not aligned");
static_assert((SECTORS_PER_CLUSTER * SECTOR_SIZE) == FLASH_ERASE_SIZE,
              "Cluster size must match erase size");

// Кэш для одного сектора (экономия RAM)
static uint32_t cached_sector = 0xFFFFFFFF;
static bool cache_dirty = false;

// Boot sector (константа)
static const fat_boot_sector_t BOOT_SECTOR_RECORD = {
    .jump_boot = {0xeb, 0x3c, 0x90},
    .OEM_name = "MSWIN4.1",
    .sector_size = SECTOR_SIZE,
    .sectors_per_cluster = SECTORS_PER_CLUSTER,
    .reserved_sectors = RESERVED_SECTORS,
    .num_FATs = FAT_COPIES,
    .root_entries = ROOT_ENTRIES,
    .total_sectors16 = TOTAL_SECTORS,
    .media = BPB_MEDIA,
    .FAT_sectors16 = SECTORS_PER_FAT,
    .sectors_per_track = 63,
    .num_heads = 255,
    .drive_num = 0x80,
    .boot_signature = 0x29,
    .volume_ID = VOLUME_ID,
    .volume_label = VOLUME_LABEL,
    .fs_type = "FAT16   ",
};

static uint16_t read_fat_entry(uint16_t cluster);

// Кэш для одного сектора (экономия RAM)
static uint8_t sector_cache[SECTOR_SIZE];

// Статические переменные для отслеживания стираний
static uint32_t last_erased_sector = 0xFFFFFFFF;
static uint32_t last_erase_time = 0;
static uint32_t operation_counter = 0;
static bool needs_erase_check = false;

static void invalidate_cache_for_sector(uint32_t sector) {
  if (cached_sector == sector) {
    cached_sector = 0xFFFFFFFF;
    cache_dirty = false;
  }
}

// Функция для проверки необходимости стирания
static bool needs_erase(uint32_t sector, uint8_t *new_data) {
  uint8_t current_data[SECTOR_SIZE];
  uint32_t flash_addr = sector * SECTOR_SIZE;

  // Читаем текущие данные с флешки
  PY25Q16_ReadBuffer(flash_addr, current_data, SECTOR_SIZE);

  // Проверяем, можно ли записать без стирания
  // Флеш-память позволяет менять биты с 1 на 0 без стирания
  // Но если нужно поменять с 0 на 1 - требуется стирание
  for (uint32_t i = 0; i < SECTOR_SIZE; i++) {
    if ((current_data[i] & new_data[i]) != new_data[i]) {
      // Нужно поменять 0 на 1 - требуется стирание
      return true;
    }
  }

  return false;
}

#define ERASE_BLOCK_SIZE 4096

static uint8_t
    current_sector_data[SECTOR_SIZE]; // Reuse as global to save stack

static void smart_write_sector(uint32_t sector, uint8_t *new_data) {
  uint32_t flash_addr = sector * SECTOR_SIZE;
  uint32_t block_addr = flash_addr & ~(ERASE_BLOCK_SIZE - 1);
  uint32_t offset_in_block = flash_addr - block_addr;

  // Read current
  PY25Q16_ReadBuffer(flash_addr, current_sector_data, SECTOR_SIZE);

  // Check if erase needed
  bool needs_erase = false;
  for (int i = 0; i < SECTOR_SIZE; i++) {
    if ((current_sector_data[i] & new_data[i]) != new_data[i]) {
      needs_erase = true;
      break;
    }
  }

  if (!needs_erase) {
    PY25Q16_WriteBuffer(flash_addr, new_data, SECTOR_SIZE, false);
  } else {
    // Erase the block and write the new sector (no save - compromise for RAM)
    PY25Q16_SectorErase(block_addr);
    PY25Q16_WriteBuffer(flash_addr, new_data, SECTOR_SIZE, false);
    // Invalidate cache if affected
    for (uint32_t s = block_addr / SECTOR_SIZE;
         s < (block_addr + ERASE_BLOCK_SIZE) / SECTOR_SIZE; s++) {
      invalidate_cache_for_sector(s);
    }
  }
}

// Обновляем flush_cache
static void flush_cache(void) {
  if (cache_dirty && cached_sector != 0xFFFFFFFF) {
    uint32_t flash_addr = cached_sector * SECTOR_SIZE;

    operation_counter++;
    /* printf("flush_cache [%lu]: sector=%lu, addr=0x%06lx\n",
       operation_counter, cached_sector, flash_addr); */

    // Используем умную запись с проверкой необходимости стирания
    smart_write_sector(cached_sector, sector_cache);

    cache_dirty = false;
  }
}

static void read_sector_to_cache(uint32_t sector) {
  if (cached_sector == sector && !cache_dirty) {
    return; // Уже в кэше и данные актуальны
  }

  flush_cache(); // Сохранить старый кэш

  uint32_t flash_addr = sector * SECTOR_SIZE;

  // ПРОВЕРЯЕМ: если это сектор FAT или Root, убеждаемся что читаем актуальные
  // данные
  if (sector >= FAT_START_SECTOR && sector < DATA_START_SECTOR) {
    // Для FAT/Root читаем напрямую, минуя кэш
    PY25Q16_ReadBuffer(flash_addr, sector_cache, SECTOR_SIZE);
    cached_sector = sector;
    cache_dirty = false;
    return;
  }

  PY25Q16_ReadBuffer(flash_addr, sector_cache, SECTOR_SIZE);
  cached_sector = sector;
  cache_dirty = false;
}

static void usb_fs_format_raw(void) {
  Log("[FAT] Formatting...");

  cached_sector = 0xFFFFFFFF;
  cache_dirty = false;

  // 1. Boot Sector и Reserved Sectors (секторы 0-7)
  Log("[FAT] Writing boot sector and reserved sectors (0x0000-0x0FFF)...");

  // Стираем весь первый блок 4KB (секторы 0-7)
  PY25Q16_SectorErase(0);

  // Сектор 0: Boot sector
  memset(sector_cache, 0, SECTOR_SIZE);
  memcpy(sector_cache, &BOOT_SECTOR_RECORD, sizeof(BOOT_SECTOR_RECORD));
  sector_cache[510] = 0x55;
  sector_cache[511] = 0xAA;

  // Записываем boot sector с Append=false
  PY25Q16_WriteBuffer(0, sector_cache, SECTOR_SIZE, false);

  // Остальные reserved sectors (1-7) заполняем нулями
  memset(sector_cache, 0, SECTOR_SIZE);
  for (int i = 1; i < RESERVED_SECTORS; i++) {
    uint32_t addr = i * SECTOR_SIZE;
    PY25Q16_WriteBuffer(addr, sector_cache, SECTOR_SIZE, false);
  }

  // 2. FAT1 (секторы 8-23, 8KB = 2 блока по 4KB)
  Log("[FAT] Initializing FAT1 (0x1000-0x2FFF)...");

  // Стираем блоки FAT1
  PY25Q16_SectorErase(FLASH_FAT_OFFSET);          // Блок 0x1000-0x1FFF
  PY25Q16_SectorErase(FLASH_FAT_OFFSET + 0x1000); // Блок 0x2000-0x2FFF

  // Первый сектор FAT с special values
  memset(sector_cache, 0, SECTOR_SIZE);
  sector_cache[0] = BPB_MEDIA; // Media descriptor (0xF8)
  sector_cache[1] = 0xFF;      // EOF маркеры
  sector_cache[2] = 0xFF;
  sector_cache[3] = 0xFF; // EOF для кластера 1

  PY25Q16_WriteBuffer(FLASH_FAT_OFFSET, sector_cache, SECTOR_SIZE, false);

  // Остальные секторы FAT1 - нули
  memset(sector_cache, 0, SECTOR_SIZE);
  for (int i = 1; i < SECTORS_PER_FAT; i++) {
    uint32_t addr = FLASH_FAT_OFFSET + i * SECTOR_SIZE;
    PY25Q16_WriteBuffer(addr, sector_cache, SECTOR_SIZE, false);
  }

  // 3. FAT2 (секторы 24-39) - копия FAT1
  Log("[FAT] Copying FAT1 to FAT2 (0x3000-0x4FFF)...");

  // Стираем блоки FAT2
  PY25Q16_SectorErase(FLASH_FAT_OFFSET +
                      SECTORS_PER_FAT * SECTOR_SIZE); // 0x3000
  PY25Q16_SectorErase(FLASH_FAT_OFFSET + SECTORS_PER_FAT * SECTOR_SIZE +
                      0x1000); // 0x4000

  // Копируем FAT1 в FAT2
  for (int i = 0; i < SECTORS_PER_FAT; i++) {
    uint32_t src_addr = FLASH_FAT_OFFSET + i * SECTOR_SIZE;
    uint32_t dst_addr =
        FLASH_FAT_OFFSET + SECTORS_PER_FAT * SECTOR_SIZE + i * SECTOR_SIZE;

    uint8_t buffer[SECTOR_SIZE];
    PY25Q16_ReadBuffer(src_addr, buffer, SECTOR_SIZE);
    PY25Q16_WriteBuffer(dst_addr, buffer, SECTOR_SIZE, false);
  }

  // 4. Root Directory (секторы 40-71, 16KB = 4 блока по 4KB)
  Log("[FAT] Clearing root directory (0x5000-0x8FFF)...");

  // Стираем блоки root directory
  for (int block = 0; block < 4; block++) {
    uint32_t addr = FLASH_ROOT_OFFSET + block * 0x1000;
    PY25Q16_SectorErase(addr);
  }

  // Заполняем root directory нулями
  memset(sector_cache, 0, SECTOR_SIZE);
  for (int i = 0; i < ROOT_SECTORS; i++) {
    uint32_t addr = FLASH_ROOT_OFFSET + i * SECTOR_SIZE;
    PY25Q16_WriteBuffer(addr, sector_cache, SECTOR_SIZE, false);
  }

  Log("[FAT] Format complete");
}

static void print_fat_layout(void) {
  printf("FAT Layout:\n");
  printf("  Total sectors: %u\n", TOTAL_SECTORS);
  printf("  Reserved sectors: %u\n", RESERVED_SECTORS);
  printf("  FAT start: %u (0x%06lx)\n", FAT_START_SECTOR,
         (uint32_t)(FAT_START_SECTOR * SECTOR_SIZE));
  printf("  FAT2 start: %u (0x%06lx)\n", FAT2_START_SECTOR,
         (uint32_t)(FAT2_START_SECTOR * SECTOR_SIZE));
  printf("  Root start: %u (0x%06lx)\n", ROOT_START_SECTOR,
         (uint32_t)(ROOT_START_SECTOR * SECTOR_SIZE));
  printf("  Data start: %u (0x%06lx)\n", DATA_START_SECTOR,
         (uint32_t)(DATA_START_SECTOR * SECTOR_SIZE));
  printf("  FLASH offsets: FAT=0x%06lx, Root=0x%06lx, Data=0x%06lx\n",
         FLASH_FAT_OFFSET, FLASH_ROOT_OFFSET, FLASH_DATA_OFFSET);

  // Проверка выравнивания
  printf("  Alignment check:\n");
  printf("    FAT offset 0x%06lx %% 0x%04lx = 0x%04lx %s\n", FLASH_FAT_OFFSET,
         FLASH_ERASE_SIZE, FLASH_FAT_OFFSET % FLASH_ERASE_SIZE,
         (FLASH_FAT_OFFSET % FLASH_ERASE_SIZE == 0) ? "OK" : "FAIL");
  printf("    Root offset 0x%06lx %% 0x%04lx = 0x%04lx %s\n", FLASH_ROOT_OFFSET,
         FLASH_ERASE_SIZE, FLASH_ROOT_OFFSET % FLASH_ERASE_SIZE,
         (FLASH_ROOT_OFFSET % FLASH_ERASE_SIZE == 0) ? "OK" : "FAIL");
  printf("    Data offset 0x%06lx %% 0x%04lx = 0x%04lx %s\n", FLASH_DATA_OFFSET,
         FLASH_ERASE_SIZE, FLASH_DATA_OFFSET % FLASH_ERASE_SIZE,
         (FLASH_DATA_OFFSET % FLASH_ERASE_SIZE == 0) ? "OK" : "FAIL");
}

void usb_fs_format_safe(void) {
  Log("[FAT] Safe formatting...");

  cached_sector = 0xFFFFFFFF;
  cache_dirty = false;

  // 1. Boot sector
  Log("[FAT] Writing boot sector...");
  memset(sector_cache, 0, SECTOR_SIZE);
  memcpy(sector_cache, &BOOT_SECTOR_RECORD, sizeof(BOOT_SECTOR_RECORD));
  sector_cache[510] = 0x55;
  sector_cache[511] = 0xAA;

  PY25Q16_SectorErase(0);
  PY25Q16_WriteBuffer(0, sector_cache, SECTOR_SIZE, false);

  // 2. FAT1 (минимум 2 сектора для первых 256 кластеров)
  Log("[FAT] Init FAT1...");

  // Первый сектор FAT
  memset(sector_cache, 0, SECTOR_SIZE);
  sector_cache[0] = BPB_MEDIA;
  sector_cache[1] = 0xFF;
  sector_cache[2] = 0xFF;
  sector_cache[3] = 0xFF;
  // Остальное - нули (свободные кластеры)

  PY25Q16_SectorErase(FLASH_FAT_OFFSET);
  PY25Q16_WriteBuffer(FLASH_FAT_OFFSET, sector_cache, SECTOR_SIZE, false);

  // Второй сектор FAT - все нули (свободные кластеры)
  memset(sector_cache, 0, SECTOR_SIZE);
  PY25Q16_WriteBuffer(FLASH_FAT_OFFSET + SECTOR_SIZE, sector_cache, SECTOR_SIZE,
                      false);

  // 3. FAT2 - копия FAT1
  Log("[FAT] Init FAT2...");

  // Копируем первый сектор
  memset(sector_cache, 0, SECTOR_SIZE);
  sector_cache[0] = BPB_MEDIA;
  sector_cache[1] = 0xFF;
  sector_cache[2] = 0xFF;
  sector_cache[3] = 0xFF;

  PY25Q16_SectorErase(FLASH_FAT_OFFSET + SECTORS_PER_FAT * SECTOR_SIZE);
  PY25Q16_WriteBuffer(FLASH_FAT_OFFSET + SECTORS_PER_FAT * SECTOR_SIZE,
                      sector_cache, SECTOR_SIZE, false);

  // Второй сектор FAT2
  memset(sector_cache, 0, SECTOR_SIZE);
  PY25Q16_WriteBuffer(FLASH_FAT_OFFSET + SECTORS_PER_FAT * SECTOR_SIZE +
                          SECTOR_SIZE,
                      sector_cache, SECTOR_SIZE, false);

  // 4. Root directory - очищаем минимум 2 сектора
  Log("[FAT] Clear root dir...");
  memset(sector_cache, 0, SECTOR_SIZE);

  PY25Q16_SectorErase(FLASH_ROOT_OFFSET);
  PY25Q16_WriteBuffer(FLASH_ROOT_OFFSET, sector_cache, SECTOR_SIZE, false);

  // Второй сектор root
  PY25Q16_WriteBuffer(FLASH_ROOT_OFFSET + SECTOR_SIZE, sector_cache,
                      SECTOR_SIZE, false);

  Log("[FAT] Format completed");
}

void usb_fs_init(void) {
  Log("[FAT] Init");

  // Инициализация кэша
  cached_sector = 0xFFFFFFFF;
  cache_dirty = false;

  // Простая проверка: только boot signature
  uint8_t boot_check[512];
  PY25Q16_ReadBuffer(0, boot_check, 512);

  bool boot_valid = (boot_check[510] == 0x55 && boot_check[511] == 0xAA);

  if (!boot_valid) {
    LogC(LOG_C_BRIGHT_YELLOW, "[FAT] Invalid boot sector, formatting...");
    usb_fs_format_safe();
  } else {
    Log("[FAT] Boot sector valid");
  }

  // print_fat_layout();
}

// Прочитать FAT entry
static uint16_t read_fat_entry(uint16_t cluster) {
  uint32_t sector = FAT_START_SECTOR + (cluster * 2) / SECTOR_SIZE;
  uint32_t offset = (cluster * 2) % SECTOR_SIZE;

  // printf("read_fat_entry(%d)\n", cluster);

  read_sector_to_cache(sector);
  return (sector_cache[offset + 1] << 8) | sector_cache[offset];
}

// Записать FAT entry
static void write_fat_entry(uint16_t cluster, uint16_t value) {
  uint32_t sector = FAT_START_SECTOR + (cluster * 2) / SECTOR_SIZE;
  uint32_t offset = (cluster * 2) % SECTOR_SIZE;

  read_sector_to_cache(sector);

  // Проверяем, действительно ли нужно изменить значение
  uint16_t current_value =
      (sector_cache[offset + 1] << 8) | sector_cache[offset];
  if (current_value == value) {
    return; // Значение уже установлено
  }

  sector_cache[offset] = value & 0xFF;
  sector_cache[offset + 1] = value >> 8;
  cache_dirty = true;

  // Немедленно записываем изменения в FAT1
  flush_cache();

  // Копируем в FAT2
  sector = FAT2_START_SECTOR + (cluster * 2) / SECTOR_SIZE;
  read_sector_to_cache(sector);
  sector_cache[offset] = value & 0xFF;
  sector_cache[offset + 1] = value >> 8;
  cache_dirty = true;
  flush_cache();
}

// Найти свободный кластер
static uint16_t find_free_cluster(void) {
  for (uint16_t i = 2; i < (DATA_SECTORS / SECTORS_PER_CLUSTER); i++) {
    if (read_fat_entry(i) == 0) {
      LogC(LOG_C_BRIGHT_GREEN, "[FAT] Found free cluster: %u", i);
      return i;
    }
  }
  LogC(LOG_C_BRIGHT_GREEN, "[FAT] No free clusters found!");
  return 0;
}

// Освободить кластеры
static void free_clusters(uint16_t first_cluster) {
  uint16_t cluster = first_cluster;

  while (cluster >= 2 && cluster < 0xFFF8) {
    uint16_t next = read_fat_entry(cluster);
    write_fat_entry(cluster, 0);
    cluster = next;
  }
}

// Упрощенная версия allocate_clusters, если она еще где-то используется
static uint16_t allocate_clusters(uint16_t *first_cluster,
                                  uint32_t additional_bytes) {
  if (additional_bytes == 0) {
    return *first_cluster ? *first_cluster : 0;
  }

  printf("allocate_clusters: first_cluster=%u, additional_bytes=%lu\n",
         *first_cluster, additional_bytes);

  uint32_t clusters_needed =
      (additional_bytes + CLUSTER_SIZE - 1) / CLUSTER_SIZE;

  if (*first_cluster == 0) {
    // Новый файл
    *first_cluster = find_free_cluster();
    if (*first_cluster == 0) {
      printf("  No free clusters\n");
      return 0;
    }

    uint16_t current = *first_cluster;
    write_fat_entry(current, 0xFFFF);
    clusters_needed--;

    for (uint32_t i = 0; i < clusters_needed; i++) {
      uint16_t next = find_free_cluster();
      if (next == 0) {
        // Освобождаем уже выделенные
        free_clusters(*first_cluster);
        *first_cluster = 0;
        return 0;
      }
      write_fat_entry(current, next);
      write_fat_entry(next, 0xFFFF);
      current = next;
    }

    return current;
  } else {
    // Существующий файл - находим последний кластер
    uint16_t current = *first_cluster;
    uint16_t next;

    while (true) {
      next = read_fat_entry(current);
      if (next >= 0xFFF8) {
        break;
      }
      current = next;
    }

    // Добавляем новые кластеры
    for (uint32_t i = 0; i < clusters_needed; i++) {
      next = find_free_cluster();
      if (next == 0) {
        return current; // Частичное выделение
      }
      write_fat_entry(current, next);
      write_fat_entry(next, 0xFFFF);
      current = next;
    }

    return current;
  }
}

static int find_file_entry(const char *formatted_name, fat_dir_entry_t *entry) {
  // printf("find_file_entry: formatted_name='%.11s'\n", formatted_name);

  for (uint32_t sector = ROOT_START_SECTOR; sector < DATA_START_SECTOR;
       sector++) {
    read_sector_to_cache(sector);

    for (int i = 0; i < (SECTOR_SIZE / 32); i++) {
      fat_dir_entry_t *e = (fat_dir_entry_t *)(sector_cache + i * 32);

      if (e->name[0] == 0) {
        /* printf("  End of directory at sector %lu, entry %d (0x%02X)\n",
           sector, i, e->name[0]); */
        return -1;
      }

      if (e->name[0] != 0xE5 && !(e->attr & 0x08)) {
        /* printf("  Checking sector %lu, entry %d: '%.11s' (attr=0x%02X)\n",
               sector, i, e->name, e->attr); */

        if (memcmp(e->name, formatted_name, 11) == 0) {
          // printf("  Found at sector %lu, entry %d\n", sector, i);
          if (entry) {
            memcpy(entry, e, sizeof(fat_dir_entry_t));
          }
          return (sector - ROOT_START_SECTOR) * (SECTOR_SIZE / 32) + i;
        }
      }
    }
  }

  // printf("  File not found in entire root directory\n");
  return -1;
}

// Создать или обновить запись в root directory
static int update_file_entry(const char *name, const fat_dir_entry_t *entry) {
  int idx = find_file_entry(name, NULL);

  if (idx == -1) {
    // Найти свободную запись
    // printf("update_file_entry: Finding free entry for '%.11s'\n", name);

    for (uint32_t sector = ROOT_START_SECTOR; sector < DATA_START_SECTOR;
         sector++) {
      read_sector_to_cache(sector);

      for (int i = 0; i < (SECTOR_SIZE / 32); i++) {
        fat_dir_entry_t *e = (fat_dir_entry_t *)(sector_cache + i * 32);

        if (e->name[0] == 0 || e->name[0] == 0xE5) {
          /* printf("  Free entry found at sector %lu, entry %d (0x%02X)\n",
                 sector, i, e->name[0]); */

          memcpy(e, entry, sizeof(fat_dir_entry_t));

          // КРИТИЧНО: Если это была нулевая запись (конец директории),
          // нужно установить следующую запись как новый конец
          if (e->name[0] == 0) { // Это была нулевая запись
            // Проверяем, не последняя ли это запись в секторе
            if (i + 1 < (SECTOR_SIZE / 32)) {
              fat_dir_entry_t *next =
                  (fat_dir_entry_t *)(sector_cache + (i + 1) * 32);
              if (next->name[0] != 0) {
                /* printf("  Setting next entry (%d) as end of directory\n",
                       i + 1); */
                next->name[0] = 0;
              }
            } else {
              // Если это последняя запись в секторе, нужно обнулить
              // первую запись следующего сектора
              printf("  Warning: Last entry in sector, may need to clear next "
                     "sector\n");
            }
          }

          cache_dirty = true;
          flush_cache();
          // printf("  Entry written successfully\n");
          return 0;
        }
      }
    }
    printf("  ERROR: No free entries found!\n");
    return -1;
  }

  // Обновить существующую запись
  // printf("update_file_entry: Updating existing entry at idx %d\n", idx);

  uint32_t sector = ROOT_START_SECTOR + idx / (SECTOR_SIZE / 32);
  uint32_t offset = (idx % (SECTOR_SIZE / 32)) * 32;

  read_sector_to_cache(sector);
  memcpy(sector_cache + offset, entry, sizeof(fat_dir_entry_t));
  cache_dirty = true;
  flush_cache();

  return 0;
}

int usb_fs_write_file(const char *name, const uint8_t *data, uint32_t size,
                      bool append) {
  // printf("usb_fs_write_file: '%s', size=%lu, append=%d\n", name, size,
  // append);

  char formatted_name[12];
  fat_format_name(name, formatted_name);
  // printf("Formatted name: '%.11s'\n", formatted_name);

  // 1. Если файл уже существует и append=false, удаляем его
  fat_dir_entry_t entry;
  int idx = find_file_entry(formatted_name, &entry);

  if (idx != -1 && !append) {
    // printf("  File exists, deleting first...\n");
    int delete_result = usb_fs_delete_file(name);
    if (delete_result != 0) {
      LogC(LOG_C_RED, "[FAT] Failed to delete existing file\n");
      return -1;
    }
    idx = -1;
  }

  bool existed = (idx != -1);
  uint32_t old_size = 0;
  uint16_t first_cluster = 0;

  if (existed) {
    old_size = entry.file_size;
    first_cluster = entry.first_clusterLO;
    /* printf("  Appending to existing file: size=%lu, cluster=%u\n", old_size,
           first_cluster); */
  } else {
    old_size = 0;
    first_cluster = 0;
    // printf("  Creating new file\n");
  }

  uint32_t new_size = old_size + size;

  // 2. Если нет данных для записи
  if (size == 0) {
    memset(&entry, 0, sizeof(entry));
    memcpy(entry.name, formatted_name, 11);
    entry.attr = 0x20;
    entry.first_clusterLO = first_cluster;
    entry.file_size = new_size;
    entry.create_date = _VOLUME_CREATE_DATE;
    entry.create_time = _VOLUME_CREATE_TIME;
    entry.write_date = _VOLUME_CREATE_DATE;
    entry.write_time = _VOLUME_CREATE_TIME;

    return update_file_entry(formatted_name, &entry);
  }

  // 3. Выделяем кластеры
  uint32_t total_clusters_needed = (new_size + CLUSTER_SIZE - 1) / CLUSTER_SIZE;
  uint32_t existing_clusters = (old_size + CLUSTER_SIZE - 1) / CLUSTER_SIZE;
  uint32_t additional_clusters = total_clusters_needed - existing_clusters;

  /* printf("  old_size=%lu, new_size=%lu, existing_clusters=%lu, "
         "additional_clusters=%lu\n",
         old_size, new_size, existing_clusters, additional_clusters); */

  // Если нужно добавить кластеры
  if (additional_clusters > 0) {
    if (first_cluster == 0) {
      // Новый файл - выделяем первый кластер
      first_cluster = find_free_cluster();
      if (first_cluster == 0) {
        LogC(LOG_C_RED, "[FAT] No free cluster");
        return -1;
      }
      // printf("  Allocated cluster: %u\n", first_cluster);

      // Устанавливаем EOF для первого кластера
      write_fat_entry(first_cluster, 0xFFFF);
      additional_clusters--;

      // Выделяем остальные кластеры
      uint16_t current = first_cluster;
      for (uint32_t i = 0; i < additional_clusters; i++) {
        uint16_t next = find_free_cluster();
        if (next == 0) {
          free_clusters(first_cluster);
          LogC(LOG_C_RED, "[FAT] Out of space");
          return -1;
        }
        write_fat_entry(current, next);
        write_fat_entry(next, 0xFFFF);
        current = next;
      }
    } else {
      // Существующий файл - находим последний кластер
      uint16_t current = first_cluster;
      uint16_t next;

      while (true) {
        next = read_fat_entry(current);
        if (next >= 0xFFF8) {
          break;
        }
        current = next;
      }

      // Добавляем новые кластеры
      for (uint32_t i = 0; i < additional_clusters; i++) {
        next = find_free_cluster();
        if (next == 0) {
          LogC(LOG_C_RED, "[FAT] Out of space while extending");
          return -1;
        }
        write_fat_entry(current, next);
        write_fat_entry(next, 0xFFFF);
        current = next;
      }
    }
  }

  LogC(LOG_C_BRIGHT_GREEN,
       "[FAT] Writing file '%s', size=%lu, clusters needed=%lu", name, size,
       (size + CLUSTER_SIZE - 1) / CLUSTER_SIZE);

  if (first_cluster == 0) {
    printf("  ERROR: No cluster allocated!\n");
    return -1;
  }

  // printf("  First cluster: %u\n", first_cluster);

  // 4. Записываем данные
  uint32_t written = 0;
  uint16_t cluster = first_cluster;
  uint32_t offset_in_cluster = 0;

  // Если append, находим позицию в последнем кластере
  if (append && old_size > 0) {
    cluster = first_cluster;
    uint32_t remaining = old_size;

    while (remaining > CLUSTER_SIZE) {
      cluster = read_fat_entry(cluster);
      if (cluster >= 0xFFF8 || cluster < 2) {
        LogC(LOG_C_RED, "[FAT] Corrupted FAT chain");
        return -1;
      }
      remaining -= CLUSTER_SIZE;
    }
    offset_in_cluster = remaining;
  }

  // ПРИНУДИТЕЛЬНО СБРАСЫВАЕМ КЭШ ПЕРЕД ЗАПИСЬЮ БОЛЬШОГО ФАЙЛА
  if (size > SECTOR_SIZE) {
    flush_cache();
    cached_sector = 0xFFFFFFFF;
  }

  while (written < size) {
    LogC(LOG_C_BRIGHT_GREEN,
         "[FAT] Write loop: written=%lu, total=%lu, cluster=%u, offset=%lu\n",
         written, size, cluster, offset_in_cluster);
    uint32_t flash_addr =
        FLASH_DATA_OFFSET + (cluster - 2) * CLUSTER_SIZE + offset_in_cluster;
    uint32_t space_in_cluster = CLUSTER_SIZE - offset_in_cluster;
    uint32_t to_write = size - written;

    if (to_write > space_in_cluster) {
      to_write = space_in_cluster;
    }

    // Проверяем, нужно ли стирать кластер
    if (offset_in_cluster == 0 && (!append || old_size == 0)) {
      // Сбрасываем кэш перед каждой новой операцией записи
      flush_cache();
      cached_sector = 0xFFFFFFFF;

      uint8_t check_byte;
      PY25Q16_ReadBuffer(flash_addr, &check_byte, 1);

      if (check_byte != 0xFF) {
        // printf("  Erasing cluster %u at 0x%06lx\n", cluster, flash_addr);
        PY25Q16_SectorErase(flash_addr);
      }
    }

    // СБРАСЫВАЕМ КЭШ ПЕРЕД КАЖДОЙ ЗАПИСЬЮ
    flush_cache();

    /* printf("  Writing %lu bytes to cluster %u at 0x%06lx\n", to_write,
       cluster, flash_addr); */
    PY25Q16_WriteBuffer(flash_addr, data + written, to_write, true);

    written += to_write;
    offset_in_cluster += to_write;

    // Если кластер заполнен, переходим к следующему
    if (offset_in_cluster >= CLUSTER_SIZE) {
      offset_in_cluster = 0;
      uint16_t next_cluster = read_fat_entry(cluster);

      if (next_cluster >= 0xFFF8 && written < size) {
        LogC(LOG_C_RED, "[FAT] FAT chain ended prematurely");
        return -1;
      }

      cluster = next_cluster;
    }
  }

  // ФИНАЛЬНЫЙ СБРОС КЭША
  flush_cache();
  cached_sector = 0xFFFFFFFF;

  // 5. Обновляем запись в директории
  memset(&entry, 0, sizeof(entry));
  memcpy(entry.name, formatted_name, 11);
  entry.attr = 0x20;
  entry.first_clusterLO = first_cluster;
  entry.file_size = new_size;
  entry.create_date = _VOLUME_CREATE_DATE;
  entry.create_time = _VOLUME_CREATE_TIME;
  entry.write_date = _VOLUME_CREATE_DATE;
  entry.write_time = _VOLUME_CREATE_TIME;

  int result = update_file_entry(formatted_name, &entry);
  // printf("  Directory update result: %d\n", result);

  return result;
}

void usb_fs_close(usb_fs_handle_t *handle) {
  // Просто обнуляем структуру
  handle->first_cluster = 0;
  handle->file_size = 0;
  handle->position = 0;
  handle->current_cluster = 0;
  handle->current_position_in_cluster = 0;
}

int usb_fs_open(const char *name, usb_fs_handle_t *handle) {
  fat_dir_entry_t entry;
  char formatted_name[12];

  // Форматируем имя
  fat_format_name(name, formatted_name);

  // printf("usb_fs_open: name='%s', formatted='%.11s'\n", name,
  // formatted_name);

  if (find_file_entry(formatted_name, &entry) == -1) {
    // printf("  File not found!\n");
    LogC(LOG_C_YELLOW, "[FAT] File '%s' not found", name);
    return -1;
  }

  /* printf("  File found: cluster=%u, size=%lu\n", entry.first_clusterLO,
         entry.file_size); */

  // ОБЯЗАТЕЛЬНО обнуляем handle перед использованием
  memset(handle, 0, sizeof(usb_fs_handle_t));

  handle->first_cluster = entry.first_clusterLO;
  handle->file_size = entry.file_size;
  handle->position = 0;
  handle->current_cluster = entry.first_clusterLO;
  handle->current_position_in_cluster = 0;

  return 0;
}

size_t usb_fs_read_bytes(usb_fs_handle_t *handle, uint8_t *buf, size_t len) {
  size_t read = 0;

  // ПРОВЕРКА: handle должен быть валидным
  if (!handle || handle->first_cluster < 2) {
    return 0;
  }

  // ПРОВЕРКА: если файл пустой
  if (handle->file_size == 0) {
    return 0;
  }

  while (len > 0 && handle->position < handle->file_size) {
    // ПРОВЕРКА: кластер должен быть валидным
    if (handle->current_cluster < 2 || handle->current_cluster >= 0xFFF8) {
      LogC(LOG_C_RED, "[FAT] Invalid cluster %u", handle->current_cluster);
      break;
    }

    uint32_t remaining_in_cluster =
        CLUSTER_SIZE - handle->current_position_in_cluster;

    if (remaining_in_cluster == 0) {
      // Переходим к следующему кластеру
      uint16_t next_cluster = read_fat_entry(handle->current_cluster);

      // ПРОВЕРКА: следующий кластер должен быть валидным
      if (next_cluster < 2 || next_cluster >= 0xFFF8) {
        if (next_cluster < 0xFFF8) {
          LogC(LOG_C_RED, "[FAT] Corrupted FAT chain at cluster %u -> %u\n",
               handle->current_cluster, next_cluster);
        }
        break;
      }

      handle->current_cluster = next_cluster;
      handle->current_position_in_cluster = 0;
      continue;
    }

    uint32_t to_read = remaining_in_cluster;
    if (to_read > len)
      to_read = len;
    if (to_read > handle->file_size - handle->position) {
      to_read = handle->file_size - handle->position;
    }

    uint32_t flash_addr = FLASH_DATA_OFFSET +
                          (handle->current_cluster - 2) * CLUSTER_SIZE +
                          handle->current_position_in_cluster;

    // Читаем данные
    PY25Q16_ReadBuffer(flash_addr, buf + read, to_read);

    read += to_read;
    len -= to_read;
    handle->position += to_read;
    handle->current_position_in_cluster += to_read;
  }

  return read;
}

int usb_fs_read_file(const char *name, uint8_t *data, uint32_t *size) {
  usb_fs_handle_t handle;

  // Передаем исходное имя, пусть usb_fs_open сама его форматирует
  if (usb_fs_open(name, &handle) != 0) {
    return -1;
  }

  uint32_t max_read = *size;
  *size = 0;

  // СБРАСЫВАЕМ КЭШ перед чтением большого файла
  if (handle.file_size > SECTOR_SIZE) {
    flush_cache();
    cached_sector = 0xFFFFFFFF;
  }

  uint8_t *ptr = data;
  while (*size < handle.file_size && *size < max_read) {
    size_t chunk = usb_fs_read_bytes(&handle, ptr, max_read - *size);
    if (chunk == 0)
      break;
    ptr += chunk;
    *size += chunk;
  }

  if (*size < handle.file_size) {
    return -1; // Buffer too small
  }

  return 0;
}

int usb_fs_delete_file(const char *name) {
  char formatted_name[12];
  fat_format_name(name, formatted_name);

  fat_dir_entry_t entry;
  int idx = find_file_entry(formatted_name, &entry);

  if (idx == -1) {
    return -1;
  }

  free_clusters(entry.first_clusterLO);

  uint32_t sector = ROOT_START_SECTOR + idx / (SECTOR_SIZE / 32);
  uint32_t offset = (idx % (SECTOR_SIZE / 32)) * 32;

  read_sector_to_cache(sector);
  sector_cache[offset] = 0xE5;
  cache_dirty = true;
  flush_cache();

  return 0;
}

// Получить список файлов
int usb_fs_list_files(file_info_t *list, int max_count) {
  int count = 0;

  for (uint32_t sector = ROOT_START_SECTOR;
       sector < DATA_START_SECTOR && count < max_count; sector++) {
    read_sector_to_cache(sector);

    for (int i = 0; i < (SECTOR_SIZE / 32) && count < max_count; i++) {
      fat_dir_entry_t *e = (fat_dir_entry_t *)(sector_cache + i * 32);

      if (e->name[0] == 0) {
        return count;
      }

      if (e->name[0] != 0xE5 && !(e->attr & 0x08)) {
        // memcpy(list[count].name, e->name, 11);
        // list[count].name[11] = '\0';
        fat_unformat_name(e->name, list[count].name);
        list[count].size = e->file_size;
        list[count].create_date = e->create_date;
        list[count].create_time = e->create_time;
        list[count].write_date = e->write_date;
        list[count].write_time = e->write_time;
        count++;
      }
    }
  }

  return count;
}

int usb_fs_get_file_count(void) {
  int count = 0;

  for (uint32_t sector = ROOT_START_SECTOR; sector < DATA_START_SECTOR;
       sector++) {
    read_sector_to_cache(sector);

    for (int i = 0; i < (SECTOR_SIZE / 32); i++) {
      fat_dir_entry_t *e = (fat_dir_entry_t *)(sector_cache + i * 32);

      if (e->name[0] == 0)
        return count;
      if (e->name[0] != 0xE5 && !(e->attr & 0x08))
        count++;
    }
  }

  return count;
}

bool usb_fs_file_exists(const char *name) {
  char formatted_name[12];
  fat_format_name(name, formatted_name);
  return find_file_entry(formatted_name, NULL) != -1;
}

uint32_t usb_fs_get_free_space(void) {
  uint32_t free_clusters = 0;

  for (uint16_t i = 2; i < (DATA_SECTORS / SECTORS_PER_CLUSTER); i++) {
    if (read_fat_entry(i) == 0) {
      free_clusters++;
    }
  }

  return free_clusters * SECTORS_PER_CLUSTER * SECTOR_SIZE;
}

uint32_t usb_fs_get_total_space(void) { return DATA_SECTORS * SECTOR_SIZE; }

// Публичная функция форматирования
void usb_fs_format(void) {
  printf("FAT FS: Full format requested\n");
  usb_fs_format_raw();
}

// USB callbacks

void usb_fs_reset_cache(void) {
  flush_cache(); // Сохраняем изменения если есть
  cached_sector = 0xFFFFFFFF;
  cache_dirty = false;
}

void usb_fs_configure_done(void) {
  Log("[USB] MSC Configure done");
  // КРИТИЧНО: Сбрасываем кэш FAT при подключении USB
  // Это заставит FAT перечитать все структуры с флешки
  flush_cache(); // Если функция доступна, иначе нужно добавить
  cached_sector = 0xFFFFFFFF; // Инвалидируем кэш
  cache_dirty = false;
}

void usb_fs_get_cap(uint32_t *sector_num, uint16_t *sector_size) {
  *sector_num = TOTAL_SECTORS;
  *sector_size = SECTOR_SIZE;
}

int usb_fs_sector_read(uint32_t sector, uint8_t *buf, uint32_t size) {
  if (size != SECTOR_SIZE) {
    return 1;
  }

  // Если этот сектор в кэше и он грязный, сбрасываем его
  if (cached_sector == sector && cache_dirty) {
    flush_cache();
    cached_sector = 0xFFFFFFFF;
  }

  if (sector == 0) {
    // Boot sector - генерируем на лету
    memcpy(buf, &BOOT_SECTOR_RECORD, sizeof(BOOT_SECTOR_RECORD));
    buf[510] = 0x55;
    buf[511] = 0xAA;
    memset(buf + sizeof(BOOT_SECTOR_RECORD), 0,
           SECTOR_SIZE - sizeof(BOOT_SECTOR_RECORD));
  } else {
    // Чтение данных - БЕЗ использования кэша
    uint32_t flash_addr = sector * SECTOR_SIZE;
    PY25Q16_ReadBuffer(flash_addr, buf, SECTOR_SIZE);
  }

  return 0;
}

int usb_fs_sector_write(uint32_t sector, uint8_t *buf, uint32_t size) {
  if (size != SECTOR_SIZE) {
    return 1;
  }

  flush_cache(); // Сброс кэша перед записью
  cached_sector = 0xFFFFFFFF;
  cache_dirty = false;

  if (sector == 0) {
    // Специальная обработка boot sector (как есть, но используем smart)
    uint8_t protected_boot[SECTOR_SIZE];
    PY25Q16_ReadBuffer(0, protected_boot, SECTOR_SIZE);
    // Разрешаем только Volume ID (0x24-0x27) и Label (0x2B-0x35)
    for (int i = 0; i < SECTOR_SIZE; i++) {
      if (!((i >= 0x24 && i <= 0x27) || (i >= 0x2B && i <= 0x35))) {
        buf[i] = protected_boot[i];
      }
    }
    buf[510] = 0x55;
    buf[511] = 0xAA;
    // Теперь smart запись
    smart_write_sector(sector, buf);
  } else {
    // Для всех остальных — smart запись
    smart_write_sector(sector, buf);
  }

  // Инвалидация кэша для FAT/root
  if ((sector >= FAT_START_SECTOR &&
       sector < FAT2_START_SECTOR + SECTORS_PER_FAT) ||
      (sector >= ROOT_START_SECTOR && sector < DATA_START_SECTOR)) {
    cached_sector = 0xFFFFFFFF;
    cache_dirty = false;
  }

  return 0;
}

void debug_file_structure(const char *name) {
  char formatted_name[12];
  fat_format_name(name, formatted_name);

  fat_dir_entry_t entry;
  if (find_file_entry(formatted_name, &entry) == -1) {
    printf("File '%s' not found\n", name);
    return;
  }

  printf("File '%s' structure:\n", name);
  printf("  Size: %lu bytes\n", entry.file_size);
  printf("  First cluster: %u\n", entry.first_clusterLO);

  if (entry.first_clusterLO >= 2) {
    printf("  FAT chain: ");
    uint16_t cluster = entry.first_clusterLO;
    int count = 0;

    while (cluster >= 2 && cluster < 0xFFF8 && count < 20) {
      printf("%u", cluster);
      cluster = read_fat_entry(cluster);
      if (cluster >= 2 && cluster < 0xFFF8) {
        printf(" -> ");
      }
      count++;
    }

    if (cluster >= 0xFFF8) {
      printf(" -> EOF (0x%04X)", cluster);
    }
    printf("\n");

    // Проверяем данные
    printf("  Data verification: ");
    uint8_t buffer[16];
    uint32_t flash_addr =
        FLASH_DATA_OFFSET + (entry.first_clusterLO - 2) * CLUSTER_SIZE;
    PY25Q16_ReadBuffer(flash_addr, buffer, 16);
    printf("First 16 bytes at 0x%06lx: ", flash_addr);
    for (int i = 0; i < 16; i++) {
      printf("%02X ", buffer[i]);
    }
    printf(" (");
    for (int i = 0; i < 16; i++) {
      if (buffer[i] >= 32 && buffer[i] < 127) {
        printf("%c", buffer[i]);
      } else {
        printf(".");
      }
    }
    printf(")\n");
  }
}

void debug_fat_table(uint16_t start_cluster, uint16_t count) {
  printf("FAT table dump (clusters %u-%u):\n", start_cluster,
         start_cluster + count - 1);

  for (uint16_t i = start_cluster; i < start_cluster + count; i++) {
    uint16_t entry = read_fat_entry(i);
    if (entry != 0) {
      printf("  FAT[%3u] = 0x%04X", i, entry);
      if (entry == 0xFFFF)
        printf(" (EOF)");
      else if (entry == 0xFFF8)
        printf(" (BAD)");
      printf("\n");
    }
  }
}

void check_fat_consistency(void) {
  printf("=== FAT Consistency Check ===\n");

  // Проверяем, что FAT1 и FAT2 идентичны
  uint8_t fat1_sector[SECTOR_SIZE];
  uint8_t fat2_sector[SECTOR_SIZE];
  bool fat_ok = true;

  for (int i = 0; i < SECTORS_PER_FAT; i++) {
    uint32_t fat1_addr = FLASH_FAT_OFFSET + i * SECTOR_SIZE;
    uint32_t fat2_addr =
        FLASH_FAT_OFFSET + SECTORS_PER_FAT * SECTOR_SIZE + i * SECTOR_SIZE;

    PY25Q16_ReadBuffer(fat1_addr, fat1_sector, SECTOR_SIZE);
    PY25Q16_ReadBuffer(fat2_addr, fat2_sector, SECTOR_SIZE);

    if (memcmp(fat1_sector, fat2_sector, SECTOR_SIZE) != 0) {
      printf("FAT mismatch in sector %d\n", i);
      fat_ok = false;
    }
  }
  printf("FAT copies identical: %s\n", fat_ok ? "YES" : "NO");

  // Проверяем кэш
  printf("Cache state: sector=%lu, dirty=%d\n", cached_sector, cache_dirty);

  // Проверяем наличие файла настроек
  if (usb_fs_file_exists("settings.ini")) {
    printf("settings.ini exists\n");

    // Читаем и показываем размер
    uint8_t buffer[100];
    uint32_t size = sizeof(buffer);
    if (usb_fs_read_file("settings.ini", buffer, &size) == 0) {
      printf("File size: %lu bytes\n", size);
      printf("First 32 bytes: ");
      for (int i = 0; i < 32 && i < size; i++) {
        printf("%02X ", buffer[i]);
      }
      printf("\n");
    }
  } else {
    printf("settings.ini does NOT exist\n");
  }

  printf("=== End Check ===\n");
}
