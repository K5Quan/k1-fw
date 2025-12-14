#include "../../../printf/printf.h"
#include "bk4829.h"
#include "fat_fs.h"
#include "py32f071_ll_bus.h"
#include "py32f071_ll_gpio.h"
#include "usbd_core.h"
#include "usbd_msc.h"
#include <string.h>

// Размер блока и количество блоков (2MB / 512 байт)
#define FLASH_BLOCK_COUNT 4096

/*!< endpoint address */
#define MSC_IN_EP 0x81
#define MSC_OUT_EP 0x01

// Используем стандартные VID/PID для тестирования
#define USBD_VID 0x0483 // STMicroelectronics
#define USBD_PID 0x5720 // Mass Storage
#define USBD_MAX_POWER 100
#define USBD_LANGID_STRING 1033

/*!< config descriptor size */
#define USB_CONFIG_SIZE (9 + MSC_DESCRIPTOR_LEN)

#ifdef CONFIG_USB_HS
#define MSC_MAX_MPS 512
#else
#define MSC_MAX_MPS 64
#endif

/*!< global descriptor */
// ПОЛНЫЙ дескриптор - замените весь массив msc_descriptor
static const uint8_t msc_descriptor[] = {
    /********************************************
     * Device Descriptor
     ********************************************/
    0x12,                               // bLength
    0x01,                               // bDescriptorType (Device)
    0x00, 0x02,                         // bcdUSB 2.00
    0x00,                               // bDeviceClass
    0x00,                               // bDeviceSubClass
    0x00,                               // bDeviceProtocol
    0x40,                               // bMaxPacketSize0 (64)
    (USBD_VID & 0xFF), (USBD_VID >> 8), // idVendor
    (USBD_PID & 0xFF), (USBD_PID >> 8), // idProduct
    0x00, 0x01,                         // bcdDevice 1.00
    0x01,                               // iManufacturer
    0x02,                               // iProduct
    0x00, // iSerialNumber = 0 (НЕТ Serial Number!)
    0x01, // bNumConfigurations

    /********************************************
     * Configuration Descriptor
     ********************************************/
    0x09,                 // bLength
    0x02,                 // bDescriptorType (Configuration)
    0x20, 0x00,           // wTotalLength (32 bytes = 9+9+7+7)
    0x01,                 // bNumInterfaces
    0x01,                 // bConfigurationValue
    0x00,                 // iConfiguration
    0x80,                 // bmAttributes (Bus Powered)
    (USBD_MAX_POWER / 2), // bMaxPower (в единицах 2mA)

    /********************************************
     * Interface Descriptor - Mass Storage
     ********************************************/
    0x09, // bLength
    0x04, // bDescriptorType (Interface)
    0x00, // bInterfaceNumber
    0x00, // bAlternateSetting
    0x02, // bNumEndpoints (2: Bulk IN + Bulk OUT)
    0x08, // bInterfaceClass (Mass Storage)
    0x06, // bInterfaceSubClass (SCSI Transparent)
    0x50, // bInterfaceProtocol (Bulk-Only Transport)
    0x00, // iInterface

    /********************************************
     * Endpoint Descriptor - Bulk OUT
     ********************************************/
    0x07,                                     // bLength
    0x05,                                     // bDescriptorType (Endpoint)
    MSC_OUT_EP,                               // bEndpointAddress (OUT, EP1)
    0x02,                                     // bmAttributes (Bulk)
    (MSC_MAX_MPS & 0xFF), (MSC_MAX_MPS >> 8), // wMaxPacketSize
    0x00,                                     // bInterval (ignored for Bulk)

    /********************************************
     * Endpoint Descriptor - Bulk IN
     ********************************************/
    0x07,                                     // bLength
    0x05,                                     // bDescriptorType (Endpoint)
    MSC_IN_EP,                                // bEndpointAddress (IN, EP1)
    0x02,                                     // bmAttributes (Bulk)
    (MSC_MAX_MPS & 0xFF), (MSC_MAX_MPS >> 8), // wMaxPacketSize
    0x00,                                     // bInterval (ignored for Bulk)

    /********************************************
     * String Descriptor 0 - Language ID
     ********************************************/
    0x04,                        // bLength
    0x03,                        // bDescriptorType (String)
    (USBD_LANGID_STRING & 0xFF), // wLANGID[0] (0x0409 = English US)
    (USBD_LANGID_STRING >> 8),

    /********************************************
     * String Descriptor 1 - Manufacturer
     ********************************************/
    0x0A, // bLength (10 bytes)
    0x03, // bDescriptorType (String)
    'P', 0x00, 'U', 0x00, 'Y', 0x00, 'A', 0x00,

    /********************************************
     * String Descriptor 2 - Product
     ********************************************/
    0x1C, // bLength (28 bytes)
    0x03, // bDescriptorType (String)
    'P', 0x00, 'Y', 0x00, '3', 0x00, '2', 0x00, ' ', 0x00, 'F', 0x00, 'l', 0x00,
    'a', 0x00, 's', 0x00, 'h', 0x00, ' ', 0x00, 'D', 0x00, 'i', 0x00, 's', 0x00,
    'k', 0x00,

    /********************************************
     * String Descriptor 3 - Serial Number
     ********************************************/
    0x16, // bLength (22 bytes)
    0x03, // bDescriptorType (String)
    '2', 0x00, '0', 0x00, '2', 0x00, '4', 0x00, '1', 0x00, '2', 0x00, '3', 0x00,
    '4', 0x00, '5', 0x00, '6', 0x00,

    // Терминатор
    0x00};

struct usbd_interface intf0;

