#include "board.h"
#include "driver/audio.h"
#include "driver/bk4819-regs.h"
#include "driver/bk4829.h"
#include "driver/gpio.h"
#include "driver/systick.h"
#include "helper/audio_io.h"
#include "system.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

int main(void) {
  SYSTICK_Init();

  BOARD_Init();

  AUDIO_IO_Init();

  GPIO_TurnOnBacklight();

  /* BK4819_Init();
  // это включает помехуйство
  BK4819_ToggleGpioOut(BK4819_GPIO0_PIN28_RX_ENABLE, true);

  BK4819_RX_TurnOn();

  AUDIO_ToggleSpeaker(true);

  BK4819_SelectFilter(25230000);
  BK4819_TuneTo(25230000, true);

  __disable_irq();
  for (;;) {
    SYSTICK_DelayMs(1000);
  } */

  /* BK4819_Init();
  BK4819_ToggleGpioOut(BK4819_GPIO0_PIN28_RX_ENABLE, false);
  BK4819_TuneTo(43400000, true);

  for (;;) {
    BK4819_PrepareTransmit();
    SYSTICK_DelayMs(10);
    BK4819_ToggleGpioOut(BK4819_GPIO1_PIN29_PA_ENABLE, true);
    SYSTICK_DelayMs(5);
    BK4819_SetupPowerAmplifier(1, 43400000);

    SYSTICK_DelayMs(3);

    BK4819_ToggleGpioOut(BK4819_GPIO1_PIN29_PA_ENABLE, false);
    BK4819_SetupPowerAmplifier(0, 0);
    BK4819_TurnsOffTones_TurnsOnRX();

    SYSTICK_DelayMs(250);
  } */

  SYS_Main();
}
