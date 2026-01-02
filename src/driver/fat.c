#include "fat.h"
#include "../board.h"
#include "py25q16.h"
#include <assert.h>
#include <stdbool.h>
#include <string.h>

#define _VOLUME_CREATE_DATE FAT_MK_DATE(2025, 1, 2)
#define _VOLUME_CREATE_TIME FAT_MK_TIME(12, 0, 0)

#define VOLUME_ID ((_VOLUME_CREATE_DATE) << 16) | (_VOLUME_CREATE_TIME)
#define VOLUME_LABEL "STORAGE    "
#define BPB_MEDIA 0xf8

// ВАЖНО: Размер стирания SPI флешки = 4096 байт (команда 0x20)
#define FLASH_ERASE_SIZE 4096

// Параметры файловой системы
#define FLASH_SIZE (2 * 1024 * 1024)             // 2MB
#define TOTAL_SECTORS (FLASH_SIZE / SECTOR_SIZE) // 4096 секторов
#define SECTORS_PER_CLUSTER 8                    // 4KB кластеры = размер стирания!
#define RESERVED_SECTORS 1
#define FAT_COPIES 2
#define ROOT_ENTRIES 512
#define SECTORS_PER_FAT 16

// Вычисляемые значения
#define FAT_START_SECTOR RESERVED_SECTORS
#define FAT2_START_SECTOR (FAT_START_SECTOR + SECTORS_PER_FAT)
#define ROOT_START_SECTOR (FAT2_START_SECTOR + SECTORS_PER_FAT)
#define ROOT_SECTORS ((ROOT_ENTRIES * 32) / SECTOR_SIZE)
#define DATA_START_SECTOR (ROOT_START_SECTOR + ROOT_SECTORS)
#define DATA_SECTORS (TOTAL_SECTORS - DATA_START_SECTOR)

// Смещения в SPI флешке (выравнены по 4KB)
#define FLASH_FAT_OFFSET 0
#define FLASH_ROOT_OFFSET (SECTORS_PER_FAT * SECTOR_SIZE * 2)
#define FLASH_DATA_OFFSET (FLASH_ROOT_OFFSET + ROOT_SECTORS * SECTOR_SIZE)

// Проверка выравнивания
static_assert((FLASH_FAT_OFFSET % FLASH_ERASE_SIZE) == 0, "FAT not aligned");
static_assert((FLASH_ROOT_OFFSET % FLASH_ERASE_SIZE) == 0, "Root not aligned");
static_assert((FLASH_DATA_OFFSET % FLASH_ERASE_SIZE) == 0, "Data not aligned");
static_assert((SECTORS_PER_CLUSTER * SECTOR_SIZE) == FLASH_ERASE_SIZE, "Cluster size must match erase size");

// Кэш для одного сектора (экономия RAM)
static uint8_t sector_cache[SECTOR_SIZE];
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

// Сброс кэша на флешку
static void flush_cache(void) {
  if (cache_dirty && cached_sector != 0xFFFFFFFF) {
    uint32_t flash_addr = 0;

    if (cached_sector >= FAT_START_SECTOR && cached_sector < ROOT_START_SECTOR) {
      flash_addr = FLASH_FAT_OFFSET + (cached_sector - FAT_START_SECTOR) * SECTOR_SIZE;
    } else if (cached_sector >= ROOT_START_SECTOR && cached_sector < DATA_START_SECTOR) {
      flash_addr = FLASH_ROOT_OFFSET + (cached_sector - ROOT_START_SECTOR) * SECTOR_SIZE;
    } else if (cached_sector >= DATA_START_SECTOR) {
      flash_addr = FLASH_DATA_OFFSET + (cached_sector - DATA_START_SECTOR) * SECTOR_SIZE;
    }

    // ВАЖНО: Append=false чтобы использовать кэш в py25q16.c
    PY25Q16_WriteBuffer(flash_addr, sector_cache, SECTOR_SIZE, false);
    cache_dirty = false;
  }
}

// Чтение сектора из флешки в кэш
static void read_sector_to_cache(uint32_t sector) {
  if (cached_sector == sector) {
    return; // Уже в кэше
  }

  flush_cache(); // Сохранить старый кэш

  uint32_t flash_addr = 0;

  if (sector >= FAT_START_SECTOR && sector < ROOT_START_SECTOR) {
    flash_addr = FLASH_FAT_OFFSET + (sector - FAT_START_SECTOR) * SECTOR_SIZE;
  } else if (sector >= ROOT_START_SECTOR && sector < DATA_START_SECTOR) {
    flash_addr = FLASH_ROOT_OFFSET + (sector - ROOT_START_SECTOR) * SECTOR_SIZE;
  } else if (sector >= DATA_START_SECTOR) {
    flash_addr = FLASH_DATA_OFFSET + (sector - DATA_START_SECTOR) * SECTOR_SIZE;
  }

  PY25Q16_ReadBuffer(flash_addr, sector_cache, SECTOR_SIZE);
  cached_sector = sector;
  cache_dirty = false;
}

