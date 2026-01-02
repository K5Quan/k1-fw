#ifndef SETTINGS_INI_H
#define SETTINGS_INI_H

#include "settings.h"

// Сохранить настройки в INI файл на флешке
// filename: например "config.ini" (будет преобразовано в "CONFIG  INI")
// Возвращает 0 при успехе, -1 при ошибке
int SETTINGS_SaveToINI(const Settings* settings, const char* filename);

// Загрузить настройки из INI файла
// filename: например "config.ini"
// Возвращает 0 при успехе, -1 если файл не найден
int SETTINGS_LoadFromINI(Settings* settings, const char* filename);

// Экспортировать текущие настройки (из gSettings)
int SETTINGS_Export(const char* filename);

// Импортировать настройки и сохранить в EEPROM
int SETTINGS_Import(const char* filename);

#endif // SETTINGS_INI_H
