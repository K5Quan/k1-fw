// flash_sync.c
#include "flash_sync.h"
#include "py32f071_ll_system.h"

static volatile bool flash_busy = false;

void flash_lock(void) {
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  while (flash_busy) {
    __enable_irq();
    for (volatile int i = 0; i < 1000; i++)
      __NOP();
    __disable_irq();
  }
  flash_busy = true;
  __set_PRIMASK(primask);
}

void flash_unlock(void) {
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  flash_busy = false;
  __set_PRIMASK(primask);
}

bool flash_is_locked(void) { return flash_busy; }