// Инициализация файловой системы
void usb_fs_init(void) {
  cached_sector = 0xFFFFFFFF;
  cache_dirty = false;

  // Проверяем, инициализирована ли уже флешка
  uint8_t check[4];
  PY25Q16_ReadBuffer(FLASH_FAT_OFFSET, check, 4);
  
  // Если уже инициализирована (первые байты FAT правильные), не трогаем
  if (check[0] == BPB_MEDIA && check[1] == 0xff && 
      check[2] == 0xff && check[3] == 0xff) {
    return; // Уже инициализирована
  }

  // Полная инициализация
  memset(sector_cache, 0, SECTOR_SIZE);
  
  // FAT1: Инициализация первых entry
  sector_cache[0] = BPB_MEDIA;
  sector_cache[1] = 0xff;
  sector_cache[2] = 0xff;
  sector_cache[3] = 0xff;

  PY25Q16_SectorErase(FLASH_FAT_OFFSET);
  PY25Q16_WriteBuffer(FLASH_FAT_OFFSET, sector_cache, SECTOR_SIZE, true);

  // FAT2: копия FAT1
  PY25Q16_SectorErase(FLASH_FAT_OFFSET + SECTORS_PER_FAT * SECTOR_SIZE);
  PY25Q16_WriteBuffer(FLASH_FAT_OFFSET + SECTORS_PER_FAT * SECTOR_SIZE, 
                      sector_cache, SECTOR_SIZE, true);

  // Root Directory: создаём пустой (все нули = нет файлов)
  memset(sector_cache, 0, SECTOR_SIZE);
  PY25Q16_SectorErase(FLASH_ROOT_OFFSET);
  for (int i = 0; i < ROOT_SECTORS; i++) {
    PY25Q16_WriteBuffer(FLASH_ROOT_OFFSET + i * SECTOR_SIZE, 
                       sector_cache, SECTOR_SIZE, true);
  }
}

// Прочитать FAT entry
static uint16_t read_fat_entry(uint16_t cluster) {
  uint32_t sector = FAT_START_SECTOR + (cluster * 2) / SECTOR_SIZE;
  uint32_t offset = (cluster * 2) % SECTOR_SIZE;

  read_sector_to_cache(sector);
  return (sector_cache[offset + 1] << 8) | sector_cache[offset];
}

// Записать FAT entry
static void write_fat_entry(uint16_t cluster, uint16_t value) {
  uint32_t sector = FAT_START_SECTOR + (cluster * 2) / SECTOR_SIZE;
  uint32_t offset = (cluster * 2) % SECTOR_SIZE;

  read_sector_to_cache(sector);
  sector_cache[offset] = value & 0xFF;
  sector_cache[offset + 1] = value >> 8;
  cache_dirty = true;
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
      return i;
    }
  }
  return 0;
}

