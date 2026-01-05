#include "settings_ini.h"
#include "driver/fat.h"
#include "external/printf/printf.h"
#include <stdlib.h>
#include <string.h>

#define INI_BUFFER_SIZE 512
#define LINE_BUFFER_SIZE 64

static char ini_buffer[INI_BUFFER_SIZE];
static uint32_t ini_size = 0;

// Названия полей для INI файла
static const char *setting_names[SETTING_COUNT] = {
    [SETTING_EEPROMTYPE] = "eeprom_type",
    [SETTING_BATSAVE] = "battery_save",
    [SETTING_VOX] = "vox",
    [SETTING_BACKLIGHT] = "backlight",
    [SETTING_TXTIME] = "tx_time",
    [SETTING_CURRENTSCANLIST] = "current_scanlist",
    [SETTING_ROGER] = "roger",
    [SETTING_SCANMODE] = "scan_mode",
    [SETTING_CHDISPLAYMODE] = "ch_display_mode",
    [SETTING_BEEP] = "beep",
    [SETTING_KEYLOCK] = "key_lock",
    [SETTING_PTT_LOCK] = "ptt_lock",
    [SETTING_BUSYCHANNELTXLOCK] = "busy_channel_tx_lock",
    [SETTING_STE] = "ste",
    [SETTING_REPEATERSTE] = "repeater_ste",
    [SETTING_DTMFDECODE] = "dtmf_decode",
    [SETTING_BRIGHTNESS_H] = "brightness_high",
    [SETTING_BRIGHTNESS_L] = "brightness_low",
    [SETTING_CONTRAST] = "contrast",
    [SETTING_MAINAPP] = "main_app",
    [SETTING_SQOPENEDTIMEOUT] = "sq_opened_timeout",
    [SETTING_SQCLOSEDTIMEOUT] = "sq_closed_timeout",
    [SETTING_SQLOPENTIME] = "sql_open_time",
    [SETTING_SQLCLOSETIME] = "sql_close_time",
    [SETTING_SKIPGARBAGEFREQUENCIES] = "skip_garbage_frequencies",
    [SETTING_ACTIVEVFO] = "active_vfo",
    [SETTING_BACKLIGHTONSQUELCH] = "backlight_on_squelch",
    [SETTING_BATTERYCALIBRATION] = "battery_calibration",
    [SETTING_BATTERYTYPE] = "battery_type",
    [SETTING_BATTERYSTYLE] = "battery_style",
    [SETTING_UPCONVERTER] = "upconverter",
    [SETTING_DEVIATION] = "deviation",
    [SETTING_MIC] = "mic",
    [SETTING_SHOWLEVELINVFO] = "show_level_in_vfo",
    [SETTING_BOUND240_280] = "bound_240_280",
    [SETTING_NOLISTEN] = "no_listen",
    [SETTING_SI4732POWEROFF] = "si4732_power_off",
    [SETTING_TONELOCAL] = "tone_local",
    [SETTING_FCTIME] = "fc_time",
    [SETTING_MULTIWATCH] = "multiwatch",
    [SETTING_FREQ_CORRECTION] = "freq_correction",
};

// Вспомогательные функции для работы с INI
static void append_line(const char *line) {
  size_t len = strlen(line);
  if (ini_size + len + 1 < INI_BUFFER_SIZE) {
    strcpy(ini_buffer + ini_size, line);
    ini_size += len;
    ini_buffer[ini_size++] = '\n';
    ini_buffer[ini_size] = '\0';
  }
}

static void append_setting(const char *name, uint32_t value) {
  char line[LINE_BUFFER_SIZE];
  snprintf(line, LINE_BUFFER_SIZE, "%s=%u", name, value);
  append_line(line);
}

