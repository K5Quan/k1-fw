#include "files.h"
#include "../driver/lfs.h"
#include "../driver/uart.h"
#include "../helper/menu.h"
#include "../helper/screenshot.h"
#include "../ui/components.h"
#include "../ui/graphics.h"
#include "../ui/statusline.h"
#include "apps.h"
#include "files.h"
#include <string.h>

// Максимальное количество отображаемых файлов
#define MAX_FILES 32
#define MAX_PATH_LEN 64
#define MAX_NAME_LEN 16

// Типы элементов файловой системы
typedef enum {
  FILE_TYPE_FILE = 0,
  FILE_TYPE_FOLDER,
  FILE_TYPE_VFO,
  FILE_TYPE_BAND,
  FILE_TYPE_CH,
  FILE_TYPE_SET,
  FILE_TYPE_SL,
  FILE_TYPE_BACK, // для кнопки "назад"
} FileType;

// Структура для элемента файловой системы
typedef struct {
  char name[MAX_NAME_LEN]; // Имя файла/папки
  FileType type;           // Тип
  uint32_t size;           // Размер (для файлов)
} FileEntry;

static FileEntry gFilesList[MAX_FILES]; // Список файлов
static uint16_t gFilesCount = 0;        // Количество файлов
static char gCurrentPath[MAX_PATH_LEN]; // Текущий путь
static char gStatusText[32]; // Текст статусной строки

static bool showingScreenshot;
static char screenshotPath[32];

// Символы для отображения типов файлов
static const Symbol fileTypeIcons[] = {
    [FILE_TYPE_FILE] = SYM_FILE,     //
    [FILE_TYPE_FOLDER] = SYM_FOLDER, //
    [FILE_TYPE_BACK] = SYM_MISC2,    //
    [FILE_TYPE_VFO] = SYM_VFO,       //
    [FILE_TYPE_BAND] = SYM_BAND,     //
    [FILE_TYPE_CH] = SYM_CH,         //
    [FILE_TYPE_SET] = SYM_SETTING,   //
    [FILE_TYPE_SL] = SYM_SCAN,       //
};

// Прототипы функций
static void loadDirectory(const char *path);
static void renderItem(uint16_t index, uint8_t i);
static bool action(const uint16_t index, KEY_Code_t key, Key_State_t state);
static void navigateTo(const char *name);
static void formatSize(uint32_t size, char *buffer, uint8_t bufferSize);

// Меню для файлов
static Menu filesMenu = {.render_item = renderItem,
                         .itemHeight = MENU_ITEM_H,
                         .action = action,
                         .num_items = 0};

// Форматирование размера файла
static void formatSize(uint32_t size, char *buffer, uint8_t bufferSize) {
  if (size < 1024) {
    snprintf(buffer, bufferSize, "%u B", size);
  } else if (size < 1024 * 1024) {
    snprintf(buffer, bufferSize, "%u KB", size / 1024);
  } else {
    snprintf(buffer, bufferSize, "%u MB", size / (1024 * 1024));
  }
}

// Удаление элемента
static void deleteItem(const char *name, FileType type) {
  char fullPath[MAX_PATH_LEN];

  // Формируем полный путь
  if (strcmp(gCurrentPath, "/") == 0) {
    snprintf(fullPath, sizeof(fullPath), "/%s", name);
  } else {
    snprintf(fullPath, sizeof(fullPath), "%s/%s", gCurrentPath, name);
  }

  // Удаляем
  int err;
  if (type == FILE_TYPE_FOLDER) {
    err = lfs_remove(&gLfs, fullPath); // Внимание: папка должна быть пустой!
  } else {
    err = lfs_remove(&gLfs, fullPath);
  }

  if (err < 0) {
    char msg[32];
    snprintf(msg, sizeof(msg), "Delete error: %d", err);
    STATUSLINE_SetText(msg);
  } else {
    STATUSLINE_SetText("Deleted");
    loadDirectory(gCurrentPath); // Обновляем список
  }
}

static const char *getFileExtension(const char *filename) {
  // Ищем последнюю точку в имени
  const char *dot = strrchr(filename, '.');

  // Если точка не найдена или это первая точка (скрытые файлы .name)
  if (!dot || dot == filename) {
    return ""; // Нет расширения
  }

  return dot + 1; // Возвращаем часть после точки
}

