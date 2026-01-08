#include "systick.h"
#include "py32f0xx.h"

static uint32_t gTickMultiplier;
static volatile uint32_t gGlobalSysTickCounter;

void SYSTICK_Init(void) {
  LL_SetSystemCoreClock(48000000);
  SystemCoreClockUpdate();
  LL_Init1msTick(SystemCoreClock);
  SysTick_Config(48000);
  gTickMultiplier = 48;

  NVIC_SetPriority(SysTick_IRQn, 0);
}

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
  SYSTICK_DelayTicks(Delay * gTickMultiplier);
}

void SysTick_Handler(void) { gGlobalSysTickCounter++; }

uint32_t Now() { return gGlobalSysTickCounter; }

void SYSTICK_DelayMs(uint32_t ms) { SYSTICK_DelayUs(ms * 1000); }

void SetTimeout(uint32_t *v, uint32_t t) {
  *v = t == UINT32_MAX ? UINT32_MAX : Now() + t;
}

bool CheckTimeout(uint32_t *v) { return Now() >= *v; }
