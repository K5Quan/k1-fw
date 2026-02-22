#ifndef STORAGE_H
#define STORAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Initialize storage file for fixed-size items
 * @param name File name
 * @param item_size Size of one item in bytes
 * @param max_items Maximum number of items
 * @return true if successful
 */
bool Storage_Init(const char *name, size_t item_size, uint16_t max_items);

/**
 * Load item from storage
 * @param name File name
 * @param num Item index (0-based)
 * @param item Pointer to item buffer
 * @param item_size Size of item in bytes
 * @return true if successful
 */
bool Storage_Load(const char *name, uint16_t num, void *item, size_t item_size);

/**
 * Save item to storage
 * @param name File name
 * @param num Item index (0-based)
 * @param item Pointer to item data
 * @param item_size Size of item in bytes
 * @return true if successful
 */
bool Storage_Save(const char *name, uint16_t num, const void *item,
                  size_t item_size);
bool Storage_Exists(const char *name);

bool Storage_LoadMultiple(const char *name, uint16_t start_num, void *items,
                          size_t item_size, uint16_t count);
bool Storage_SaveMultiple(const char *name, uint16_t start_num,
                          const void *items, size_t item_size, uint16_t count);

/**
 * Type-safe macros for convenience
 */
#define STORAGE_INIT(name, type, max_items)                                    \
  Storage_Init(name, sizeof(type), max_items)

#define STORAGE_LOAD(name, num, item_ptr)                                      \
  Storage_Load(name, num, item_ptr, sizeof(*(item_ptr)))

#define STORAGE_SAVE(name, num, item_ptr)                                      \
  Storage_Save(name, num, item_ptr, sizeof(*(item_ptr)))

#endif // STORAGE_H