// Выделить кластеры для файла
static uint16_t allocate_clusters(uint32_t size) {
  uint32_t clusters_needed = (size + (SECTORS_PER_CLUSTER * SECTOR_SIZE) - 1) /
                             (SECTORS_PER_CLUSTER * SECTOR_SIZE);

  if (clusters_needed == 0)
    clusters_needed = 1;

  uint16_t first_cluster = 0;
  uint16_t prev_cluster = 0;

  for (uint32_t i = 0; i < clusters_needed; i++) {
    uint16_t cluster = find_free_cluster();
    if (cluster == 0) {
      return 0; // Нет места
    }

    if (first_cluster == 0) {
      first_cluster = cluster;
    }

    if (prev_cluster != 0) {
      write_fat_entry(prev_cluster, cluster);
    }

    prev_cluster = cluster;
  }

  if (prev_cluster != 0) {
    write_fat_entry(prev_cluster, 0xFFFF);
  }

  return first_cluster;
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

// Найти файл в root directory
static int find_file_entry(const char *name, fat_dir_entry_t *entry) {
  for (uint32_t sector = ROOT_START_SECTOR; sector < DATA_START_SECTOR; sector++) {
    read_sector_to_cache(sector);

    for (int i = 0; i < (SECTOR_SIZE / 32); i++) {
      fat_dir_entry_t *e = (fat_dir_entry_t *)(sector_cache + i * 32);

      if (e->name[0] == 0) {
        return -1; // Конец директории
      }

      if (e->name[0] != 0xE5 && !(e->attr & 0x08)) {
        if (memcmp(e->name, name, 11) == 0) {
          if (entry) {
            memcpy(entry, e, sizeof(fat_dir_entry_t));
          }
          return (sector - ROOT_START_SECTOR) * (SECTOR_SIZE / 32) + i;
        }
      }
    }
  }

  return -1;
}

// Создать или обновить запись в root directory
static int update_file_entry(const char *name, const fat_dir_entry_t *entry) {
  int idx = find_file_entry(name, NULL);

  if (idx == -1) {
    // Найти свободную запись
    for (uint32_t sector = ROOT_START_SECTOR; sector < DATA_START_SECTOR; sector++) {
      read_sector_to_cache(sector);

      for (int i = 0; i < (SECTOR_SIZE / 32); i++) {
        fat_dir_entry_t *e = (fat_dir_entry_t *)(sector_cache + i * 32);

        if (e->name[0] == 0 || e->name[0] == 0xE5) {
          memcpy(e, entry, sizeof(fat_dir_entry_t));
          cache_dirty = true;
          flush_cache();
          return 0;
        }
      }
    }
    return -1; // Нет места в root
  }

  // Обновить существующую запись
  uint32_t sector = ROOT_START_SECTOR + idx / (SECTOR_SIZE / 32);
  uint32_t offset = (idx % (SECTOR_SIZE / 32)) * 32;

  read_sector_to_cache(sector);
  memcpy(sector_cache + offset, entry, sizeof(fat_dir_entry_t));
  cache_dirty = true;
  flush_cache();

  return 0;
}

// Записать файл
int usb_fs_write_file(const char *name, const uint8_t *data, uint32_t size) {
  fat_dir_entry_t entry;
  int idx = find_file_entry(name, &entry);

  if (idx != -1) {
    // Удалить старые кластеры
    if (entry.first_clusterLO >= 2) {
      free_clusters(entry.first_clusterLO);
    }
  }

  // Выделить новые кластеры
  uint16_t first_cluster = 0;
  if (size > 0) {
    first_cluster = allocate_clusters(size);
    if (first_cluster == 0) {
      return -1; // Нет места
    }

    // Записать данные по кластерам (каждый кластер = 4KB = размер стирания)
    uint16_t cluster = first_cluster;
    uint32_t written = 0;

    while (written < size && cluster >= 2 && cluster < 0xFFF8) {
      uint32_t flash_addr = FLASH_DATA_OFFSET + 
                           (cluster - 2) * SECTORS_PER_CLUSTER * SECTOR_SIZE;

      uint32_t to_write = (size - written) > (SECTORS_PER_CLUSTER * SECTOR_SIZE)
                              ? (SECTORS_PER_CLUSTER * SECTOR_SIZE)
                              : (size - written);

      // Стираем весь кластер (4KB)
      PY25Q16_SectorErase(flash_addr);
      // Записываем данные
      PY25Q16_WriteBuffer(flash_addr, data + written, to_write, true);

      written += to_write;
      cluster = read_fat_entry(cluster);
    }
  }

  // Обновить directory entry
  memset(&entry, 0, sizeof(entry));
  memcpy(entry.name, name, 11);
  entry.attr = 0x20; // Archive
  entry.first_clusterLO = first_cluster;
  entry.file_size = size;
  entry.create_date = _VOLUME_CREATE_DATE;
  entry.create_time = _VOLUME_CREATE_TIME;
  entry.write_date = _VOLUME_CREATE_DATE;
  entry.write_time = _VOLUME_CREATE_TIME;

  return update_file_entry(name, &entry);
}

// Прочитать файл
int usb_fs_read_file(const char *name, uint8_t *data, uint32_t *size) {
  fat_dir_entry_t entry;

  if (find_file_entry(name, &entry) == -1) {
    return -1;
  }

  if (*size < entry.file_size) {
    return -1;
  }

  uint16_t cluster = entry.first_clusterLO;
  uint32_t read_bytes = 0;

  while (read_bytes < entry.file_size && cluster >= 2 && cluster < 0xFFF8) {
    uint32_t flash_addr = FLASH_DATA_OFFSET + 
                         (cluster - 2) * SECTORS_PER_CLUSTER * SECTOR_SIZE;

    uint32_t to_read = (entry.file_size - read_bytes) > (SECTORS_PER_CLUSTER * SECTOR_SIZE)
                          ? (SECTORS_PER_CLUSTER * SECTOR_SIZE)
                          : (entry.file_size - read_bytes);

    PY25Q16_ReadBuffer(flash_addr, data + read_bytes, to_read);

    read_bytes += to_read;
    cluster = read_fat_entry(cluster);
  }

  *size = entry.file_size;
  return 0;
}

// Удалить файл
int usb_fs_delete_file(const char *name) {
  fat_dir_entry_t entry;
  int idx = find_file_entry(name, &entry);

  if (idx == -1) {
    return -1;
  }

  free_clusters(entry.first_clusterLO);

  // Пометить как удаленный
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
        memcpy(list[count].name, e->name, 11);
        list[count].name[11] = '\0';
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

  for (uint32_t sector = ROOT_START_SECTOR; sector < DATA_START_SECTOR; sector++) {
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
  return find_file_entry(name, NULL) != -1;
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

uint32_t usb_fs_get_total_space(void) { 
  return DATA_SECTORS * SECTOR_SIZE; 
}

// Форматирование файловой системы
void usb_fs_format(void) {
  cached_sector = 0xFFFFFFFF;
  cache_dirty = false;

  memset(sector_cache, 0, SECTOR_SIZE);
  
  // FAT1: Инициализация первых entry
  sector_cache[0] = BPB_MEDIA;
  sector_cache[1] = 0xff;
  sector_cache[2] = 0xff;
  sector_cache[3] = 0xff;

  PY25Q16_SectorErase(FLASH_FAT_OFFSET);
  PY25Q16_WriteBuffer(FLASH_FAT_OFFSET, sector_cache, SECTOR_SIZE, true);

  // FAT2: копия FAT1
  PY25Q16_SectorErase(FLASH_FAT_OFFSET + SECTORS_PER_FAT * SECTOR_SIZE);
  PY25Q16_WriteBuffer(FLASH_FAT_OFFSET + SECTORS_PER_FAT * SECTOR_SIZE, 
                      sector_cache, SECTOR_SIZE, true);

  // Root Directory: очистка
  memset(sector_cache, 0, SECTOR_SIZE);
  PY25Q16_SectorErase(FLASH_ROOT_OFFSET);
  for (int i = 0; i < ROOT_SECTORS; i++) {
    PY25Q16_WriteBuffer(FLASH_ROOT_OFFSET + i * SECTOR_SIZE, 
                       sector_cache, SECTOR_SIZE, true);
  }
  
  // Очистка первых нескольких блоков данных (опционально)
  for (int i = 0; i < 4; i++) {
    PY25Q16_SectorErase(FLASH_DATA_OFFSET + i * FLASH_ERASE_SIZE);
  }
}

// USB callbacks
void usb_fs_configure_done(void) { 
  BOARD_ToggleRed(true); 
}

void usb_fs_get_cap(uint32_t *sector_num, uint16_t *sector_size) {
  *sector_num = TOTAL_SECTORS;
  *sector_size = SECTOR_SIZE;
}

int usb_fs_sector_read(uint32_t sector, uint8_t *buf, uint32_t size) {
  if (size != SECTOR_SIZE || ((uint32_t)buf) % 4 != 0) {
    return 1;
  }

  memset(buf, 0, SECTOR_SIZE);

  if (sector == 0) {
    // Boot sector
    memcpy(buf, &BOOT_SECTOR_RECORD, sizeof(BOOT_SECTOR_RECORD));
    buf[510] = 0x55;
    buf[511] = 0xAA;
  } else if (sector >= FAT_START_SECTOR && sector < DATA_START_SECTOR) {
    // FAT или Root - читаем из флешки
    read_sector_to_cache(sector);
    memcpy(buf, sector_cache, SECTOR_SIZE);
  } else if (sector >= DATA_START_SECTOR && sector < TOTAL_SECTORS) {
    // Data область
    uint32_t flash_addr = FLASH_DATA_OFFSET + 
                         (sector - DATA_START_SECTOR) * SECTOR_SIZE;
    PY25Q16_ReadBuffer(flash_addr, buf, SECTOR_SIZE);
  }

  return 0;
}

int usb_fs_sector_write(uint32_t sector, const uint8_t *buf, uint32_t size) {
  if (size != SECTOR_SIZE || ((uint32_t)buf) % 4 != 0) {
    return 1;
  }

  if (sector >= FAT_START_SECTOR && sector < DATA_START_SECTOR) {
    // FAT или Root - используем кэш
    memcpy(sector_cache, buf, SECTOR_SIZE);
    cached_sector = sector;
    cache_dirty = true;
    flush_cache();
  } else if (sector >= DATA_START_SECTOR && sector < TOTAL_SECTORS) {
    // Data область - пишем напрямую (py25q16.c сам обработает)
    uint32_t flash_addr = FLASH_DATA_OFFSET + 
                         (sector - DATA_START_SECTOR) * SECTOR_SIZE;
    PY25Q16_WriteBuffer(flash_addr, buf, SECTOR_SIZE, false);
  }

  return 0;
}
