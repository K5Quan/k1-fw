#include <string.h>

#include "../external/printf/printf.h"
#include "gpio.h"
#include "py25q16.h"
#include "py32f071_ll_bus.h"
#include "py32f071_ll_dma.h"
#include "py32f071_ll_spi.h"
#include "py32f071_ll_system.h"
#include "systick.h"

#define DEBUG

#define SPIx SPI2
#define CHANNEL_RD LL_DMA_CHANNEL_4
#define CHANNEL_WR LL_DMA_CHANNEL_5

#define CS_PIN GPIO_MAKE_PIN(GPIOA, LL_GPIO_PIN_3)

#define SECTOR_SIZE 256
#define PAGE_SIZE 0x100

static uint32_t SectorCacheAddr = 0x1000000;
static uint8_t SectorCache[SECTOR_SIZE];
static uint8_t BlackHole[1];
static volatile bool TC_Flag;

static inline void CS_Assert() { GPIO_ResetOutputPin(CS_PIN); }

static inline void CS_Release() { GPIO_SetOutputPin(CS_PIN); }

static void SPI_Init() {
  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_SPI2);
  LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_DMA1);
  LL_IOP_GRP1_EnableClock(LL_IOP_GRP1_PERIPH_GPIOA);

  do {
    // SCK: PA0
    // MOSI: PA1
    // MISO: PA2

    LL_GPIO_InitTypeDef InitStruct;
    LL_GPIO_StructInit(&InitStruct);
    InitStruct.Mode = LL_GPIO_MODE_ALTERNATE;
    InitStruct.Speed = LL_GPIO_SPEED_FREQ_VERY_HIGH;
    InitStruct.OutputType = LL_GPIO_OUTPUT_PUSHPULL;
    InitStruct.Pull = LL_GPIO_PULL_UP;

    InitStruct.Pin = LL_GPIO_PIN_0;
    InitStruct.Alternate = LL_GPIO_AF8_SPI2;
    LL_GPIO_Init(GPIOA, &InitStruct);

    InitStruct.Pin = LL_GPIO_PIN_1 | LL_GPIO_PIN_2;
    InitStruct.Alternate = LL_GPIO_AF9_SPI2;
    LL_GPIO_Init(GPIOA, &InitStruct);

  } while (0);

  LL_SYSCFG_SetDMARemap(DMA1, CHANNEL_RD, LL_SYSCFG_DMA_MAP_SPI2_RD);
  LL_SYSCFG_SetDMARemap(DMA1, CHANNEL_WR, LL_SYSCFG_DMA_MAP_SPI2_WR);

  NVIC_SetPriority(DMA1_Channel4_5_6_7_IRQn, 1);
  NVIC_EnableIRQ(DMA1_Channel4_5_6_7_IRQn);

  LL_SPI_InitTypeDef InitStruct;
  LL_SPI_StructInit(&InitStruct);
  InitStruct.Mode = LL_SPI_MODE_MASTER;
  InitStruct.TransferDirection = LL_SPI_FULL_DUPLEX;
  InitStruct.ClockPhase = LL_SPI_PHASE_2EDGE;
  InitStruct.ClockPolarity = LL_SPI_POLARITY_HIGH;
  InitStruct.BaudRate = LL_SPI_BAUDRATEPRESCALER_DIV2;
  InitStruct.BitOrder = LL_SPI_MSB_FIRST;
  InitStruct.NSS = LL_SPI_NSS_SOFT;
  InitStruct.CRCCalculation = LL_SPI_CRCCALCULATION_DISABLE;
  LL_SPI_Init(SPIx, &InitStruct);

  LL_SPI_Enable(SPIx);
}