// Загрузка списка файлов из каталога
static void loadDirectory(const char *path) {
  lfs_dir_t dir;
  struct lfs_info info;
  gFilesCount = 0;

  // Открываем директорию
  int err = lfs_dir_open(&gLfs, &dir, path);
  if (err < 0) {
    // Если не удалось открыть, создаем корневую директорию
    if (strcmp(path, "/") == 0) {
      // В корне показываем только ".." если не пусто
      strcpy(gFilesList[0].name, "..");
      gFilesList[0].type = FILE_TYPE_BACK;
      gFilesList[0].size = 0;
      gFilesCount = 1;
    }
    strncpy(gCurrentPath, path, sizeof(gCurrentPath));
    return;
  }

  // Добавляем кнопку "назад" если не в корне
  if (strcmp(path, "/") != 0) {
    strcpy(gFilesList[gFilesCount].name, "..");
    gFilesList[gFilesCount].type = FILE_TYPE_BACK;
    gFilesList[gFilesCount].size = 0;
    gFilesCount++;
  }

  // Читаем содержимое директории
  while (lfs_dir_read(&gLfs, &dir, &info) == 1) {
    if (gFilesCount >= MAX_FILES) {
      break; // Превышен лимит файлов
    }

    // Пропускаем "." и ".."
    if (strcmp(info.name, ".") == 0 || strcmp(info.name, "..") == 0) {
      continue;
    }

    // Копируем имя (обрезаем если слишком длинное)
    strncpy(gFilesList[gFilesCount].name, info.name, MAX_NAME_LEN - 1);
    gFilesList[gFilesCount].name[MAX_NAME_LEN - 1] = '\0';

    // Определяем тип
    if (info.type == LFS_TYPE_DIR) {
      gFilesList[gFilesCount].type = FILE_TYPE_FOLDER;
      gFilesList[gFilesCount].size = 0;
    } else {

      const char *ext = getFileExtension(gFilesList[gFilesCount].name);
      if (strcmp(ext, "vfo") == 0) {
        gFilesList[gFilesCount].type = FILE_TYPE_VFO;
      } else if (strcmp(ext, "bnd") == 0) {
        gFilesList[gFilesCount].type = FILE_TYPE_BAND;
      } else if (strcmp(ext, "ch") == 0) {
        gFilesList[gFilesCount].type = FILE_TYPE_CH;
      } else if (strcmp(ext, "set") == 0) {
        gFilesList[gFilesCount].type = FILE_TYPE_SET;
      } else if (strcmp(ext, "sl") == 0) {
        gFilesList[gFilesCount].type = FILE_TYPE_SL;
      } else {
        gFilesList[gFilesCount].type = FILE_TYPE_FILE;
      }
      gFilesList[gFilesCount].size = info.size;
    }

    gFilesCount++;
  }

  // Закрываем директорию
  lfs_dir_close(&gLfs, &dir);

  // Сортируем: сначала папки, потом файлы, по алфавиту
  // Простая пузырьковая сортировка
  for (uint16_t i = 0; i < gFilesCount - 1; i++) {
    for (uint16_t j = 0; j < gFilesCount - i - 1; j++) {
      bool swap = false;

      // Папки должны быть перед файлами
      if (gFilesList[j].type != FILE_TYPE_FOLDER &&
          gFilesList[j + 1].type == FILE_TYPE_FOLDER) {
        swap = true;
      }
      // Если оба одинакового типа - сортируем по алфавиту
      else if (gFilesList[j].type == gFilesList[j + 1].type) {
        if (strcmp(gFilesList[j].name, gFilesList[j + 1].name) > 0) {
          swap = true;
        }
      }

      if (swap) {
        FileEntry temp = gFilesList[j];
        gFilesList[j] = gFilesList[j + 1];
        gFilesList[j + 1] = temp;
      }
    }
  }

  // Обновляем меню
  filesMenu.num_items = gFilesCount;
  MENU_Init(&filesMenu);

  // Сохраняем текущий путь
  strncpy(gCurrentPath, path, sizeof(gCurrentPath) - 1);
  gCurrentPath[sizeof(gCurrentPath) - 1] = '\0';

  // Обновляем статус
  snprintf(gStatusText, sizeof(gStatusText), "%u items", gFilesCount);
}

// Переход в директорию или открытие файла
static void navigateTo(const char *name) {
  char newPath[MAX_PATH_LEN];

  if (strcmp(name, "..") == 0) {
    // Назад
    char *lastSlash = strrchr(gCurrentPath, '/');
    if (lastSlash != NULL) {
      if (lastSlash == gCurrentPath) {
        // Мы в корне
        strcpy(newPath, "/");
      } else {
        *lastSlash = '\0';
        strcpy(newPath, gCurrentPath);
        if (strlen(newPath) == 0) {
          strcpy(newPath, "/");
        }
      }
    }
  } else {
    // Формируем новый путь
    if (strcmp(gCurrentPath, "/") == 0) {
      snprintf(newPath, sizeof(newPath), "/%s", name);
    } else {
      snprintf(newPath, sizeof(newPath), "%s/%s", gCurrentPath, name);
    }
  }

  // Проверяем, является ли это директорией
  struct lfs_info info;
  int err = lfs_stat(&gLfs, newPath, &info);

  if (err == 0 && info.type == LFS_TYPE_DIR) {
    // Это директория - загружаем её содержимое
    loadDirectory(newPath);
    STATUSLINE_SetText("%s", gStatusText);
  } else {
    // Это файл - можно показать информацию или открыть
    // Пока просто покажем в статусе
    char sizeStr[16];
    formatSize(info.size, sizeStr, sizeof(sizeStr));
    STATUSLINE_SetText("%s - %s", name, sizeStr);
    const char *ext = getFileExtension(name);
    if (strcmp(ext, "bmp") == 0) {
      showingScreenshot = true;
      sprintf(screenshotPath, "%s/%s", gCurrentPath, name);
    }
  }
}

