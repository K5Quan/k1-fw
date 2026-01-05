#include "channels_csv.h"
#include "driver/fat.h"
#include "external/printf/printf.h"
#include <stdlib.h>
#include <string.h>

static void ProcessCSVLine(const char *line, bool skip_headers, CHType type);

// Определим заголовки для каждого типа
static const char *GetCSVHeaders(CHType type) {
  switch (type) {
  case TYPE_CH:
  case TYPE_VFO: // Похожи
    return "num,name,rxF,ppm,txF,offsetDir,allowTx,step,modulation,bw,radio,"
           "power,scrambler,squelch_value,squelch_type,code_rx_value,code_rx_"
           "type,code_tx_value,code_tx_type,fixedBoundsMode,gainIndex,"
           "scanlists\n";
  case TYPE_BAND:
    return "num,name,rxF,ppm,txF,offsetDir,allowTx,step,modulation,bw,radio,"
           "power,scrambler,squelch_value,squelch_type,code_rx_value,code_rx_"
           "type,code_tx_value,code_tx_type,fixedBoundsMode,gainIndex,"
           "scanlists,bank,powCalib_s,powCalib_m,powCalib_e,lastUsedFreq\n";
  default:
    return NULL;
  }
}

// Функция для определения имени файла по типу (как раньше)
static const char *GetFileNameByType(CHType type) {
  switch (type) {
  case TYPE_CH:
    return "channels.csv";
  case TYPE_VFO:
    return "vfos.csv";
  case TYPE_BAND:
    return "bands.csv";
  default:
    return NULL;
  }
}

// Вспомогательная: запись одной структуры CH в CSV строку и в файл
void CHANNELS_WriteCHToFile(int16_t num, const char *filename, bool append,
                            CH *ch) {
  char line[MAX_LINE_LEN * 2]; // Достаточно для всех полей

  sprintf(line,
          "%d,%.*s,%lu,%ld,%lu,%d,%d,%d,%d,%d,%d,%d,%d,%u,%u,%u,%u,%u,%u,%d,%u,"
          "%u\n",
          num, 10, ch->name, (unsigned long)ch->rxF, (long)ch->ppm,
          (unsigned long)ch->txF, ch->offsetDir, ch->allowTx, ch->step,
          ch->modulation, ch->bw, ch->radio, ch->power, ch->scrambler,
          ch->squelch.value, ch->squelch.type, ch->code.rx.value,
          ch->code.rx.type, ch->code.tx.value, ch->code.tx.type,
          ch->fixedBoundsMode, ch->gainIndex, ch->scanlists);

  usb_fs_write_file(filename, (const uint8_t *)line, strlen(line), append);
}

// Аналогично для VFO (идентично CH)
void CHANNELS_WriteVFOToFile(int16_t num, const char *filename, bool append,
                             VFO *vfo) {
  // То же sprintf, поскольку поля одинаковые
  // Используйте vfo-> вместо ch->
  char line[MAX_LINE_LEN * 2];

  sprintf(line,
          "%d,%.*s,%lu,%ld,%lu,%d,%d,%d,%d,%d,%d,%d,%d,%u,%u,%u,%u,%u,%u,%d,%u,"
          "%u\n",
          num, 10, vfo->name, (unsigned long)vfo->rxF, (long)vfo->ppm,
          (unsigned long)vfo->txF, vfo->offsetDir, vfo->allowTx, vfo->step,
          vfo->modulation, vfo->bw, vfo->radio, vfo->power, vfo->scrambler,
          vfo->squelch.value, vfo->squelch.type, vfo->code.rx.value,
          vfo->code.rx.type, vfo->code.tx.value, vfo->code.tx.type,
          vfo->fixedBoundsMode, vfo->gainIndex, vfo->scanlists);

  usb_fs_write_file(filename, (const uint8_t *)line, strlen(line), append);
}

