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

#define FLASH_ERASE_SIZE 4096

// Параметры файловой системы
#define FLASH_SIZE (2 * 1024 * 1024)
#define TOTAL_SECTORS (FLASH_SIZE / SECTOR_SIZE)
#define SECTORS_PER_CLUSTER 8
#define RESERVED_SECTORS 8
#define FAT_COPIES 2
#define ROOT_ENTRIES 512
#define SECTORS_PER_FAT 16
#define CLUSTER_SIZE (SECTORS_PER_CLUSTER * SECTOR_SIZE)

// Вычисляемые значения
#define FAT_START_SECTOR RESERVED_SECTORS
#define FAT2_START_SECTOR (FAT_START_SECTOR + SECTORS_PER_FAT)
#define ROOT_START_SECTOR (FAT2_START_SECTOR + SECTORS_PER_FAT)
#define ROOT_SECTORS ((ROOT_ENTRIES * 32) / SECTOR_SIZE)
#define DATA_START_SECTOR (ROOT_START_SECTOR + ROOT_SECTORS)
#define DATA_SECTORS (TOTAL_SECTORS - DATA_START_SECTOR)

// Смещения в SPI флешке
#define FLASH_FAT_OFFSET (FAT_START_SECTOR * SECTOR_SIZE)
#define FLASH_ROOT_OFFSET (ROOT_START_SECTOR * SECTOR_SIZE)
#define FLASH_DATA_OFFSET (DATA_START_SECTOR * SECTOR_SIZE)

// Проверка выравнивания
static_assert((FLASH_FAT_OFFSET % FLASH_ERASE_SIZE) == 0, "FAT not aligned");
static_assert((FLASH_ROOT_OFFSET % FLASH_ERASE_SIZE) == 0, "Root not aligned");
static_assert((FLASH_DATA_OFFSET % FLASH_ERASE_SIZE) == 0, "Data not aligned");
static_assert((SECTORS_PER_CLUSTER * SECTOR_SIZE) == FLASH_ERASE_SIZE,
              "Cluster size must match erase size");

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

// === ОСНОВНЫЕ ФУНКЦИИ ДЛЯ РАБОТЫ С FLASH ===

// Читать сектор из флешки
static void read_sector(uint32_t sector, uint8_t *buf) {
  uint32_t flash_addr = sector * SECTOR_SIZE;
  PY25Q16_ReadBuffer(flash_addr, buf, SECTOR_SIZE);
}

// Буфер для сохранения блока 4KB при стирании
static uint8_t erase_block_buffer[FLASH_ERASE_SIZE];

// Записать сектор во флешку (с умной проверкой)
static void write_sector(uint32_t sector, uint8_t *buf) {
  uint32_t flash_addr = sector * SECTOR_SIZE;
  uint8_t current[SECTOR_SIZE];

  // Читаем текущее содержимое
  PY25Q16_ReadBuffer(flash_addr, current, SECTOR_SIZE);

  // Проверяем, нужно ли стирание
  bool needs_erase = false;
  for (int i = 0; i < SECTOR_SIZE; i++) {
    if ((current[i] & buf[i]) != buf[i]) {
      needs_erase = true;
      break;
    }
  }

  if (needs_erase) {
    // КРИТИЧНО: Сохраняем весь блок 4KB перед стиранием
    uint32_t block_addr = flash_addr & ~(FLASH_ERASE_SIZE - 1);
    uint32_t offset_in_block = flash_addr - block_addr;

    // Сохраняем весь блок
    PY25Q16_ReadBuffer(block_addr, erase_block_buffer, FLASH_ERASE_SIZE);

    // Обновляем нужный сектор в буфере
    memcpy(erase_block_buffer + offset_in_block, buf, SECTOR_SIZE);

    // Стираем блок
    PY25Q16_SectorErase(block_addr);

    // Записываем весь блок обратно
    PY25Q16_WriteBuffer(block_addr, erase_block_buffer, FLASH_ERASE_SIZE,
                        false);
  } else {
    // Можем писать без стирания
    PY25Q16_WriteBuffer(flash_addr, buf, SECTOR_SIZE, false);
  }
}

