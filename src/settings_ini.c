#include "settings_ini.h"
#include "driver/fat.h"
#include "driver/uart.h"
#include "external/printf/printf.h"
#include <stdlib.h>
#include <string.h>

#define LINE_BUFFER_SIZE 64
#define NAME_BUFFER_SIZE 32

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
    [SETTING_INVERT_BUTTONS] = "invert_buttons",
};

// Добавьте эту статическую переменную в начало файла (вне функций)
static bool pending_lf = false; // флаг: предыдущий байт был \r

// НОВАЯ надёжная функция чтения строки
static int read_ini_line(usb_fs_handle_t *handle, char *line_buf,
                         size_t buf_size) {
  size_t pos = 0;

  // Если предыдущая строка закончилась на \r, пропускаем следующий \n
  if (pending_lf) {
    pending_lf = false;
    uint8_t byte;
    if (usb_fs_read_bytes(handle, &byte, 1) == 0) {
      // EOF сразу после \r — считаем строку завершённой
      if (pos == 0)
        return 0;
      goto finalize;
    }
    if (byte != '\n') {
      // Это был одиночный \r — добавляем его как обычный символ
      if (pos < buf_size - 1) {
        line_buf[pos++] = '\r';
      }
    }
    // иначе \n после \r — просто пропустили
  }

  while (1) {
    uint8_t byte;
    size_t read = usb_fs_read_bytes(handle, &byte, 1);
    if (read == 0) {
      // EOF
      if (pos > 0)
        goto finalize;
      return 0;
    }

    if (byte == '\r') {
      pending_lf = true; // ждём возможный \n в следующей итерации
      goto finalize;
    }

    if (byte == '\n') {
      // Одиночный \n (Unix-style) — конец строки
      goto finalize;
    }

    // Обычный символ
    if (pos < buf_size - 1) {
      line_buf[pos++] = (char)byte;
    }
    // если буфер полон — игнорируем остаток до конца строки
  }

finalize:
  line_buf[pos] = '\0';
  return (pos > 0 || !pending_lf); // возвращаем 1, если есть данные
}

// Запись строки в файл
static int write_line(const char *filename, const char *line, bool append) {
  return usb_fs_write_file(filename, (const uint8_t *)line, strlen(line),
                           append);
}

static bool parse_ini_line(const char *line, char *name_buf, uint32_t *value) {
  const char *p = line;
  while (*p == ' ' || *p == '\t')
    p++;

  if (*p == ';' || *p == '#' || *p == '\0')
    return false;

  const char *eq = strchr(p, '=');
  if (!eq)
    return false;

  // Имя
  size_t name_len = eq - p;
  while (name_len > 0 && (p[name_len - 1] == ' ' || p[name_len - 1] == '\t'))
    name_len--;
  if (name_len == 0 || name_len >= NAME_BUFFER_SIZE)
    return false;
  strncpy(name_buf, p, name_len);
  name_buf[name_len] = '\0';

  // Значение — ищем первый пробел или ; для комментария
  const char *val_start = eq + 1;
  while (*val_start == ' ' || *val_start == '\t')
    val_start++;

  // Обрезаем по комментарию
  const char *comment = strchr(val_start, ';');
  if (comment) {
    // Копируем только до комментария
    char temp[32];
    size_t len = comment - val_start;
    if (len >= sizeof(temp))
      len = sizeof(temp) - 1;
    strncpy(temp, val_start, len);
    temp[len] = '\0';
    *value = atoi(temp);
  } else {
    *value = atoi(val_start);
  }

  return true;
}