// Для Band: добавить поля
void CHANNELS_WriteBandToFile(int16_t num, const char *filename, bool append,
                              Band *band) {
  char line[MAX_LINE_LEN * 2];

  sprintf(line,
          "%d,%.*s,%lu,%ld,%lu,%d,%d,%d,%d,%d,%d,%d,%d,%u,%u,%u,%u,%u,%u,%d,%u,"
          "%u,%u,%u,%u,%u,%lu\n",
          num, 10, band->name, (unsigned long)band->rxF, (long)band->ppm,
          (unsigned long)band->txF, band->offsetDir, band->allowTx, band->step,
          band->modulation, band->bw, band->radio, band->power, band->scrambler,
          band->squelch.value, band->squelch.type, band->code.rx.value,
          band->code.rx.type, band->code.tx.value, band->code.tx.type,
          band->fixedBoundsMode, // Для Band это может быть неиспользуемым, но
                                 // для совместимости
          band->gainIndex, band->scanlists, band->misc.bank,
          band->misc.powCalib.s, band->misc.powCalib.m, band->misc.powCalib.e,
          (unsigned long)band->misc.lastUsedFreq);

  usb_fs_write_file(filename, (const uint8_t *)line, strlen(line), append);
}

// Экспорт всех в файл (сначала заголовки с append=false, затем строки с
// append=true)
void CHANNELS_ExportToFile(CHType type) {
  const char *filename = GetFileNameByType(type);
  if (!filename)
    return;

  const char *headers = GetCSVHeaders(type);
  usb_fs_write_file(filename, (const uint8_t *)headers, strlen(headers),
                    false); // Перезапись

  uint16_t count = CHANNELS_GetCountMax();
  for (int16_t i = 0; i < count; i++) {
    if (!CHANNELS_Existing(i) || CHANNELS_GetMeta(i).type != type)
      continue;

    switch (type) {
    case TYPE_CH: {
      CH ch;
      CHANNELS_Load(i, (CH *)&ch);
      CHANNELS_WriteCHToFile(i, filename, true, &ch);
      break;
    }
    case TYPE_VFO: {
      VFO vfo;
      CHANNELS_Load(i, (CH *)&vfo);
      CHANNELS_WriteVFOToFile(i, filename, true, &vfo);
      break;
    }
    case TYPE_BAND: {
      Band band;
      CHANNELS_Load(i, (CH *)&band);
      CHANNELS_WriteBandToFile(i, filename, true, &band);
      break;
    }
    default:
      break;
    }
  }
}

// Для импорта: потоковое чтение, пропустить заголовки, затем парсить строки
static void ParseCSVLine(const char *line, char **fields, int max_fields) {
  // Простой парсер: strtok для ','
  char *token;
  char copy[MAX_LINE_LEN * 2];
  strncpy(copy, line, sizeof(copy) - 1);
  copy[sizeof(copy) - 1] = '\0';

  int i = 0;
  token = strtok(copy, ",");
  while (token && i < max_fields) {
    fields[i++] = token;
    token = strtok(NULL, ",");
  }
}

// Вспомогательная: чтение и парсинг CSV
static void ParseCSV(const char *filename, usb_fs_handle_t *handle,
                     CHType type) {
  char line_buf[MAX_LINE_LEN * 2];
  size_t line_pos = 0;
  uint8_t buffer[256]; // Буфер для чтения
  size_t bytes_read = 0;
  size_t buffer_pos = 0;
  bool skip_headers = true; // Пропустить первую строку

  while (1) {
    // Если буфер пуст, читаем следующую порцию данных
    if (buffer_pos >= bytes_read) {
      bytes_read = usb_fs_read_bytes(handle, buffer, sizeof(buffer));
      if (bytes_read == 0) {
        // Обрабатываем последнюю строку, если она не завершена
        if (line_pos > 0) {
          line_buf[line_pos] = '\0';
          ProcessCSVLine(line_buf, skip_headers, type);
        }
        break; // Конец файла
      }
      buffer_pos = 0;
    }

    uint8_t byte = buffer[buffer_pos++];

    if (byte == '\n' || line_pos >= sizeof(line_buf) - 1) {
      line_buf[line_pos] = '\0';
      line_pos = 0;

      if (skip_headers) {
        skip_headers = false;
        continue;
      }

      ProcessCSVLine(line_buf, skip_headers, type);
    } else {
      line_buf[line_pos++] = byte;
    }
  }
}

