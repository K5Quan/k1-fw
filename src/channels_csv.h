#ifndef CHANNELS_CSV_H
#define CHANNELS_CSV_H

#include "helper/channels.h"

#define MAX_LINE_LEN 512

void CHANNEL_SaveCSV(const char *filename, int16_t num, MR *mr);
int CHANNEL_LoadCSV(const char *filename, int16_t num, MR *mr); // -1 = err
void CHANNELS_WriteBandToFile(int16_t num, const char *filename, bool append,
                              Band *band);

#endif // CHANNELS_CSV_H