static void SPI_ReadBuf(uint8_t *Buf, uint32_t Size) {
  LL_SPI_Disable(SPIx);
  LL_DMA_DisableChannel(DMA1, CHANNEL_RD);
  LL_DMA_DisableChannel(DMA1, CHANNEL_WR);

  LL_DMA_ClearFlag_GI4(DMA1);

  LL_DMA_ConfigTransfer(DMA1, CHANNEL_RD,                 //
                        LL_DMA_DIRECTION_PERIPH_TO_MEMORY //
                            | LL_DMA_MODE_NORMAL          //
                            | LL_DMA_PERIPH_NOINCREMENT   //
                            | LL_DMA_MEMORY_INCREMENT     //
                            | LL_DMA_PDATAALIGN_BYTE      //
                            | LL_DMA_MDATAALIGN_BYTE      //
                            | LL_DMA_PRIORITY_MEDIUM      //
  );

  LL_DMA_ConfigTransfer(DMA1, CHANNEL_WR,                 //
                        LL_DMA_DIRECTION_MEMORY_TO_PERIPH //
                            | LL_DMA_MODE_NORMAL          //
                            | LL_DMA_PERIPH_NOINCREMENT   //
                            | LL_DMA_MEMORY_NOINCREMENT   //
                            | LL_DMA_PDATAALIGN_BYTE      //
                            | LL_DMA_MDATAALIGN_BYTE      //
                            | LL_DMA_PRIORITY_MEDIUM      //
  );

  LL_DMA_SetMemoryAddress(DMA1, CHANNEL_RD, (uint32_t)Buf);
  LL_DMA_SetPeriphAddress(DMA1, CHANNEL_RD, LL_SPI_DMA_GetRegAddr(SPIx));
  LL_DMA_SetDataLength(DMA1, CHANNEL_RD, Size);

  LL_DMA_SetMemoryAddress(DMA1, CHANNEL_WR, (uint32_t)BlackHole);
  LL_DMA_SetPeriphAddress(DMA1, CHANNEL_WR, LL_SPI_DMA_GetRegAddr(SPIx));
  LL_DMA_SetDataLength(DMA1, CHANNEL_WR, Size);

  TC_Flag = false;
  LL_DMA_EnableIT_TC(DMA1, CHANNEL_RD);
  LL_DMA_EnableChannel(DMA1, CHANNEL_RD);
  LL_DMA_EnableChannel(DMA1, CHANNEL_WR);

  LL_SPI_EnableDMAReq_RX(SPIx);
  LL_SPI_Enable(SPIx);
  LL_SPI_EnableDMAReq_TX(SPIx);

  while (!TC_Flag)
    ;
}

static void SPI_WriteBuf(const uint8_t *Buf, uint32_t Size) {
  LL_SPI_Disable(SPIx);
  LL_DMA_DisableChannel(DMA1, CHANNEL_RD);
  LL_DMA_DisableChannel(DMA1, CHANNEL_WR);

  LL_DMA_ClearFlag_GI4(DMA1);

  LL_DMA_ConfigTransfer(DMA1, CHANNEL_RD,                 //
                        LL_DMA_DIRECTION_PERIPH_TO_MEMORY //
                            | LL_DMA_MODE_NORMAL          //
                            | LL_DMA_PERIPH_NOINCREMENT   //
                            | LL_DMA_MEMORY_NOINCREMENT   //
                            | LL_DMA_PDATAALIGN_BYTE      //
                            | LL_DMA_MDATAALIGN_BYTE      //
                            | LL_DMA_PRIORITY_LOW         //
  );

  LL_DMA_ConfigTransfer(DMA1, CHANNEL_WR,                 //
                        LL_DMA_DIRECTION_MEMORY_TO_PERIPH //
                            | LL_DMA_MODE_NORMAL          //
                            | LL_DMA_PERIPH_NOINCREMENT   //
                            | LL_DMA_MEMORY_INCREMENT     //
                            | LL_DMA_PDATAALIGN_BYTE      //
                            | LL_DMA_MDATAALIGN_BYTE      //
                            | LL_DMA_PRIORITY_LOW         //
  );

  LL_DMA_SetMemoryAddress(DMA1, CHANNEL_RD, (uint32_t)BlackHole);
  LL_DMA_SetPeriphAddress(DMA1, CHANNEL_RD, LL_SPI_DMA_GetRegAddr(SPIx));
  LL_DMA_SetDataLength(DMA1, CHANNEL_RD, Size);

  LL_DMA_SetMemoryAddress(DMA1, CHANNEL_WR, (uint32_t)Buf);
  LL_DMA_SetPeriphAddress(DMA1, CHANNEL_WR, LL_SPI_DMA_GetRegAddr(SPIx));
  LL_DMA_SetDataLength(DMA1, CHANNEL_WR, Size);

  TC_Flag = false;
  LL_DMA_EnableIT_TC(DMA1, CHANNEL_RD);
  LL_DMA_EnableChannel(DMA1, CHANNEL_RD);
  LL_DMA_EnableChannel(DMA1, CHANNEL_WR);

  LL_SPI_EnableDMAReq_RX(SPIx);
  LL_SPI_Enable(SPIx);
  LL_SPI_EnableDMAReq_TX(SPIx);

  while (!TC_Flag)
    ;
}

static uint8_t SPI_WriteByte(uint8_t Value) {
  while (!LL_SPI_IsActiveFlag_TXE(SPIx))
    ;
  LL_SPI_TransmitData8(SPIx, Value);
  while (!LL_SPI_IsActiveFlag_RXNE(SPIx))
    ;
  return LL_SPI_ReceiveData8(SPIx);
}

