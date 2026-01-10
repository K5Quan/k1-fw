#include "storage.h"
#include "../driver/fat.h"
#include "../external/printf/printf.h"
#include <string.h>

bool Storage_Init(const char *name, size_t item_size, uint16_t max_items) {
  if (usb_fs_file_exists(name)) {
    return true;
  }

  return usb_fs_create_file(name, max_items * item_size) > 0;
}

bool Storage_Load(const char *name, uint16_t num, void *item,
                  size_t item_size) {
  if (!usb_fs_file_exists(name)) {
    printf("[Storage_Load] File does not exist: %s\n", name);
    return false;
  }

  usb_fs_handle_t handle;
  if (usb_fs_open(name, &handle) != 0) {
    printf("[Storage_Load] Cannot open file: %s\n", name);
    return false;
  }

  uint32_t offset = num * item_size;
  if (offset >= handle.file_size) {
    printf("[Storage_Load] Offset %lu beyond file size %lu\n", offset,
           handle.file_size);
    usb_fs_close(&handle);
    return false;
  }

  if (usb_fs_seek(&handle, offset) != 0) {
    printf("[Storage_Load] Seek failed at offset %lu\n", offset);
    usb_fs_close(&handle);
    return false;
  }

  memset(item, 0, item_size);

  size_t read = usb_fs_read_bytes(&handle, (uint8_t *)item, item_size);
  usb_fs_close(&handle);

  if (read != item_size) {
    printf("[Storage_Load] Read %zu bytes, expected %zu\n", read, item_size);
    return false;
  }

  return true;
}

bool Storage_Save(const char *name, uint16_t num, const void *item,
                  size_t item_size) {
  if (!usb_fs_file_exists(name)) {
    printf("[Storage_Save] File does not exist, creating: %s\n", name);
    // Создаём файл с размером для первого элемента
    if (!Storage_Init(name, item_size, num + 1)) {
      return false;
    }
  }

  usb_fs_handle_t handle;
  if (usb_fs_open(name, &handle) != 0) {
    printf("[Storage_Save] Cannot open file: %s\n", name);
    return false;
  }

  // Проверяем, нужно ли расширить файл
  uint32_t required_pos = (num + 1) * item_size;
  if (required_pos > handle.file_size) {
    printf("[Storage_Save] File needs extension from %lu to %u\n",
           handle.file_size, required_pos);

    // Закрываем handle чтобы обновить размер через write_file
    usb_fs_close(&handle);

    // Просто записываем файл через usb_fs_write_file с append
    uint8_t zero_data = 0;
    if (usb_fs_write_file(name, &zero_data, 0, true) != 0) {
      printf("[Storage_Save] Failed to extend file\n");
      return false;
    }

    // Снова открываем
    if (usb_fs_open(name, &handle) != 0) {
      printf("[Storage_Save] Cannot reopen file\n");
      return false;
    }
  }

  // Перемещаемся к позиции
  uint32_t offset = num * item_size;
  if (usb_fs_seek(&handle, offset) != 0) {
    printf("[Storage_Save] Seek failed at offset %lu\n", offset);
    usb_fs_close(&handle);
    return false;
  }

  // Записываем данные
  size_t written =
      usb_fs_write_bytes(&handle, (const uint8_t *)item, item_size);

  if (written == item_size) {
    // Обновляем запись в директории
    if (usb_fs_flush(&handle, name) != 0) {
      printf("[Storage_Save] Flush failed\n");
    }
  } else {
    printf("[Storage_Save] Written %zu bytes, expected %zu\n", written,
           item_size);
  }

  usb_fs_close(&handle);

  return written == item_size;
}