// Обработка одной CSV строки
static void ProcessCSVLine(const char *line, bool skip_headers, CHType type) {
  if (skip_headers)
    return;

  char *fields[30]; // Достаточно для всех полей
  ParseCSVLine(line, fields, 30);

  int16_t num = atoi(fields[0]);
  // Проверяем тип и заполняем структуру
  union {
    CH ch;
    VFO vfo;
    Band band;
  } entry;
  memset(&entry, 0, sizeof(entry));

  // Общие поля (индексы:
  // 1=name,2=rxF,3=ppm,4=txF,5=offsetDir,6=allowTx,7=step,8=modulation,9=bw,10=radio,11=power,12=scrambler,13=squelch_value,14=squelch_type,15=code_rx_value,16=code_rx_type,17=code_tx_value,18=code_tx_type,19=fixedBoundsMode,20=gainIndex,21=scanlists
  if (fields[1])
    strncpy(entry.ch.name, fields[1], 10);
  if (fields[2])
    entry.ch.rxF = atol(fields[2]);
  if (fields[3])
    entry.ch.ppm = atol(fields[3]);
  if (fields[4])
    entry.ch.txF = atol(fields[4]);
  if (fields[5])
    entry.ch.offsetDir = atoi(fields[5]);
  if (fields[6])
    entry.ch.allowTx = atoi(fields[6]);
  if (fields[7])
    entry.ch.step = atoi(fields[7]);
  if (fields[8])
    entry.ch.modulation = atoi(fields[8]);
  if (fields[9])
    entry.ch.bw = atoi(fields[9]);
  if (fields[10])
    entry.ch.radio = atoi(fields[10]);
  if (fields[11])
    entry.ch.power = atoi(fields[11]);
  if (fields[12])
    entry.ch.scrambler = atoi(fields[12]);
  if (fields[13])
    entry.ch.squelch.value = atoi(fields[13]);
  if (fields[14])
    entry.ch.squelch.type = atoi(fields[14]);
  if (fields[15])
    entry.ch.code.rx.value = atoi(fields[15]);
  if (fields[16])
    entry.ch.code.rx.type = atoi(fields[16]);
  if (fields[17])
    entry.ch.code.tx.value = atoi(fields[17]);
  if (fields[18])
    entry.ch.code.tx.type = atoi(fields[18]);
  if (fields[19])
    entry.ch.fixedBoundsMode = atoi(fields[19]);
  if (fields[20])
    entry.ch.gainIndex = atoi(fields[20]);
  if (fields[21])
    entry.ch.scanlists = atoi(fields[21]);

  // Для Band: дополнительные поля начиная с
  // 22=bank,23=powCalib_s,24=m,25=e,26=lastUsedFreq
  if (type == TYPE_BAND) {
    if (fields[22])
      entry.band.misc.bank = atoi(fields[22]);
    if (fields[23])
      entry.band.misc.powCalib.s = atoi(fields[23]);
    if (fields[24])
      entry.band.misc.powCalib.m = atoi(fields[24]);
    if (fields[25])
      entry.band.misc.powCalib.e = atoi(fields[25]);
    if (fields[26])
      entry.band.misc.lastUsedFreq = atol(fields[26]);
  }

  // Сохранить
  switch (type) {
  case TYPE_CH:
    CHANNELS_Save(num, (CH *)&entry.ch);
    break;
  case TYPE_VFO:
    CHANNELS_Save(num, (CH *)&entry.vfo);
    break;
  case TYPE_BAND:
    CHANNELS_Save(num, (CH *)&entry.band);
    break;
  }
}