// === РАБОТА С FAT ===

// Вспомогательные функции для формирования имен FAT
// Преобразует "test.txt" в "TEST    TXT"
static inline void fat_format_name(const char *filename, char *fat_name) {
  int i, j;

  // Инициализация пробелами
  for (i = 0; i < 11; i++) {
    fat_name[i] = ' ';
  }

  // Копирование имени (до точки или 8 символов)
  for (i = 0; i < 8 && filename[i] && filename[i] != '.'; i++) {
    fat_name[i] = (filename[i] >= 'a' && filename[i] <= 'z') ? filename[i] - 32
                                                             : filename[i];
  }

  // Поиск расширения
  while (filename[i] && filename[i] != '.')
    i++;
  if (filename[i] == '.') {
    i++;
    // Копирование расширения (до 3 символов)
    for (j = 0; j < 3 && filename[i + j]; j++) {
      fat_name[8 + j] = (filename[i + j] >= 'a' && filename[i + j] <= 'z')
                            ? filename[i + j] - 32
                            : filename[i + j];
    }
  }
}

// Преобразует "TEST    TXT" в "TEST.TXT"
static void fat_unformat_name(const char *fat_name, char *output) {
  int i;

  // Копируем имя (первые 8 символов, пропускаем пробелы)
  for (i = 0; i < 8; i++) {
    if (fat_name[i] != ' ') {
      *output++ = fat_name[i];
    }
  }

  // Копируем расширение (символы 8-10)
  if (fat_name[8] != ' ') {
    *output++ = '.';
    for (i = 8; i < 11; i++) {
      if (fat_name[i] != ' ') {
        *output++ = fat_name[i];
      }
    }
  }

  *output = '\0'; // Всегда завершаем нулевым символом
}

// Прочитать FAT entry
static uint16_t read_fat_entry(uint16_t cluster) {
  uint8_t buf[SECTOR_SIZE];
  uint32_t sector = FAT_START_SECTOR + (cluster * 2) / SECTOR_SIZE;
  uint32_t offset = (cluster * 2) % SECTOR_SIZE;

  uint32_t flash_addr = sector * SECTOR_SIZE;
  PY25Q16_ReadBuffer(flash_addr, buf, SECTOR_SIZE);
  return (buf[offset + 1] << 8) | buf[offset];
}

