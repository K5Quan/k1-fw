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
size_t usb_fs_create_file(const char *name, uint32_t size);

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
int usb_fs_sector_write(uint32_t sector, uint8_t *buf, uint32_t size);

// Callback для проверки защиты от записи (должен возвращать false)
static inline bool usb_fs_is_write_protected(void) {
  return false; // Разрешаем запись
}

// Функции работы с открытыми файлами
int usb_fs_open(const char *name, usb_fs_handle_t *handle);
size_t usb_fs_read_bytes(usb_fs_handle_t *handle, uint8_t *buf, size_t len);
size_t usb_fs_write_bytes(usb_fs_handle_t *handle, const uint8_t *data,
                          size_t len);
void usb_fs_close(usb_fs_handle_t *handle);
int usb_fs_seek(usb_fs_handle_t *handle, uint32_t position);
int usb_fs_flush(usb_fs_handle_t *handle, const char *name);

void debug_file_structure(const char *name);
void debug_fat_table(uint16_t start_cluster, uint16_t count);
void debug_file_info(const char *name);
void check_fat_consistency(void);

#endif // USB_FS_H