// Отрисовка одного элемента списка
static void renderItem(uint16_t index, uint8_t i) {
  if (index >= gFilesCount) {
    return;
  }

  FileEntry *entry = &gFilesList[index];
  uint8_t y = MENU_Y + i * MENU_ITEM_H;

  // Отступ слева для иерархии
  uint8_t x_offset = 2;

  // Отображаем иконку типа
  if (fileTypeIcons[entry->type] != 0) {
    PrintSymbolsEx(x_offset, y + 8, POS_L, C_INVERT, "%c",
                   fileTypeIcons[entry->type]);
    x_offset += 13; // Смещаем для текста
  }

  // Отображаем имя
  PrintMediumEx(x_offset, y + 8, POS_L, C_INVERT, "%s", entry->name);

  // Для файлов показываем размер справа
  if (entry->type == FILE_TYPE_FILE) {
    char sizeStr[16];
    formatSize(entry->size, sizeStr, sizeof(sizeStr));
    PrintSmallEx(LCD_WIDTH - 5, y + 7, POS_R, C_INVERT, "%s", sizeStr);
  }
}

// Обработка действий
static bool action(const uint16_t index, KEY_Code_t key, Key_State_t state) {
  if (index >= gFilesCount) {
    return false;
  }

  if (state == KEY_RELEASED) {
    switch (key) {
    case KEY_PTT:  // Enter/выбор
    case KEY_MENU: // Альтернативный выбор
      navigateTo(gFilesList[index].name);
      return true;

    case KEY_5: // Обновить список
      loadDirectory(gCurrentPath);
      STATUSLINE_SetText("%s", gStatusText);
      return true;

    case KEY_1: // Создать новую папку (заглушка)
      STATUSLINE_SetText("Create folder - NYI");
      return true;

    case KEY_0: // Удалить (заглушка)
      deleteItem(gFilesList[index].name, gFilesList[index].type);
      return true;

    case KEY_EXIT:
      if (strcmp(gCurrentPath, "/") == 0) {
        APPS_exit();
        return true;
      }
      navigateTo("..");
      return true;

    default:
      break;
    }
  }

  return false;
}

// Инициализация файлового менеджера
void FILES_init() {
  // Начинаем с корневой директории
  gCurrentPath[0] = '/';
  gCurrentPath[1] = '\0';

  loadDirectory(gCurrentPath);

  if (gFilesCount == 0) {
    STATUSLINE_SetText("Empty or no filesystem");
  } else {
    STATUSLINE_SetText("%s", gStatusText);
  }
}

// Деинициализация
void FILES_deinit() {
  // Очищаем список файлов
  gFilesCount = 0;
  memset(gFilesList, 0, sizeof(gFilesList));
}

// Обработка клавиатуры
bool FILES_key(KEY_Code_t key, Key_State_t state) {
  if (state == KEY_RELEASED) {

    if (showingScreenshot) {
      switch (key) {
      case KEY_EXIT:
        showingScreenshot = false;
        return true;
      default:
        break;
      }
    }

    switch (key) {
    case KEY_STAR: // Выход
      APPS_exit();
      return true;

    case KEY_F: // Информация о файловой системе
    {
      uint32_t freeSpace = fs_get_free_space();
      char freeStr[16];
      formatSize(freeSpace, freeStr, sizeof(freeStr));
      STATUSLINE_SetText("Free: %s", freeStr);
      return true;
    }

    default:
      break;
    }
  }

  // Передаем управление меню
  if (MENU_HandleInput(key, state)) {
    return true;
  }

  return false;
}

// Отрисовка
void FILES_render() {
  if (showingScreenshot) {
    displayScreen(screenshotPath);
    FillRect(0, 0, LCD_WIDTH, 8, C_FILL);
    PrintSmall(1, 5, "Screenshot");
    return;
  }
  MENU_Render();

  // Показываем текущий путь вверху
  PrintMediumEx(2, 2, POS_L, C_FILL, "%s", gCurrentPath);
}
