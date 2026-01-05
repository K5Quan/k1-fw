#ifndef FLASH_SYNC_H
#define FLASH_SYNC_H

#include <stdbool.h>
#include <stdint.h>

// Типы блокировок
typedef enum {
    FLASH_LOCK_NONE = 0,
    FLASH_LOCK_FS,      // Файловая система
    FLASH_LOCK_USB,     // USB MSC
    FLASH_LOCK_DEBUG    // Отладка
} flash_lock_type_t;

// Инициализация
void flash_sync_init(void);

// Блокировки с таймаутами
bool flash_lock_fs(uint32_t timeout_ms);      // Для файловой системы
bool flash_lock_usb(uint32_t timeout_ms);     // Для USB
void flash_unlock(void);                      // Разблокировка

// Проверки
bool flash_is_locked(void);
flash_lock_type_t flash_get_lock_type(void);
uint32_t flash_get_lock_time(void);

// Для отладки
void flash_debug_info(void);
void flash_reset_lock(void);  // Аварийный сброс блокировки

#endif // FLASH_SYNC_H
