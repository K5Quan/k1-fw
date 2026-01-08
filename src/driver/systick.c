#include "systick.h"
#include "py32f0xx.h"

static volatile uint32_t elapsedMilliseconds;
static const uint32_t TICK_MULTIPLIER = 48;

void SYSTICK_Init(void) {
  LL_SetSystemCoreClock(48000000);
  SystemCoreClockUpdate();
  // LL_Init1msTick(SystemCoreClock);
  SysTick_Config(48000);

  NVIC_SetPriority(SysTick_IRQn, 0);
}

void SysTick_Handler(void) { elapsedMilliseconds++; }

uint32_t Now() { return elapsedMilliseconds; }

void SYSTICK_DelayTicks(const uint32_t ticks) {
  uint32_t elapsed_ticks = 0;
  uint32_t Start = SysTick->LOAD;
  uint32_t Previous = SysTick->VAL;

  do {
    uint32_t Current = SysTick->VAL;
    if (Current != Previous) {
      uint32_t Delta = (Current < Previous) ? (Previous - Current)
                                            : (Start - Current + Previous);

      elapsed_ticks += Delta;
      Previous = Current;
    }
  } while (elapsed_ticks < ticks);
}

void SYSTICK_DelayUs(const uint32_t Delay) {
  SYSTICK_DelayTicks(Delay * TICK_MULTIPLIER);
}

void SYSTICK_DelayMs(uint32_t ms) { SYSTICK_DelayUs(ms * 1000); }

void SetTimeout(uint32_t *v, uint32_t t) {
  *v = t == UINT32_MAX ? UINT32_MAX : Now() + t;
}

bool CheckTimeout(uint32_t *v) {
  if (*v == UINT32_MAX) {
    return false;
  }
  return (int32_t)(Now() - *v) >= 0;
}