// Импорт из файла
void CHANNELS_ImportFromFile(CHType type) {
  const char *filename = GetFileNameByType(type);
  if (!filename)
    return;

  usb_fs_handle_t handle;
  if (usb_fs_open(filename, &handle) != 0) {
    return;
  }

  ParseCSV(filename, &handle, type);
  // usb_fs_close(&handle);
}

// Вспомогательная: получить заголовки для типа (теперь по MR->meta.type)
static const char *GetCSVHeadersForMR(const MR *mr) {
  switch (mr->meta.type) {
  case TYPE_CH:
  case TYPE_VFO:
    return "name,rxF,ppm,txF,offsetDir,allowTx,step,modulation,bw,radio,power,"
           "scrambler,squelch_value,squelch_type,code_rx_value,code_rx_type,"
           "code_tx_value,code_tx_type,fixedBoundsMode,gainIndex,scanlists\n";
  case TYPE_BAND:
    return "name,rxF,ppm,txF,offsetDir,allowTx,step,modulation,bw,radio,power,"
           "scrambler,squelch_value,squelch_type,code_rx_value,code_rx_type,"
           "code_tx_value,code_tx_type,fixedBoundsMode,gainIndex,scanlists,"
           "bank,powCalib_s,powCalib_m,powCalib_e,lastUsedFreq\n";
  default:
    return NULL;
  }
}

// Сохранение одного MR в CSV
void CHANNEL_SaveCSV(const char *filename, int16_t num, MR *mr) {
  if (!filename || !mr)
    return;

  const char *headers =
      GetCSVHeaders(mr->meta.type); // тот же, что и для массового
  if (!headers)
    return;

  // перезаписываем файл
  usb_fs_write_file(filename, (const uint8_t *)headers, strlen(headers), false);

  char line[MAX_LINE_LEN * 2];

  if (mr->meta.type == TYPE_BAND) {
    sprintf(line,
            "%d,%.*s,%lu,%ld,%lu,%d,%d,%d,%d,%d,%d,%d,%d,%u,%u,%u,%u,%u,%u,%d,%"
            "u,%u,%u,%u,%u,%u,%lu\n",
            num, 10, mr->name, (unsigned long)mr->rxF, (long)mr->ppm,
            (unsigned long)mr->txF, mr->offsetDir, mr->allowTx, mr->step,
            mr->modulation, mr->bw, mr->radio, mr->power, mr->scrambler,
            mr->squelch.value, mr->squelch.type, mr->code.rx.value,
            mr->code.rx.type, mr->code.tx.value, mr->code.tx.type,
            mr->fixedBoundsMode, mr->gainIndex, mr->scanlists, mr->misc.bank,
            mr->misc.powCalib.s, mr->misc.powCalib.m, mr->misc.powCalib.e,
            (unsigned long)mr->misc.lastUsedFreq);
  } else { // CH/VFO
    sprintf(line,
            "%d,%.*s,%lu,%ld,%lu,%d,%d,%d,%d,%d,%d,%d,%d,%u,%u,%u,%u,%u,%u,%d,%"
            "u,%u\n",
            num, 10, mr->name, (unsigned long)mr->rxF, (long)mr->ppm,
            (unsigned long)mr->txF, mr->offsetDir, mr->allowTx, mr->step,
            mr->modulation, mr->bw, mr->radio, mr->power, mr->scrambler,
            mr->squelch.value, mr->squelch.type, mr->code.rx.value,
            mr->code.rx.type, mr->code.tx.value, mr->code.tx.type,
            mr->fixedBoundsMode, mr->gainIndex, mr->scanlists);
  }

  usb_fs_write_file(filename, (const uint8_t *)line, strlen(line), true);
}