// Колбэк вызывается после успешной конфигурации USB устройства
void usbd_configure_done_callback(void) {
  // printf("usbd_configure_done_callback called!\n");
}

// Опциональные колбэки (могут понадобиться в некоторых версиях CherryUSB)
__attribute__((weak)) void usbd_event_handler(uint8_t event) {
  // Отладка: можно вывести event через UART
  // printf("USB Event: %d\r\n", event);

  switch (event) {
  case 0: // USBD_EVENT_RESET
    // Устройство сброшено хостом
    break;
  case 1: // USBD_EVENT_CONNECTED
    // Устройство подключено
    break;
  case 2: // USBD_EVENT_DISCONNECTED
    // Устройство отключено
    break;
  }
}

__attribute__((weak)) bool usbd_msc_get_write_protection(uint8_t lun) {
  return false; // Нет защиты от записи
}

__attribute__((weak)) void usbd_msc_notify_ready(uint8_t lun) {
  // Уведомление о готовности устройства
}

// Счётчики для отладки
static volatile uint32_t read_count = 0;
static volatile uint32_t write_count = 0;
static volatile uint32_t get_cap_count = 0;

// Колбэки для CherryUSB MSC - вызываются библиотекой при SCSI командах
// Версия CherryUSB с LUN, но без busid

void usbd_msc_get_cap(uint8_t lun, uint32_t *block_num, uint16_t *block_size) {
  get_cap_count++;
  printf("GET_CAP lun=%u count=%lu\n", lun, get_cap_count);

  *block_num = FLASH_BLOCK_COUNT; // ИСПРАВЛЕНО: total blocks, не -1
  *block_size = 512;

  printf("  -> blocks=%lu size=%u\n", *block_num, *block_size);
}

int usbd_msc_sector_read(uint32_t sector, uint8_t *buffer, uint32_t length) {
  read_count++;
  printf(">>> READ sector=%lu len=%lu count=%lu\n", sector, length, read_count);

  // УБЕРИТЕ LED мигание - оно блокирует!
  // if (read_count == 1) { ... }

  if (length == 0) {
    printf(">>> READ done (zero)\n");
    return 0;
  }

  if (sector >= FLASH_BLOCK_COUNT) {
    printf(">>> READ error: sector out of range\n");
    return -1;
  }

  uint32_t block_count = length / 512;
  if (sector + block_count > FLASH_BLOCK_COUNT) {
    printf(">>> READ error: exceeds size\n");
    return -1;
  }

  // ВРЕМЕННО: верните фиксированные данные БЕЗ чтения flash
  memset(buffer, 0x00, length);

  // Закомментируйте чтение из flash для теста
  /*
  for (uint32_t i = 0; i < block_count; i++) {
      FAT_ReadBlock(sector + i, buffer + i * 512);
  }
  */

  printf(">>> READ done OK\n");
  return 0;
}

int usbd_msc_sector_write(uint32_t sector, uint8_t *buffer, uint32_t length) {
  if (length == 0) {
    return 0;
  }

  // Проверка границ
  if (sector >= FLASH_BLOCK_COUNT) {
    return -1; // Ошибка: вне диапазона
  }

  uint32_t block_count = length / 512;

  // Проверка что не выходим за границы
  if (sector + block_count > FLASH_BLOCK_COUNT) {
    return -1;
  }

  for (uint32_t i = 0; i < block_count; i++) {
    FAT_WriteBlock(sector + i, buffer + i * 512);
  }

  return 0;
}

// Инициализация MSC устройства
void msc_init(void) {

  printf("Device Descriptor (first 18 bytes):\n");
  for (int i = 0; i < 18; i++) {
    printf("%02X ", msc_descriptor[i]);
    if ((i + 1) % 8 == 0)
      printf("\n");
  }
  printf("\n");
  printf("Byte 14 (iManufacturer): %02X\n", msc_descriptor[14]);
  printf("Byte 15 (iProduct): %02X\n", msc_descriptor[15]);
  printf("Byte 16 (iSerialNumber): %02X\n", msc_descriptor[16]);

  printf("Checking CherryUSB version...\n");
#ifdef CHERRYUSB_VERSION
  printf("CherryUSB version: %s\n", CHERRYUSB_VERSION);
#else
  printf("CherryUSB version: UNKNOWN (old version?)\n");
#endif

// Проверка что MSC класс скомпилирован правильно
#ifdef CONFIG_USBDEV_MSC
  printf("CONFIG_USBDEV_MSC is defined\n");
#else
  printf("ERROR: CONFIG_USBDEV_MSC is NOT defined!\n");
#endif

  // GPIO уже настроены в main.c, здесь только для надёжности
  LL_GPIO_InitTypeDef GPIO_InitStruct = {0};
  GPIO_InitStruct.Pin = LL_GPIO_PIN_11 | LL_GPIO_PIN_12;
  GPIO_InitStruct.Mode = LL_GPIO_MODE_ALTERNATE;
  GPIO_InitStruct.Speed = LL_GPIO_SPEED_FREQ_HIGH;
  GPIO_InitStruct.OutputType = LL_GPIO_OUTPUT_PUSHPULL;
  GPIO_InitStruct.Pull = LL_GPIO_PULL_NO;
  GPIO_InitStruct.Alternate = LL_GPIO_AF_2;
  LL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  // ВРЕМЕННО: НЕ инициализируем FAT для теста
  // FAT_Init();

  // Регистрация дескрипторов
  usbd_desc_register(msc_descriptor);

  // Инициализация MSC интерфейса
  usbd_add_interface(usbd_msc_init_intf(&intf0, MSC_OUT_EP, MSC_IN_EP));

  // Инициализация USB стека (прерывания уже включены в main)
  usbd_initialize();
}