static void WriteAddr(uint32_t Addr);
static uint8_t ReadStatusReg(uint32_t Which);
static void WaitWIP();
static void WriteEnable();
static void SectorErase(uint32_t Addr);
static void SectorProgram(uint32_t Addr, const uint8_t *Buf, uint32_t Size);
static void PageProgram(uint32_t Addr, const uint8_t *Buf, uint32_t Size);

void PY25Q16_Init() {
  CS_Release();
  SPI_Init();
}

void PY25Q16_ReadBuffer(uint32_t Address, void *pBuffer, uint32_t Size) {
  /* #ifdef DEBUG
      printf("spi flash read: %06x %ld\n", Address, Size);
  #endif */
  CS_Assert();

  SPI_WriteByte(0x03); // Fast read
  WriteAddr(Address);

  if (Size >= 16) {
    SPI_ReadBuf((uint8_t *)pBuffer, Size);
  } else {
    for (uint32_t i = 0; i < Size; i++) {
      ((uint8_t *)(pBuffer))[i] = SPI_WriteByte(0xff);
    }
  }

  CS_Release();
}

void PY25Q16_WriteBuffer(uint32_t Address, const void *pBuffer, uint32_t Size,
                         bool Append) {
#ifdef DEBUG
  printf("spi flash write: %06lx %lu %d\n", Address, Size, Append);
#endif

  // Защита от слишком быстрых операций записи
  static uint32_t last_write_time = 0;
  uint32_t current_time = Now(); // Нужна функция получения времени

  // Если с последней записи прошло менее 10мс, ждем
  if (current_time - last_write_time < 10) {
    SYSTICK_DelayMs(10 - (current_time - last_write_time));
  }

  const uint8_t *ptr = (const uint8_t *)pBuffer;
  uint32_t written = 0;

  while (written < Size) {
    uint32_t page_addr = Address + written;
    uint32_t page_offset = page_addr % PAGE_SIZE;
    uint32_t to_write = PAGE_SIZE - page_offset;

    if (to_write > Size - written) {
      to_write = Size - written;
    }

    PageProgram(page_addr, ptr + written, to_write);
    written += to_write;

    // Обновляем время последней записи
    last_write_time = Now();
  }
}

void PY25Q16_SectorErase(uint32_t Address) {
  Address -= (Address % SECTOR_SIZE);
  SectorErase(Address);
  if (SectorCacheAddr == Address) {
    memset(SectorCache, 0xff, SECTOR_SIZE);
  }
}

static inline void WriteAddr(uint32_t Addr) {
  SPI_WriteByte(0xff & (Addr >> 16));
  SPI_WriteByte(0xff & (Addr >> 8));
  SPI_WriteByte(0xff & Addr);
}

static uint8_t ReadStatusReg(uint32_t Which) {
  uint8_t Cmd;
  switch (Which) {
  case 0:
    Cmd = 0x5;
    break;
  case 1:
    Cmd = 0x35;
    break;
  case 2:
    Cmd = 0x15;
    break;
  default:
    return 0;
  }

  CS_Assert();
  SPI_WriteByte(Cmd);
  uint8_t Value = SPI_WriteByte(0xff);
  CS_Release();

  return Value;
}

static void WaitWIP() {
  uint32_t timeout = 5000000; // Увеличиваем до 5 секунд

  printf("WaitWIP: ");

  for (uint32_t i = 0; i < timeout; i++) {
    uint8_t Status = ReadStatusReg(0);

    if ((Status & 0x01) == 0) { // WIP бит очищен
      printf("OK (iterations: %lu)\n", i);
      return;
    }

    // Периодически выводим точку
    if (i % 50000 == 0) {
      printf(".");
    }

    SYSTICK_DelayUs(10); // Увеличиваем задержку

    // Если долго ждем, пробуем сбросить флешку
    if (i == 1000000) { // Через 1 секунду ожидания
      printf("\n  Attempting flash reset...\n");

      CS_Assert();
      SPI_WriteByte(0x66); // Enable Reset
      CS_Release();

      SYSTICK_DelayMs(1);

      CS_Assert();
      SPI_WriteByte(0x99); // Reset
      CS_Release();

      SYSTICK_DelayMs(10); // Даем время на сброс

      printf("  Flash reset complete, continuing wait...\n");
    }
  }

  printf("\nFATAL: WaitWIP timeout after 5 seconds!\n");

  // Критическая ошибка - перезагрузка системы
  printf("System reset required\n");
  for (int i = 0; i < 10000000; i++)
    __NOP();
  NVIC_SystemReset();
}

