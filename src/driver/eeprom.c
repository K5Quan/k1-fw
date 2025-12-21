#include "eeprom.h"
#include "py25q16.h"
#include <stdbool.h>
#include <string.h>

bool gEepromWrite = false;

void EEPROM_Init(void) {}

void EEPROM_ReadBuffer(uint32_t address, void *pBuffer, uint16_t size) {
  PY25Q16_ReadBuffer(address, pBuffer, size);
}

void EEPROM_WriteBuffer(uint32_t address, uint8_t *pBuffer, uint16_t size) {
  gEepromWrite = true;
  PY25Q16_WriteBuffer(address, pBuffer, size, true);
}

void EEPROM_ClearPage(uint16_t page) {
  uint8_t buf[256];
  memset(buf, 0xff, sizeof(buf));
  gEepromWrite = true;
  PY25Q16_WriteBuffer(page * EEPROM_GetPageSize(), buf, EEPROM_GetPageSize(),
                      false);
}

EEPROMType EEPROM_DetectType(void) { return EEPROM_M24M02; }

uint32_t EEPROM_DetectSize(void) { return EEPROM_SIZES[EEPROM_DetectType()]; }

uint16_t EEPROM_GetPageSize(void) { return PAGE_SIZES[EEPROM_DetectType()]; }

void EEPROM_ReadByte(uint32_t address, uint8_t *value) {
  EEPROM_ReadBuffer(address, value, 1);
}

int EEPROM_WriteByte(uint32_t address, uint8_t value) {
  EEPROM_WriteBuffer(address, &value, 1);
  return 0;
}