int CHANNEL_LoadCSV(const char *filename, int16_t num, MR *mr) {
  if (!filename || !mr)
    return -1;
  printf("LOAD CSV %s\n", filename);

  usb_fs_handle_t handle;
  if (usb_fs_open(filename, &handle) != 0) {
    printf("[err] cannot open %s\n", filename);
    return -1;
  }

  char line_buf[MAX_LINE_LEN * 2];
  size_t line_pos = 0;
  uint8_t buffer[256]; // Буфер для чтения
  size_t bytes_read = 0;
  size_t buffer_pos = 0;
  bool skip_headers = true;
  bool found = false;

  while (!found) {
    // Если буфер пуст, читаем следующую порцию данных
    if (buffer_pos >= bytes_read) {
      bytes_read = usb_fs_read_bytes(&handle, buffer, sizeof(buffer));
      if (bytes_read == 0) {
        break; // Конец файла
      }
      buffer_pos = 0;
    }

    uint8_t byte = buffer[buffer_pos++];

    if (byte == '\n' || line_pos >= sizeof(line_buf) - 1) {
      line_buf[line_pos] = '\0';
      line_pos = 0;

      if (skip_headers) {
        skip_headers = false;
        continue;
      }

      // Парсинг строки и заполнение mr
      char *fields[30];
      ParseCSVLine(line_buf, fields, 30);

      int idx = 0;

      if (fields[idx]) {
        num = (int16_t)atoi(fields[idx]);
        idx++;
      }

      if (fields[idx])
        strncpy(mr->name, fields[idx++], 10);
      if (fields[idx])
        mr->rxF = atol(fields[idx++]);
      if (fields[idx])
        mr->ppm = atol(fields[idx++]);
      if (fields[idx])
        mr->txF = atol(fields[idx++]);
      if (fields[idx])
        mr->offsetDir = atoi(fields[idx++]);
      if (fields[idx])
        mr->allowTx = atoi(fields[idx++]);
      if (fields[idx])
        mr->step = atoi(fields[idx++]);
      if (fields[idx])
        mr->modulation = atoi(fields[idx++]);
      if (fields[idx])
        mr->bw = atoi(fields[idx++]);
      if (fields[idx])
        mr->radio = atoi(fields[idx++]);
      if (fields[idx])
        mr->power = atoi(fields[idx++]);
      if (fields[idx])
        mr->scrambler = atoi(fields[idx++]);
      if (fields[idx])
        mr->squelch.value = atoi(fields[idx++]);
      if (fields[idx])
        mr->squelch.type = atoi(fields[idx++]);
      if (fields[idx])
        mr->code.rx.value = atoi(fields[idx++]);
      if (fields[idx])
        mr->code.rx.type = atoi(fields[idx++]);
      if (fields[idx])
        mr->code.tx.value = atoi(fields[idx++]);
      if (fields[idx])
        mr->code.tx.type = atoi(fields[idx++]);
      if (fields[idx])
        mr->fixedBoundsMode = atoi(fields[idx++]);
      if (fields[idx])
        mr->gainIndex = atoi(fields[idx++]);
      if (fields[idx])
        mr->scanlists = atoi(fields[idx++]);

      if (fields[idx]) {
        // есть доп. поля — считаем, что это BAND
        mr->meta.type = TYPE_BAND;
        mr->misc.bank = atoi(fields[idx++]);
        mr->misc.powCalib.s = atoi(fields[idx++]);
        mr->misc.powCalib.m = atoi(fields[idx++]);
        mr->misc.powCalib.e = atoi(fields[idx++]);
        mr->misc.lastUsedFreq = atol(fields[idx++]);
      }

      found = true; // Нашли запись
    } else {
      line_buf[line_pos++] = byte;
    }
  }

  // usb_fs_close(&handle);

  if (!found) {
    printf("[err] no valid record found in %s\n", filename);
    return -1;
  }

  printf("LOAD CSV %s OK\n", filename);
  return 0;
}