static void PageProgram(uint32_t Addr, const uint8_t *Buf, uint32_t Size) {
#ifdef DEBUG
  printf("spi flash page program: %06lx %lu\n", Addr, Size);
#endif

  // Проверка на переполнение страницы
  if (Size > PAGE_SIZE) {
    Size = PAGE_SIZE;
  }

  WriteEnable();

  // Задержка перед началом операции
  for (int i = 0; i < 100; i++) {
    __NOP();
  }

  CS_Assert();
  SPI_WriteByte(0x02); // Page Program command
  WriteAddr(Addr);

  // Простая запись без DMA
  for (uint32_t i = 0; i < Size; i++) {
    SPI_WriteByte(Buf[i]);
  }

  CS_Release();

  // Задержка после команды
  for (int i = 0; i < 100; i++) {
    __NOP();
  }

  WaitWIP();
}

static void WriteEnable() {
  CS_Assert();
  SPI_WriteByte(0x6);
  CS_Release();
}

static void SectorErase(uint32_t Addr) {
#ifdef DEBUG
  printf("spi flash sector erase: %06lx (starting)\n", Addr);
#endif

  WriteEnable();

  // Проверяем статус перед стиранием
  uint8_t status_before = ReadStatusReg(0);
  printf("Status before erase: 0x%02X\n", status_before);

  CS_Assert();
  SPI_WriteByte(0x20); // Sector Erase (4KB)
  WriteAddr(Addr);
  CS_Release();

  printf("Erase command sent, waiting...\n");

  WaitWIP();

  // Проверяем статус после стирания
  uint8_t status_after = ReadStatusReg(0);
  printf("Status after erase: 0x%02X\n", status_after);

#ifdef DEBUG
  printf("spi flash sector erase: %06lx (done)\n", Addr);
#endif
}

static void SectorProgram(uint32_t Addr, const uint8_t *Buf, uint32_t Size) {
  uint32_t Size1 = PAGE_SIZE - (Addr % PAGE_SIZE);

  while (Size) {
    if (Size < Size1) {
      Size1 = Size;
    }

    PageProgram(Addr, Buf, Size1);

    Addr += Size1;
    Buf += Size1;
    Size -= Size1;

    Size1 = PAGE_SIZE;
  }
}

void DMA1_Channel4_5_6_7_IRQHandler() {
  if (LL_DMA_IsActiveFlag_TC4(DMA1) &&
      LL_DMA_IsEnabledIT_TC(DMA1, CHANNEL_RD)) {
    LL_DMA_DisableIT_TC(DMA1, CHANNEL_RD);
    LL_DMA_ClearFlag_TC4(DMA1);

    while (LL_SPI_TX_FIFO_EMPTY != LL_SPI_GetTxFIFOLevel(SPIx))
      ;
    while (LL_SPI_IsActiveFlag_BSY(SPIx))
      ;
    while (LL_SPI_RX_FIFO_EMPTY != LL_SPI_GetRxFIFOLevel(SPIx))
      ;

    LL_SPI_DisableDMAReq_TX(SPIx);
    LL_SPI_DisableDMAReq_RX(SPIx);

    TC_Flag = true;
  }
}

void test_flash_basic(void) {
  printf("\n=== Flash Basic Test ===\n");

  // 1. Читаем ID флешки
  printf("Reading flash ID...\n");
  CS_Assert();
  SPI_WriteByte(0x9F); // Read ID command
  uint8_t id1 = SPI_WriteByte(0xFF);
  uint8_t id2 = SPI_WriteByte(0xFF);
  uint8_t id3 = SPI_WriteByte(0xFF);
  CS_Release();
  printf("Flash ID: %02X %02X %02X\n", id1, id2, id3);

  // 2. Тест чтения
  printf("Test read...\n");
  uint8_t read_buf[16];
  PY25Q16_ReadBuffer(0x1000, read_buf, 16);
  printf("Read from 0x1000: ");
  for (int i = 0; i < 16; i++)
    printf("%02X ", read_buf[i]);
  printf("\n");

  // 3. Тест записи (маленький)
  printf("Test write 4 bytes...\n");
  uint8_t test_data[] = {0xAA, 0x55, 0xAA, 0x55};

  // Стираем сектор сначала
  PY25Q16_SectorErase(0x1000);

  // Записываем
  PY25Q16_WriteBuffer(0x1000, test_data, 4, false);

  // Читаем обратно
  memset(read_buf, 0, 16);
  PY25Q16_ReadBuffer(0x1000, read_buf, 16);
  printf("Read back: ");
  for (int i = 0; i < 16; i++)
    printf("%02X ", read_buf[i]);
  printf("\n");

  printf("=== Test Complete ===\n\n");
}