// SETTINGS_SaveToINI — построчно, без буфера
int SETTINGS_SaveToINI(const Settings *settings, const char *filename) {
  char line[LINE_BUFFER_SIZE];

  // Заголовок
  write_line(filename, "; Settings Configuration\n", false);
  write_line(filename, "; Generated automatically\n\n", true);

  // [General]
  write_line(filename, "[General]\n", true);
  snprintf(line, sizeof(line), "eeprom_type=%u\n", settings->eepromType);
  write_line(filename, line, true);
  snprintf(line, sizeof(line), "battery_save=%u\n", settings->batsave);
  write_line(filename, line, true);
  snprintf(line, sizeof(line), "vox=%u\n", settings->vox);
  write_line(filename, line, true);
  snprintf(line, sizeof(line), "backlight=%u\n", settings->backlight);
  write_line(filename, line, true);
  snprintf(line, sizeof(line), "tx_time=%u\n", settings->txTime);
  write_line(filename, line, true);
  write_line(filename, "\n[Display]\n", true);

  // [Display]
  snprintf(line, sizeof(line), "contrast=%u\n", settings->contrast);
  write_line(filename, line, true);
  snprintf(line, sizeof(line), "brightness_high=%u\n", settings->brightness);
  write_line(filename, line, true);
  snprintf(line, sizeof(line), "brightness_low=%u\n", settings->brightnessLow);
  write_line(filename, line, true);
  snprintf(line, sizeof(line), "ch_display_mode=%u\n", settings->chDisplayMode);
  write_line(filename, line, true);
  snprintf(line, sizeof(line), "show_level_in_vfo=%u\n",
           settings->showLevelInVFO);
  write_line(filename, line, true);
  snprintf(line, sizeof(line), "backlight_on_squelch=%u\n",
           settings->backlightOnSquelch);
  write_line(filename, line, true);
  write_line(filename, "\n[Audio]\n", true);

  // [Audio]
  snprintf(line, sizeof(line), "beep=%u\n", settings->beep);
  write_line(filename, line, true);
  snprintf(line, sizeof(line), "roger=%u\n", settings->roger);
  write_line(filename, line, true);
  snprintf(line, sizeof(line), "mic=%u\n", settings->mic);
  write_line(filename, line, true);
  snprintf(line, sizeof(line), "deviation=%u\n", settings->deviation);
  write_line(filename, line, true);
  snprintf(line, sizeof(line), "tone_local=%u\n", settings->toneLocal);
  write_line(filename, line, true);
  write_line(filename, "\n[Scanning]\n", true);

  // [Scanning]
  snprintf(line, sizeof(line), "current_scanlist=%u\n",
           settings->currentScanlist);
  write_line(filename, line, true);
  snprintf(line, sizeof(line), "scan_mode=%u\n", settings->scanmode);
  write_line(filename, line, true);
  snprintf(line, sizeof(line), "sq_opened_timeout=%u\n",
           settings->sqOpenedTimeout);
  write_line(filename, line, true);
  snprintf(line, sizeof(line), "sq_closed_timeout=%u\n",
           settings->sqClosedTimeout);
  write_line(filename, line, true);
  snprintf(line, sizeof(line), "sql_open_time=%u\n", settings->sqlOpenTime);
  write_line(filename, line, true);
  snprintf(line, sizeof(line), "sql_close_time=%u\n", settings->sqlCloseTime);
  write_line(filename, line, true);
  snprintf(line, sizeof(line), "multiwatch=%u\n", settings->mWatch);
  write_line(filename, line, true);
  write_line(filename, "\n[Security]\n", true);

  // [Security]
  snprintf(line, sizeof(line), "key_lock=%u\n", settings->keylock);
  write_line(filename, line, true);
  snprintf(line, sizeof(line), "ptt_lock=%u\n", settings->pttLock);
  write_line(filename, line, true);
  snprintf(line, sizeof(line), "busy_channel_tx_lock=%u\n",
           settings->busyChannelTxLock);
  write_line(filename, line, true);
  write_line(filename, "\n[Features]\n", true);

  // [Features]
  snprintf(line, sizeof(line), "ste=%u\n", settings->ste);
  write_line(filename, line, true);
  snprintf(line, sizeof(line), "repeater_ste=%u\n", settings->repeaterSte);
  write_line(filename, line, true);
  snprintf(line, sizeof(line), "dtmf_decode=%u\n", settings->dtmfdecode);
  write_line(filename, line, true);
  snprintf(line, sizeof(line), "main_app=%u\n", settings->mainApp);
  write_line(filename, line, true);
  snprintf(line, sizeof(line), "skip_garbage_frequencies=%u\n",
           settings->skipGarbageFrequencies);
  write_line(filename, line, true);
  snprintf(line, sizeof(line), "active_vfo=%u\n", settings->activeVFO);
  write_line(filename, line, true);
  snprintf(line, sizeof(line), "no_listen=%u\n", settings->noListen);
  write_line(filename, line, true);
  snprintf(line, sizeof(line), "si4732_power_off=%u\n",
           settings->si4732PowerOff);
  write_line(filename, line, true);
  snprintf(line, sizeof(line), "fc_time=%u\n", settings->fcTime);
  write_line(filename, line, true);
  write_line(filename, "\n[Hardware]\n", true);

  // [Hardware]
  snprintf(line, sizeof(line), "battery_type=%u\n", settings->batteryType);
  write_line(filename, line, true);
  snprintf(line, sizeof(line), "battery_style=%u\n", settings->batteryStyle);
  write_line(filename, line, true);
  snprintf(line, sizeof(line), "battery_calibration=%u\n",
           settings->batteryCalibration);
  write_line(filename, line, true);
  snprintf(line, sizeof(line), "upconverter=%u\n", settings->upconverter);
  write_line(filename, line, true);
  snprintf(line, sizeof(line), "bound_240_280=%u\n", settings->bound_240_280);
  write_line(filename, line, true);
  snprintf(line, sizeof(line), "freq_correction=%u\n",
           settings->freqCorrection);
  write_line(filename, line, true);
  snprintf(line, sizeof(line), "invert_buttons=%u\n", settings->invertButtons);
  write_line(filename, line, true);

  return 0;
}

