#include "flash_sync.h"
#include "py32f071_ll_system.h"
#include "systick.h"
#include <stdbool.h>

// Глобальные переменные
static volatile bool flash_busy = false;
static volatile flash_lock_type_t lock_type = FLASH_LOCK_NONE;
static volatile uint32_t lock_start_time = 0;
static volatile uint32_t lock_attempts = 0;
static volatile uint32_t lock_timeouts = 0;
static volatile uint32_t max_lock_time = 0;

// Приоритеты блокировок (чем выше, тем приоритетнее)
#define LOCK_PRIORITY_FS 2
#define LOCK_PRIORITY_USB 1

// Максимальное время удержания блокировки (мс)
#define MAX_LOCK_TIME_MS 5000

void flash_sync_init(void) {
  flash_busy = false;
  lock_type = FLASH_LOCK_NONE;
  lock_start_time = 0;
  lock_attempts = 0;
  lock_timeouts = 0;
  max_lock_time = 0;
}

// Блокировка для файловой системы (более высокий приоритет)
bool flash_lock_fs(uint32_t timeout_ms) {
  uint32_t start = Now();
  lock_attempts++;

  while (flash_busy) {
    // Если блокировка удерживается слишком долго, принудительно сбрасываем
    if (lock_start_time > 0 && (Now() - lock_start_time) > MAX_LOCK_TIME_MS) {
      flash_reset_lock();
      printf("WARNING: Forced lock reset (held for %lu ms)\n",
             Now() - lock_start_time);
    }

    if (Now() - start > timeout_ms) {
      lock_timeouts++;
      return false;
    }

    // Короткая задержка с проверкой прерываний
    for (volatile int i = 0; i < 100; i++) {
      __NOP();
    }

    // Если USB держит блокировку, даем ему шанс освободить
    if (lock_type == FLASH_LOCK_USB) {
      // Разрешаем прерывания на короткое время
      uint32_t primask = __get_PRIMASK();
      __enable_irq();
      for (volatile int i = 0; i < 10; i++)
        __NOP();
      __set_PRIMASK(primask);
    }
  }

  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  flash_busy = true;
  lock_type = FLASH_LOCK_FS;
  lock_start_time = Now();
  __set_PRIMASK(primask);

  return true;
}

// Блокировка для USB (низкий приоритет, с проверкой прерываний)
bool flash_lock_usb(uint32_t timeout_ms) {
  uint32_t start = Now();

  while (flash_busy) {
    // USB не должен ждать слишком долго
    if (Now() - start > timeout_ms) {
      return false;
    }

    // Короткая задержка
    for (volatile int i = 0; i < 50; i++) {
      __NOP();
    }
  }

  // USB получает блокировку только если флеш свободна
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  flash_busy = true;
  lock_type = FLASH_LOCK_USB;
  lock_start_time = Now();
  __set_PRIMASK(primask);

  return true;
}

void flash_unlock(void) {
  uint32_t primask = __get_PRIMASK();
  __disable_irq();

  if (flash_busy) {
    uint32_t lock_time = Now() - lock_start_time;
    if (lock_time > max_lock_time) {
      max_lock_time = lock_time;
    }
  }

  flash_busy = false;
  lock_type = FLASH_LOCK_NONE;
  lock_start_time = 0;

  __set_PRIMASK(primask);

  // Даем время на обработку прерываний
  for (volatile int i = 0; i < 100; i++) {
    __NOP();
  }
}

bool flash_is_locked(void) { return flash_busy; }

flash_lock_type_t flash_get_lock_type(void) { return lock_type; }

uint32_t flash_get_lock_time(void) {
  if (!flash_busy)
    return 0;
  return Now() - lock_start_time;
}

void flash_debug_info(void) {
  printf("Flash lock status:\n");
  printf("  Busy: %s\n", flash_busy ? "YES" : "NO");
  printf("  Type: ");
  switch (lock_type) {
  case FLASH_LOCK_NONE:
    printf("NONE\n");
    break;
  case FLASH_LOCK_FS:
    printf("FS\n");
    break;
  case FLASH_LOCK_USB:
    printf("USB\n");
    break;
  case FLASH_LOCK_DEBUG:
    printf("DEBUG\n");
    break;
  default:
    printf("UNKNOWN\n");
    break;
  }
  printf("  Held for: %lu ms\n", flash_get_lock_time());
  printf("  Attempts: %lu, Timeouts: %lu\n", lock_attempts, lock_timeouts);
  printf("  Max lock time: %lu ms\n", max_lock_time);
}

void flash_reset_lock(void) {
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  flash_busy = false;
  lock_type = FLASH_LOCK_NONE;
  lock_start_time = 0;
  __set_PRIMASK(primask);
  printf("Flash lock forcibly reset\n");
}
