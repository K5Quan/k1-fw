#include "ch.h"
#include "../driver/fat.h"
#include "../external/printf/printf.h"
#include <string.h>

#define MAX_CHANNELS 1024
#define CH_SZ sizeof(CH)

bool CH_Init(const char *name) {
  if (usb_fs_file_exists(name)) {
    return true;
  }

  return usb_fs_create_file(name, MAX_CHANNELS * CH_SZ) > 0;
}

bool CH_Load(const char *name, uint16_t num, CH *ch) {
  if (!usb_fs_file_exists(name)) {
    printf("[CH_Load] File does not exist\n");
    return false;
  }

  usb_fs_handle_t handle;
  if (usb_fs_open(name, &handle) != 0) {
    printf("[CH_Load] Cannot open file\n");
    return false;
  }

  if ((num * CH_SZ) >= handle.file_size) {
    printf("[CH_Load] Offset beyond file size\n");
    usb_fs_close(&handle);
    return false;
  }

  if (usb_fs_seek(&handle, num * CH_SZ) != 0) {
    printf("[CH_Load] Seek failed\n");
    usb_fs_close(&handle);
    return false;
  }

  memset(ch, 0, sizeof(CH));

  size_t read = usb_fs_read_bytes(&handle, (uint8_t *)ch, CH_SZ);
  usb_fs_close(&handle);

  return read == CH_SZ;
}

bool CH_Save(const char *name, uint16_t num, CH *ch) {
  if (!usb_fs_file_exists(name)) {
    printf("[CH_Save] File does not exist, creating...\n");
    if (!CH_Init(name)) {
      return false;
    }
  }

  usb_fs_handle_t handle;
  if (usb_fs_open(name, &handle) != 0) {
    printf("[CH_Save] Cannot open file\n");
    return false;
  }

  // Проверяем, нужно ли расширять файл
  uint32_t required_pos = (num + 1) * CH_SZ;
  if (required_pos > handle.file_size) {
    printf("[CH_Save] File needs extension from %lu to %u\n", handle.file_size,
           required_pos);

    // Закрываем handle чтобы обновить размер через write_file
    usb_fs_close(&handle);

    // Просто записываем файл через usb_fs_write_file с append
    uint8_t zero_data = 0;
    if (usb_fs_write_file(name, &zero_data, 0, true) != 0) {
      // Это обновит размер файла
      printf("[CH_Save] Failed to extend file\n");
      return false;
    }

    // Снова открываем
    if (usb_fs_open(name, &handle) != 0) {
      printf("[CH_Save] Cannot reopen file\n");
      return false;
    }
  }

  // Перемещаемся к позиции
  if (usb_fs_seek(&handle, num * CH_SZ) != 0) {
    printf("[CH_Save] Seek failed\n");
    usb_fs_close(&handle);
    return false;
  }

  // Записываем данные
  size_t written = usb_fs_write_bytes(&handle, (const uint8_t *)ch, CH_SZ);

  if (written == CH_SZ) {
    // Обновляем запись в директории
    if (usb_fs_flush(&handle, name) == 0) {
      // printf("[CH_Save] Flush successful\n");
    } else {
      printf("[CH_Save] Flush failed\n");
    }
  }

  usb_fs_close(&handle);

  return written == CH_SZ;
}