// Загрузить настройки из INI файла
int SETTINGS_LoadFromINI(Settings *settings, const char *filename) {
  usb_fs_handle_t handle;

  // Попытка открыть файл
  if (usb_fs_open(filename, &handle) != 0) {
    return -1; // Файл не найден
  }

  char line_buf[LINE_BUFFER_SIZE];
  char name_buf[NAME_BUFFER_SIZE];
  size_t line_pos;

  // Сброс настроек по умолчанию
  memset(settings, 0, sizeof(*settings));

  while (read_ini_line(&handle, line_buf, sizeof(line_buf))) {
    LogC(LOG_C_BRIGHT_CYAN, "[INI] line: %s", line_buf);

    uint32_t value;
    if (!parse_ini_line(line_buf, name_buf, &value)) {
      LogC(LOG_C_BRIGHT_CYAN, "[INI] cannot parse line");
      continue;
    }

    // Применение значений
    if (strcmp(name_buf, "eeprom_type") == 0)
      settings->eepromType = value;
    else if (strcmp(name_buf, "battery_save") == 0)
      settings->batsave = value;
    else if (strcmp(name_buf, "vox") == 0)
      settings->vox = value;
    else if (strcmp(name_buf, "backlight") == 0)
      settings->backlight = value;
    else if (strcmp(name_buf, "tx_time") == 0)
      settings->txTime = value;
    else if (strcmp(name_buf, "contrast") == 0)
      settings->contrast = value;
    else if (strcmp(name_buf, "brightness_high") == 0)
      settings->brightness = value;
    else if (strcmp(name_buf, "brightness_low") == 0)
      settings->brightnessLow = value;
    else if (strcmp(name_buf, "ch_display_mode") == 0)
      settings->chDisplayMode = value;
    else if (strcmp(name_buf, "show_level_in_vfo") == 0)
      settings->showLevelInVFO = value;
    else if (strcmp(name_buf, "backlight_on_squelch") == 0)
      settings->backlightOnSquelch = value;
    else if (strcmp(name_buf, "beep") == 0)
      settings->beep = value;
    else if (strcmp(name_buf, "roger") == 0)
      settings->roger = value;
    else if (strcmp(name_buf, "mic") == 0)
      settings->mic = value;
    else if (strcmp(name_buf, "deviation") == 0)
      settings->deviation = value;
    else if (strcmp(name_buf, "tone_local") == 0)
      settings->toneLocal = value;
    else if (strcmp(name_buf, "current_scanlist") == 0)
      settings->currentScanlist = value;
    else if (strcmp(name_buf, "scan_mode") == 0)
      settings->scanmode = value;
    else if (strcmp(name_buf, "sq_opened_timeout") == 0)
      settings->sqOpenedTimeout = value;
    else if (strcmp(name_buf, "sq_closed_timeout") == 0)
      settings->sqClosedTimeout = value;
    else if (strcmp(name_buf, "sql_open_time") == 0)
      settings->sqlOpenTime = value;
    else if (strcmp(name_buf, "sql_close_time") == 0)
      settings->sqlCloseTime = value;
    else if (strcmp(name_buf, "multiwatch") == 0)
      settings->mWatch = value;
    else if (strcmp(name_buf, "key_lock") == 0)
      settings->keylock = value;
    else if (strcmp(name_buf, "ptt_lock") == 0)
      settings->pttLock = value;
    else if (strcmp(name_buf, "busy_channel_tx_lock") == 0)
      settings->busyChannelTxLock = value;
    else if (strcmp(name_buf, "ste") == 0)
      settings->ste = value;
    else if (strcmp(name_buf, "repeater_ste") == 0)
      settings->repeaterSte = value;
    else if (strcmp(name_buf, "dtmf_decode") == 0)
      settings->dtmfdecode = value;
    else if (strcmp(name_buf, "main_app") == 0)
      settings->mainApp = value;
    else if (strcmp(name_buf, "skip_garbage_frequencies") == 0)
      settings->skipGarbageFrequencies = value;
    else if (strcmp(name_buf, "active_vfo") == 0)
      settings->activeVFO = value;
    else if (strcmp(name_buf, "no_listen") == 0)
      settings->noListen = value;
    else if (strcmp(name_buf, "si4732_power_off") == 0)
      settings->si4732PowerOff = value;
    else if (strcmp(name_buf, "fc_time") == 0)
      settings->fcTime = value;
    else if (strcmp(name_buf, "battery_type") == 0)
      settings->batteryType = value;
    else if (strcmp(name_buf, "battery_style") == 0)
      settings->batteryStyle = value;
    else if (strcmp(name_buf, "battery_calibration") == 0)
      settings->batteryCalibration = value;
    else if (strcmp(name_buf, "upconverter") == 0)
      settings->upconverter = value;
    else if (strcmp(name_buf, "bound_240_280") == 0)
      settings->bound_240_280 = value;
    else if (strcmp(name_buf, "freq_correction") == 0)
      settings->freqCorrection = value;
    else if (strcmp(name_buf, "invert_buttons") == 0)
      settings->invertButtons = value;
  }

  usb_fs_close(&handle);
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
