#ifndef USB_FS_H
#define USB_FS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Константы
#define SECTOR_SIZE 512
#define MAX_FILES 16
#define MAX_FILENAME 11
#define MAX_FILE_SIZE (128 * 1024) // 128KB на файл

// Структуры FAT16
#pragma pack(push, 1)

typedef struct {
  uint8_t jump_boot[3];
  char OEM_name[8];
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
  uint8_t reserved;
  uint8_t boot_signature;
  uint32_t volume_ID;
  char volume_label[11];
  char fs_type[8];
  uint8_t boot_code[448];
  uint16_t signature;
} fat_boot_sector_t;

typedef struct {
  char name[11];
  uint8_t attr;
  uint8_t reserved;
  uint8_t create_time_tenth;
  uint16_t create_time;
  uint16_t create_date;
  uint16_t last_access_date;
  uint16_t first_clusterHI;
  uint16_t write_time;
  uint16_t write_date;
  uint16_t first_clusterLO;
  uint32_t file_size;
} fat_dir_entry_t;

#pragma pack(pop)

// Структура для информации о файле
typedef struct {
  char name[MAX_FILENAME + 1];
  uint32_t size;
  uint16_t create_date;
  uint16_t create_time;
  uint16_t write_date;
  uint16_t write_time;
} file_info_t;

// Структура дескриптора файла
typedef struct {
  uint16_t first_cluster;
  uint32_t file_size;
  uint32_t position;
  uint16_t current_cluster;
  uint32_t current_position_in_cluster;
} usb_fs_handle_t;

// Макросы для работы с датой/временем FAT
#define FAT_MK_DATE(year, month, day)                                          \
  ((((year) - 1980) << 9) | ((month) << 5) | (day))

#define FAT_MK_TIME(hour, min, sec)                                            \
  (((hour) << 11) | ((min) << 5) | ((sec) / 2))

#define FAT_GET_YEAR(date) (((date) >> 9) + 1980)
#define FAT_GET_MONTH(date) (((date) >> 5) & 0xF)
#define FAT_GET_DAY(date) ((date) & 0x1F)

#define FAT_GET_HOUR(time) ((time) >> 11)
#define FAT_GET_MIN(time) (((time) >> 5) & 0x3F)
#define FAT_GET_SEC(time) (((time) & 0x1F) * 2)

// Инициализация
void usb_fs_init(void);

// Работа с файлами
int usb_fs_write_file(const char *name, const uint8_t *data, uint32_t size,
                      bool append);
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

// Форматирование
void usb_fs_format(void);

// USB Mass Storage callbacks (реализуются драйвером USB)
void usb_fs_configure_done(void);
void usb_fs_get_cap(uint32_t *sector_num, uint16_t *sector_size);
int usb_fs_sector_read(uint32_t sector, uint8_t *buf, uint32_t size);
int usb_fs_sector_write(uint32_t sector, const uint8_t *buf, uint32_t size);

// Callback для проверки защиты от записи (должен возвращать false)
static inline bool usb_fs_is_write_protected(void) {
  return false; // Разрешаем запись
}

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

// Функции работы с открытыми файлами
int usb_fs_open(const char *name, usb_fs_handle_t *handle);
size_t usb_fs_read_bytes(usb_fs_handle_t *handle, uint8_t *buf, size_t len);

#endif // USB_FS_H
