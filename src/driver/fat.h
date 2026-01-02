#ifndef _FAT_H
#define _FAT_H

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

// Boot ----------

#define FAT_DEFAULT_SECTOR_SIZE 512
#define FAT_SIGNATURE_WORD 0xaa55
#define FAT_BOOT_SIGNATURE_ENABLE 0x29

#define SECTOR_SIZE FAT_DEFAULT_SECTOR_SIZE
#define SECTOR_NUM 32000

#define BOOT_SECTOR 0
#define FAT_SECTOR 1
#define FAT_SECTOR_NUM 125
#define ROOT_SECTOR (FAT_SECTOR + FAT_SECTOR_NUM)
#define ROOT_SECTOR_NUM 32
#define DATA_SECTOR (ROOT_SECTOR + ROOT_SECTOR_NUM)
#define DATA_SECTOR_NUM (SECTOR_NUM - DATA_SECTOR)

#define FAT_ENTRY_SIZE 2
#define FAT_ENTRIES_PER_SECTOR (SECTOR_SIZE / FAT_ENTRY_SIZE)

#define DIR_ENTRIES_PER_SECTOR (SECTOR_SIZE / FAT_DIR_ENTRY_SIZE)

#define DATA_SECTOR_TO_FAT_ENTRY(n) (2 + (n))
#define FAT_ENTRY_TO_SECTOR(n) ((n) / FAT_ENTRIES_PER_SECTOR)

#define MAX_FILES 16
#define MAX_FILENAME 11
#define MAX_FILE_SIZE (128 * 1024) // 128KB на файл

// Структура для информации о файле
typedef struct {
  char name[MAX_FILENAME + 1];
  uint32_t size;
  uint16_t create_date;
  uint16_t create_time;
  uint16_t write_date;
  uint16_t write_time;
} file_info_t;

#define FAT_GET_YEAR(date) (((date) >> 9) + 1980)
#define FAT_GET_MONTH(date) (((date) >> 5) & 0xF)
#define FAT_GET_DAY(date) ((date) & 0x1F)

#define FAT_GET_HOUR(time) ((time) >> 11)
#define FAT_GET_MIN(time) (((time) >> 5) & 0x3F)
#define FAT_GET_SEC(time) (((time) & 0x1F) * 2)

typedef struct {
  uint8_t jump_boot[3];
  uint8_t OEM_name[8];
  uint16_t sector_size;
  uint8_t sectors_per_cluster;
  uint16_t reserved_sectors;
  uint8_t num_FATs;
  uint16_t root_entries;
  uint16_t total_sectors16;
  uint8_t media;
  uint16_t FAT_sectors16;
  uint16_t sectors_per_track;
  uint16_t num_heads;
  uint32_t hidden_sectors;
  uint32_t total_sectors32;
  uint8_t drive_num;
  uint8_t reserved1;
  uint8_t boot_signature;
  uint32_t volume_ID;
  uint8_t volume_label[11];
  uint8_t fs_type[8];
} __attribute__((packed)) fat_boot_sector_t;

static_assert(sizeof(fat_boot_sector_t) == 62);

// FAT ------------

#define FAT16_ENTRY_FREE 0
#define FAT16_ENTRY_EOF 0xffff

#define FAT12_ENTRY_FREE 0
#define FAT12_ENTRY_EOF 0xfff

// Root -------------

#define FAT_DIR_ENTRY_SIZE 32

#define FAT_MK_DATE(y, m, d)                                                   \
  ((uint16_t)(((0x7f & ((y) - 1980)) << 9) | ((0xf & (m)) << 5) | (0x1f & (d))))
#define FAT_MK_TIME(h, m, s)                                                   \
  ((uint16_t)(((0x1f & (h)) << 11) | ((0x3f & (m)) << 5) | (0x1f & ((s) / 2))))

typedef struct {
  uint8_t name[11];
  uint8_t attr;
  uint8_t NTRes;
  uint8_t create_time_tenth;
  uint16_t create_time;
  uint16_t create_date;
  uint16_t last_access_date;
  uint16_t first_clusterHI;
  uint16_t write_time;
  uint16_t write_date;
  uint16_t first_clusterLO;
  uint32_t file_size;
} __attribute__((packed)) fat_dir_entry_t;

static_assert(sizeof(fat_dir_entry_t) == FAT_DIR_ENTRY_SIZE);

enum {
  FAT_DIR_ATTR_RO = 0x1,
  FAT_DIR_ATTR_HIDDEN = 0x2,
  FAT_DIR_ATTR_SYSTEM = 0x4,
  FAT_DIR_ATTR_VOLUME_ID = 0x8,
  FAT_DIR_ATTR_DIR = 0x10,
  FAT_DIR_ATTR_ARCHIVE = 0x20,
};

// Misc --------------

static inline void fat_set_word(uint8_t *buf, uint16_t value) {
  buf[0] = 0xff & value;
  buf[1] = 0xff & (value >> 8);
}

static inline void fat_set_dword(uint8_t *buf, uint32_t value) {
  for (int i = 0; i < 4; i++) {
    buf[i] = 0xff & value;
    value >>= 8;
  }
}

// Инициализация
void usb_fs_init(void);

// Работа с файлами
int usb_fs_write_file(const char *name, const uint8_t *data, uint32_t size);
int usb_fs_read_file(const char *name, uint8_t *data, uint32_t *size);
int usb_fs_delete_file(const char *name);

// Получение информации
int usb_fs_list_files(file_info_t *list, int max_count);
int usb_fs_get_file_count(void);
int usb_fs_get_file_info(const char *name, file_info_t *info);
bool usb_fs_file_exists(const char *name);

// Информация о диске
uint32_t usb_fs_get_free_space(void);
uint32_t usb_fs_get_total_space(void);

// USB Mass Storage callbacks (реализуются драйвером USB)
void usb_fs_configure_done(void);
void usb_fs_get_cap(uint32_t *sector_num, uint16_t *sector_size);
int usb_fs_sector_read(uint32_t sector, uint8_t *buf, uint32_t size);
int usb_fs_sector_write(uint32_t sector, const uint8_t *buf, uint32_t size);

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
static inline void fat_unformat_name(const char *fat_name, char *filename) {
  int i, j = 0;

  // Копирование имени
  for (i = 0; i < 8 && fat_name[i] != ' '; i++) {
    filename[j++] = fat_name[i];
  }

  // Добавление точки и расширения
  if (fat_name[8] != ' ') {
    filename[j++] = '.';
    for (i = 8; i < 11 && fat_name[i] != ' '; i++) {
      filename[j++] = fat_name[i];
    }
  }

  filename[j] = '\0';
}

#endif