static const char *find_value(const char *name) {
  static char value_buffer[64];
  const char *line = ini_buffer;
  size_t name_len = strlen(name);

  while (*line) {
    // Пропустить пробелы в начале строки
    while (*line == ' ' || *line == '\t')
      line++;

    // Пропустить комментарии и пустые строки
    if (*line == ';' || *line == '#' || *line == '\n' || *line == '\r') {
      while (*line && *line != '\n')
        line++;
      if (*line)
        line++;
      continue;
    }

    // Проверить имя параметра
    if (strncmp(line, name, name_len) == 0 && line[name_len] == '=') {
      line += name_len + 1;

      // Пропустить пробелы после '='
      while (*line == ' ' || *line == '\t')
        line++;

      // Скопировать значение
      int i = 0;
      while (*line && *line != '\n' && *line != '\r' && *line != ';' &&
             *line != '#') {
        if (i < 63) {
          value_buffer[i++] = *line;
        }
        line++;
      }

      // Убрать trailing пробелы
      while (i > 0 &&
             (value_buffer[i - 1] == ' ' || value_buffer[i - 1] == '\t')) {
        i--;
      }

      value_buffer[i] = '\0';
      return value_buffer;
    }

    // Перейти к следующей строке
    while (*line && *line != '\n')
      line++;
    if (*line)
      line++;
  }

  return NULL;
}

// Сохранить настройки в INI файл
int SETTINGS_SaveToINI(const Settings *settings, const char *filename) {
  // Очистить буфер
  memset(ini_buffer, 0, INI_BUFFER_SIZE);
  ini_size = 0;

  // Добавить заголовок
  append_line("; Settings Configuration");
  append_line("; Generated automatically");
  append_line("");

  append_line("[General]");
  append_setting("eeprom_type", settings->eepromType);
  append_setting("battery_save", settings->batsave);
  append_setting("vox", settings->vox);
  append_setting("backlight", settings->backlight);
  append_setting("tx_time", settings->txTime);
  append_line("");

  append_line("[Display]");
  append_setting("contrast", settings->contrast);
  append_setting("brightness_high", settings->brightness);
  append_setting("brightness_low", settings->brightnessLow);
  append_setting("ch_display_mode", settings->chDisplayMode);
  append_setting("show_level_in_vfo", settings->showLevelInVFO);
  append_setting("backlight_on_squelch", settings->backlightOnSquelch);
  append_line("");

  append_line("[Audio]");
  append_setting("beep", settings->beep);
  append_setting("roger", settings->roger);
  append_setting("mic", settings->mic);
  append_setting("deviation", settings->deviation);
  append_setting("tone_local", settings->toneLocal);
  append_line("");

  append_line("[Scanning]");
  append_setting("current_scanlist", settings->currentScanlist);
  append_setting("scan_mode", settings->scanmode);
  append_setting("sq_opened_timeout", settings->sqOpenedTimeout);
  append_setting("sq_closed_timeout", settings->sqClosedTimeout);
  append_setting("sql_open_time", settings->sqlOpenTime);
  append_setting("sql_close_time", settings->sqlCloseTime);
  append_setting("multiwatch", settings->mWatch);
  append_line("");

  append_line("[Security]");
  append_setting("key_lock", settings->keylock);
  append_setting("ptt_lock", settings->pttLock);
  append_setting("busy_channel_tx_lock", settings->busyChannelTxLock);
  append_line("");

  append_line("[Features]");
  append_setting("ste", settings->ste);
  append_setting("repeater_ste", settings->repeaterSte);
  append_setting("dtmf_decode", settings->dtmfdecode);
  append_setting("main_app", settings->mainApp);
  append_setting("skip_garbage_frequencies", settings->skipGarbageFrequencies);
  append_setting("active_vfo", settings->activeVFO);
  append_setting("no_listen", settings->noListen);
  append_setting("si4732_power_off", settings->si4732PowerOff);
  append_setting("fc_time", settings->fcTime);
  append_line("");

  append_line("[Hardware]");
  append_setting("battery_type", settings->batteryType);
  append_setting("battery_style", settings->batteryStyle);
  append_setting("battery_calibration", settings->batteryCalibration);
  append_setting("upconverter", settings->upconverter);
  append_setting("bound_240_280", settings->bound_240_280);
  append_setting("freq_correction", settings->freqCorrection);
  append_line("");

  // Записать в файл
  return usb_fs_write_file(filename, (uint8_t *)ini_buffer, ini_size, false);
}