// Записать FAT entry
static void write_fat_entry(uint16_t cluster, uint16_t value) {
  uint8_t buf[SECTOR_SIZE];
  uint32_t sector = FAT_START_SECTOR + (cluster * 2) / SECTOR_SIZE;
  uint32_t offset = (cluster * 2) % SECTOR_SIZE;

  // Обновляем FAT1
  uint32_t flash_addr = sector * SECTOR_SIZE;
  PY25Q16_ReadBuffer(flash_addr, buf, SECTOR_SIZE);
  buf[offset] = value & 0xFF;
  buf[offset + 1] = value >> 8;
  write_sector(sector, buf);

  // Копируем в FAT2
  sector = FAT2_START_SECTOR + (cluster * 2) / SECTOR_SIZE;
  write_sector(sector, buf);
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

// Освободить кластеры
static void free_clusters(uint16_t first_cluster) {
  uint16_t cluster = first_cluster;

  while (cluster >= 2 && cluster < 0xFFF8) {
    uint16_t next = read_fat_entry(cluster);
    write_fat_entry(cluster, 0);
    cluster = next;
  }
}

// === РАБОТА С ДИРЕКТОРИЕЙ ===

static int find_file_entry(const char *formatted_name, fat_dir_entry_t *entry) {
  uint8_t buf[SECTOR_SIZE];

  for (uint32_t sector = ROOT_START_SECTOR; sector < DATA_START_SECTOR;
       sector++) {
    uint32_t flash_addr = sector * SECTOR_SIZE;
    PY25Q16_ReadBuffer(flash_addr, buf, SECTOR_SIZE);

    for (int i = 0; i < (SECTOR_SIZE / 32); i++) {
      fat_dir_entry_t *e = (fat_dir_entry_t *)(buf + i * 32);

      if (e->name[0] == 0)
        return -1; // Конец директории

      if (e->name[0] != 0xE5 && !(e->attr & 0x08)) {
        if (memcmp(e->name, formatted_name, 11) == 0) {
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

// Создать или обновить запись в директории
static int update_file_entry(const char *formatted_name,
                             const fat_dir_entry_t *entry) {
  uint8_t buf[SECTOR_SIZE];
  int idx = find_file_entry(formatted_name, NULL);

  if (idx == -1) {
    // Найти свободную запись
    for (uint32_t sector = ROOT_START_SECTOR; sector < DATA_START_SECTOR;
         sector++) {
      uint32_t flash_addr = sector * SECTOR_SIZE;
      PY25Q16_ReadBuffer(flash_addr, buf, SECTOR_SIZE);

      for (int i = 0; i < (SECTOR_SIZE / 32); i++) {
        fat_dir_entry_t *e = (fat_dir_entry_t *)(buf + i * 32);

        if (e->name[0] == 0 || e->name[0] == 0xE5) {
          memcpy(e, entry, sizeof(fat_dir_entry_t));
          write_sector(sector, buf);
          return 0;
        }
      }
    }
    return -1; // Нет места
  }

  // Обновить существующую запись
  uint32_t sector = ROOT_START_SECTOR + idx / (SECTOR_SIZE / 32);
  uint32_t offset = (idx % (SECTOR_SIZE / 32)) * 32;

  uint32_t flash_addr = sector * SECTOR_SIZE;
  PY25Q16_ReadBuffer(flash_addr, buf, SECTOR_SIZE);
  memcpy(buf + offset, entry, sizeof(fat_dir_entry_t));
  write_sector(sector, buf);

  return 0;
}

// === ФОРМАТИРОВАНИЕ ===

void usb_fs_format(void) {
  uint8_t buf[SECTOR_SIZE];

  Log("[FAT] Formatting...");

  // 1. Boot sector
  memset(buf, 0, SECTOR_SIZE);
  memcpy(buf, &BOOT_SECTOR_RECORD, sizeof(BOOT_SECTOR_RECORD));
  buf[510] = 0x55;
  buf[511] = 0xAA;

  PY25Q16_SectorErase(0);
  PY25Q16_WriteBuffer(0, buf, SECTOR_SIZE, false);

  // 2. FAT1
  memset(buf, 0, SECTOR_SIZE);
  buf[0] = BPB_MEDIA;
  buf[1] = 0xFF;
  buf[2] = 0xFF;
  buf[3] = 0xFF;

  PY25Q16_SectorErase(FLASH_FAT_OFFSET);
  PY25Q16_WriteBuffer(FLASH_FAT_OFFSET, buf, SECTOR_SIZE, false);

  // 3. FAT2 - копия FAT1
  PY25Q16_SectorErase(FLASH_FAT_OFFSET + SECTORS_PER_FAT * SECTOR_SIZE);
  PY25Q16_WriteBuffer(FLASH_FAT_OFFSET + SECTORS_PER_FAT * SECTOR_SIZE, buf,
                      SECTOR_SIZE, false);

  // 4. Root directory
  memset(buf, 0, SECTOR_SIZE);
  PY25Q16_SectorErase(FLASH_ROOT_OFFSET);
  PY25Q16_WriteBuffer(FLASH_ROOT_OFFSET, buf, SECTOR_SIZE, false);

  Log("[FAT] Format completed");
}

// === ИНИЦИАЛИЗАЦИЯ ===

void usb_fs_init(void) {
  Log("[FAT] Init");

  // Проверка boot signature
  uint8_t boot_check[512];
  PY25Q16_ReadBuffer(0, boot_check, 512);

  bool boot_valid = (boot_check[510] == 0x55 && boot_check[511] == 0xAA);

  if (!boot_valid) {
    LogC(LOG_C_BRIGHT_YELLOW, "[FAT] Invalid boot sector, formatting...");
    usb_fs_format();
  } else {
    Log("[FAT] Boot sector valid");
  }
}

// === ЗАПИСЬ ФАЙЛА ===

int usb_fs_write_file(const char *name, const uint8_t *data, uint32_t size,
                      bool append) {
  char formatted_name[12];
  fat_format_name(name, formatted_name);

  fat_dir_entry_t entry;
  int idx = find_file_entry(formatted_name, &entry);

  // Если файл существует и append=false, удаляем его
  if (idx != -1 && !append) {
    usb_fs_delete_file(name);
    idx = -1;
  }

  bool existed = (idx != -1);
  uint32_t old_size = existed ? entry.file_size : 0;
  uint16_t first_cluster = existed ? entry.first_clusterLO : 0;
  uint32_t new_size = old_size + size;

  // Если нет данных для записи
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

  // Выделяем кластеры
  uint32_t total_clusters_needed = (new_size + CLUSTER_SIZE - 1) / CLUSTER_SIZE;
  uint32_t existing_clusters = (old_size + CLUSTER_SIZE - 1) / CLUSTER_SIZE;
  uint32_t additional_clusters = total_clusters_needed - existing_clusters;

  if (additional_clusters > 0) {
    if (first_cluster == 0) {
      // Новый файл
      first_cluster = find_free_cluster();
      if (first_cluster == 0) {
        LogC(LOG_C_RED, "[FAT] No free cluster");
        return -1;
      }
      write_fat_entry(first_cluster, 0xFFFF);
      additional_clusters--;

      // Выделяем остальные кластеры
      uint16_t current = first_cluster;
      for (uint32_t i = 0; i < additional_clusters; i++) {
        uint16_t next = find_free_cluster();
        if (next == 0) {
          free_clusters(first_cluster);
          return -1;
        }
        write_fat_entry(current, next);
        write_fat_entry(next, 0xFFFF);
        current = next;
      }
    } else {
      // Существующий файл - находим последний кластер
      uint16_t current = first_cluster;
      while (true) {
        uint16_t next = read_fat_entry(current);
        if (next >= 0xFFF8)
          break;
        current = next;
      }

      // Добавляем новые кластеры
      for (uint32_t i = 0; i < additional_clusters; i++) {
        uint16_t next = find_free_cluster();
        if (next == 0)
          return -1;
        write_fat_entry(current, next);
        write_fat_entry(next, 0xFFFF);
        current = next;
      }
    }
  }

  // Записываем данные
  uint32_t written = 0;
  uint16_t cluster = first_cluster;
  uint32_t offset_in_cluster = 0;

  // Если append, находим позицию в последнем кластере
  if (append && old_size > 0) {
    cluster = first_cluster;
    uint32_t remaining = old_size;

    while (remaining > CLUSTER_SIZE) {
      cluster = read_fat_entry(cluster);
      remaining -= CLUSTER_SIZE;
    }
    offset_in_cluster = remaining;
  }

  while (written < size) {
    uint32_t flash_addr =
        FLASH_DATA_OFFSET + (cluster - 2) * CLUSTER_SIZE + offset_in_cluster;
    uint32_t space_in_cluster = CLUSTER_SIZE - offset_in_cluster;
    uint32_t to_write = size - written;

    if (to_write > space_in_cluster) {
      to_write = space_in_cluster;
    }

    // Стираем кластер если начинаем с начала
    if (offset_in_cluster == 0 && (!append || old_size == 0)) {
      uint8_t check_byte;
      PY25Q16_ReadBuffer(flash_addr, &check_byte, 1);
      if (check_byte != 0xFF) {
        PY25Q16_SectorErase(flash_addr);
      }
    }

    PY25Q16_WriteBuffer(flash_addr, data + written, to_write, true);

    written += to_write;
    offset_in_cluster += to_write;

    if (offset_in_cluster >= CLUSTER_SIZE) {
      offset_in_cluster = 0;
      cluster = read_fat_entry(cluster);
    }
  }

  // Обновляем запись в директории
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

// === ЧТЕНИЕ ФАЙЛА ===

void usb_fs_close(usb_fs_handle_t *handle) {
  memset(handle, 0, sizeof(usb_fs_handle_t));
}

int usb_fs_open(const char *name, usb_fs_handle_t *handle) {
  fat_dir_entry_t entry;
  char formatted_name[12];

  fat_format_name(name, formatted_name);

  if (find_file_entry(formatted_name, &entry) == -1) {
    return -1;
  }

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

  if (!handle || handle->first_cluster < 2 || handle->file_size == 0) {
    return 0;
  }

  while (len > 0 && handle->position < handle->file_size) {
    if (handle->current_cluster < 2 || handle->current_cluster >= 0xFFF8) {
      break;
    }

    uint32_t remaining_in_cluster =
        CLUSTER_SIZE - handle->current_position_in_cluster;

    if (remaining_in_cluster == 0) {
      uint16_t next_cluster = read_fat_entry(handle->current_cluster);
      if (next_cluster < 2 || next_cluster >= 0xFFF8)
        break;

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

  if (usb_fs_open(name, &handle) != 0) {
    return -1;
  }

  uint32_t max_read = *size;
  *size = 0;

  uint8_t *ptr = data;
  while (*size < handle.file_size && *size < max_read) {
    size_t chunk = usb_fs_read_bytes(&handle, ptr, max_read - *size);
    if (chunk == 0)
      break;
    ptr += chunk;
    *size += chunk;
  }

  printf("[READ_FILE] Read %lu / %lu bytes\n", *size, handle.file_size);

  if (*size < handle.file_size) {
    printf("[READ_FILE] Buffer too small!\n");
    return -1;
  }

  // Отладка: показываем первые 100 байт
  if (*size > 0) {
    printf("[READ_FILE] First 100 chars:\n");
    for (uint32_t i = 0; i < 100 && i < *size; i++) {
      if (data[i] >= 32 && data[i] < 127) {
        printf("%c", data[i]);
      } else if (data[i] == '\n') {
        printf("\\n\n");
      } else if (data[i] == '\r') {
        printf("\\r");
      } else {
        printf("[%02X]", data[i]);
      }
    }
    printf("\n");
  }

  return 0;
}

// === УДАЛЕНИЕ ФАЙЛА ===

int usb_fs_delete_file(const char *name) {
  uint8_t buf[SECTOR_SIZE];
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

  uint32_t flash_addr = sector * SECTOR_SIZE;
  PY25Q16_ReadBuffer(flash_addr, buf, SECTOR_SIZE);
  buf[offset] = 0xE5;
  write_sector(sector, buf);

  return 0;
}

// === ИНФОРМАЦИЯ О ФАЙЛАХ ===

int usb_fs_list_files(file_info_t *list, int max_count) {
  uint8_t buf[SECTOR_SIZE];
  int count = 0;

  for (uint32_t sector = ROOT_START_SECTOR;
       sector < DATA_START_SECTOR && count < max_count; sector++) {
    uint32_t flash_addr = sector * SECTOR_SIZE;
    PY25Q16_ReadBuffer(flash_addr, buf, SECTOR_SIZE);

    for (int i = 0; i < (SECTOR_SIZE / 32) && count < max_count; i++) {
      fat_dir_entry_t *e = (fat_dir_entry_t *)(buf + i * 32);

      if (e->name[0] == 0)
        return count;

      if (e->name[0] != 0xE5 && !(e->attr & 0x08)) {
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
  uint8_t buf[SECTOR_SIZE];
  int count = 0;

  for (uint32_t sector = ROOT_START_SECTOR; sector < DATA_START_SECTOR;
       sector++) {
    uint32_t flash_addr = sector * SECTOR_SIZE;
    PY25Q16_ReadBuffer(flash_addr, buf, SECTOR_SIZE);

    for (int i = 0; i < (SECTOR_SIZE / 32); i++) {
      fat_dir_entry_t *e = (fat_dir_entry_t *)(buf + i * 32);

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

// === USB CALLBACKS ===

void usb_fs_reset_cache(void) {
  // Нет кеша - ничего не делаем
}

void usb_fs_configure_done(void) { Log("[USB] MSC Configure done"); }

void usb_fs_get_cap(uint32_t *sector_num, uint16_t *sector_size) {
  *sector_num = TOTAL_SECTORS;
  *sector_size = SECTOR_SIZE;
}

int usb_fs_sector_read(uint32_t sector, uint8_t *buf, uint32_t size) {
  if (size != SECTOR_SIZE) {
    printf("[READ] ERROR: Invalid size %lu\n", size);
    return 1;
  }

  if (sector == 0) {
    // Boot sector - генерируем на лету
    memcpy(buf, &BOOT_SECTOR_RECORD, sizeof(BOOT_SECTOR_RECORD));
    buf[510] = 0x55;
    buf[511] = 0xAA;
    memset(buf + sizeof(BOOT_SECTOR_RECORD), 0,
           SECTOR_SIZE - sizeof(BOOT_SECTOR_RECORD));
  } else {
    // ВАЖНО: Читаем напрямую в буфер Windows, не используя sector_buffer
    uint32_t flash_addr = sector * SECTOR_SIZE;
    PY25Q16_ReadBuffer(flash_addr, buf, SECTOR_SIZE);
    // printf("[READ] Sector %lu, addr=0x%06lx, first byte=0x%02X\n",
    //        sector, flash_addr, buf[0]);
  }

  return 0;
}

int usb_fs_sector_write(uint32_t sector, uint8_t *buf, uint32_t size) {
  if (size != SECTOR_SIZE) {
    return 1;
  }

  if (sector == 0) {
    // Защита boot sector - разрешаем только Volume ID и Label
    uint8_t protected_boot[SECTOR_SIZE];
    uint32_t flash_addr = 0;
    PY25Q16_ReadBuffer(flash_addr, protected_boot, SECTOR_SIZE);

    for (int i = 0; i < SECTOR_SIZE; i++) {
      if (!((i >= 0x24 && i <= 0x27) || (i >= 0x2B && i <= 0x35))) {
        buf[i] = protected_boot[i];
      }
    }
    buf[510] = 0x55;
    buf[511] = 0xAA;
  }

  write_sector(sector, buf);

  return 0;
}

// === DEBUG ФУНКЦИИ ===

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
  uint8_t fat1_sect[SECTOR_SIZE];
  uint8_t fat2_sect[SECTOR_SIZE];
  bool fat_ok = true;

  for (int i = 0; i < SECTORS_PER_FAT; i++) {
    uint32_t fat1_addr = FLASH_FAT_OFFSET + i * SECTOR_SIZE;
    uint32_t fat2_addr =
        FLASH_FAT_OFFSET + SECTORS_PER_FAT * SECTOR_SIZE + i * SECTOR_SIZE;

    PY25Q16_ReadBuffer(fat1_addr, fat1_sect, SECTOR_SIZE);
    PY25Q16_ReadBuffer(fat2_addr, fat2_sect, SECTOR_SIZE);

    if (memcmp(fat1_sect, fat2_sect, SECTOR_SIZE) != 0) {
      printf("FAT mismatch in sector %d\n", i);
      fat_ok = false;
    }
  }
  printf("FAT copies identical: %s\n", fat_ok ? "YES" : "NO");

  // Проверяем наличие файла настроек
  if (usb_fs_file_exists("settings.ini")) {
    printf("settings.ini exists\n");

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
