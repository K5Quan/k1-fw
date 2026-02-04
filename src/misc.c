#include "misc.h"
#include "driver/bk4829.h"
#include "driver/uart.h"
#include "external/printf/printf.h"

char IsPrintable(char ch) { return (ch < 32 || 126 < ch) ? ' ' : ch; }

// return square root of 'value'
unsigned int SQRT16(unsigned int value) {
  unsigned int shift = 16; // number of bits supplied in 'value' .. 2 ~ 32
  unsigned int bit = 1u << --shift;
  unsigned int sqrti = 0;
  while (bit) {
    const unsigned int temp = ((sqrti << 1) | bit) << shift--;
    if (value >= temp) {
      value -= temp;
      sqrti |= bit;
    }
    bit >>= 1;
  }
  return sqrti;
}

void _putchar(char c) { UART_Send((uint8_t *)&c, 1); }
void _init() {}

// Самый простой обработчик HardFault
void HardFault_Handler(void) {
  uint32_t stacked_pc, stacked_lr, stacked_sp;

  // Получаем значения из стека
  __asm volatile("mrs %0, msp \n" // MSP -> stacked_sp
                 : "=r"(stacked_sp));

  // PC и LR сохранены на стеке
  stacked_pc = ((uint32_t *)stacked_sp)[6];
  stacked_lr = ((uint32_t *)stacked_sp)[5];

  // Просто выводим информацию
  LogC(LOG_C_BRIGHT_RED, "!!! HARD FAULT !!!");
  LogC(LOG_C_RED, "PC: 0x%08X", stacked_pc);
  LogC(LOG_C_RED, "LR: 0x%08X", stacked_lr);
  LogC(LOG_C_RED, "SP: 0x%08X", stacked_sp);

  // Проверка на переполнение стека
  extern uint32_t _estack;
  uint32_t stack_end = (uint32_t)&_estack;

  if (stacked_sp > stack_end) {
    LogC(LOG_C_BRIGHT_RED, "STACK OVERFLOW DETECTED!");
    LogC(LOG_C_RED, "SP (0x%08X) > StackEnd (0x%08X)", stacked_sp, stack_end);
  }

  // Останавливаем систему
  while (1) {
    // Мигаем или делаем что-то для индикации
  }
}

void ScanlistStr(uint32_t sl, char *buf) {
  for (uint8_t i = 0; i < 16; i++) {
    bool sel = sl & (1 << i);
    if (i < 8) {
      buf[i] = sel ? '1' + i : '_';
    } else {
      buf[i] = sel ? 'A' + (i - 8) : '_';
    }
  }
}

void mhzToS(char *buf, uint32_t f) {
  sprintf(buf, "%u.%05u", f / MHZ, f % MHZ);
}

void bkAttToS(char *buf, uint8_t v) {
  sprintf(buf, v == 0 ? "Auto" : "%udB", GAIN_TABLE[v].gainDb);
}