// Загрузить настройки из INI файла
int SETTINGS_LoadFromINI(Settings *settings, const char *filename) {
  ini_size = INI_BUFFER_SIZE;

  // Пробуем прочитать как есть
  if (usb_fs_read_file(filename, (uint8_t *)ini_buffer, &ini_size) != 0) {
    // Пробуем с отформатированным именем
    char fat_name[12];
    fat_format_name(filename, fat_name);
    ini_size = INI_BUFFER_SIZE;
    if (usb_fs_read_file(fat_name, (uint8_t *)ini_buffer, &ini_size) != 0) {
      return -1; // Файл не найден
    }
  }

  ini_buffer[ini_size] = '\0';

  // Парсить значения
  const char *val;

#define READ_VALUE(field, name)                                                \
  val = find_value(name);                                                      \
  if (val)                                                                     \
    settings->field = atoi(val);

  READ_VALUE(eepromType, "eeprom_type");
  READ_VALUE(batsave, "battery_save");
  READ_VALUE(vox, "vox");
  READ_VALUE(backlight, "backlight");
  READ_VALUE(txTime, "tx_time");
  READ_VALUE(contrast, "contrast");
  READ_VALUE(brightness, "brightness_high");
  READ_VALUE(brightnessLow, "brightness_low");
  READ_VALUE(chDisplayMode, "ch_display_mode");
  READ_VALUE(showLevelInVFO, "show_level_in_vfo");
  READ_VALUE(backlightOnSquelch, "backlight_on_squelch");
  READ_VALUE(beep, "beep");
  READ_VALUE(roger, "roger");
  READ_VALUE(mic, "mic");
  READ_VALUE(deviation, "deviation");
  READ_VALUE(toneLocal, "tone_local");
  READ_VALUE(currentScanlist, "current_scanlist");
  READ_VALUE(scanmode, "scan_mode");
  READ_VALUE(sqOpenedTimeout, "sq_opened_timeout");
  READ_VALUE(sqClosedTimeout, "sq_closed_timeout");
  READ_VALUE(sqlOpenTime, "sql_open_time");
  READ_VALUE(sqlCloseTime, "sql_close_time");
  READ_VALUE(mWatch, "multiwatch");
  READ_VALUE(keylock, "key_lock");
  READ_VALUE(pttLock, "ptt_lock");
  READ_VALUE(busyChannelTxLock, "busy_channel_tx_lock");
  READ_VALUE(ste, "ste");
  READ_VALUE(repeaterSte, "repeater_ste");
  READ_VALUE(dtmfdecode, "dtmf_decode");
  READ_VALUE(mainApp, "main_app");
  READ_VALUE(skipGarbageFrequencies, "skip_garbage_frequencies");
  READ_VALUE(activeVFO, "active_vfo");
  READ_VALUE(noListen, "no_listen");
  READ_VALUE(si4732PowerOff, "si4732_power_off");
  READ_VALUE(fcTime, "fc_time");
  READ_VALUE(batteryType, "battery_type");
  READ_VALUE(batteryStyle, "battery_style");
  READ_VALUE(batteryCalibration, "battery_calibration");
  READ_VALUE(upconverter, "upconverter");
  READ_VALUE(bound_240_280, "bound_240_280");
  READ_VALUE(freqCorrection, "freq_correction");

#undef READ_VALUE

  return 0;
}

// Экспортировать текущие настройки
int SETTINGS_Export(const char *filename) {
  printf("================ %s START ==============\n", filename);
  return SETTINGS_SaveToINI(&gSettings, filename);
  printf("================ %s END   ==============\n", filename);
}

// Импортировать настройки
int SETTINGS_Import(const char *filename) {
  int result = SETTINGS_LoadFromINI(&gSettings, filename);
  if (result == 0) {
    SETTINGS_Save(); // Сохранить в EEPROM
  }
  return result;
}
