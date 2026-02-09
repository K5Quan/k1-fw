#include "newscan.h"
#include "../driver/systick.h"
#include "../driver/uart.h"
#include "../helper/lootlist.h"
#include "../helper/regs-menu.h"
#include "../helper/scan.h"
#include "../settings.h"
#include "../ui/finput.h"
#include "../ui/spectrum.h"
#include "../ui/statusline.h"
#include "apps.h"
#include <stdbool.h>

static Band range;
static VMinMax minMax;

static uint32_t targetF = 434 * MHZ;
static uint32_t delay = 1200;
static uint8_t stp = 10;
static SQL sq;
static bool still;

static Measurement *msm;
static Measurement tgt[3];

static void setTargetF(uint32_t fs, uint32_t _) { targetF = fs; }

static void setRange(uint32_t fs, uint32_t fe) {
  range.start = fs;
  range.end = fe;
  msm->f = range.start;
  SP_Init(&range);
}

bool NEWSCAN_key(KEY_Code_t key, Key_State_t state) {
  if (REGSMENU_Key(key, state)) {
    return true;
  }
  if (state == KEY_RELEASED || state == KEY_LONG_PRESSED_CONT) {
    switch (key) {
    case KEY_5:
      FINPUT_setup(0, BK4819_F_MAX, UNIT_MHZ, true);
      FINPUT_Show(setRange);
      return true;
    case KEY_6:
      FINPUT_setup(0, BK4819_F_MAX, UNIT_MHZ, false);
      FINPUT_Show(setTargetF);
      return true;

    case KEY_SIDE1:
      LOOT_BlacklistLast();
      return true;

    case KEY_SIDE2:
      LOOT_WhitelistLast();
      return true;

    case KEY_STAR:
      APPS_run(APP_LOOTLIST);
      return true;

    case KEY_UP:
    case KEY_DOWN:
      delay = AdjustU(delay, 0, 10000,
                      ((key == KEY_UP) ^ gSettings.invertButtons) ? 100 : -100);
      return true;

    case KEY_4:
      still = !still;
      return true;

    case KEY_1:
    case KEY_7:
      sq.ro = AdjustU(sq.ro, 0, 255, key == KEY_1 ? stp : -stp);
      return true;
    case KEY_2:
    case KEY_8:
      sq.no = AdjustU(sq.no, 0, 255, key == KEY_2 ? stp : -stp);
      return true;
    case KEY_3:
    case KEY_9:
      sq.go = AdjustU(sq.go, 0, 255, key == KEY_3 ? stp : -stp);
      return true;
    case KEY_F:
      if (stp == 100) {
        stp = 1;
      } else {
        stp *= 10;
      }
      return true;
    }
  }
  return false;
}

void NEWSCAN_init(void) {
  SPECTRUM_Y = 8;
  SPECTRUM_H = 44;

  range.step = STEP_25_0kHz;
  range.start = 43307500;
  range.end = range.start + StepFrequencyTable[range.step] * LCD_WIDTH;
  msm = &vfo->msm;
  msm->f = range.start;

  sq = GetSql(9);

  SCAN_SetMode(SCAN_MODE_NONE);
  SP_Init(&range);
}

void NEWSCAN_deinit(void) {}

void measure() {
  msm->rssi = RADIO_GetRSSI(ctx);
  msm->noise = RADIO_GetNoise(ctx);
  msm->glitch = RADIO_GetGlitch(ctx);

  msm->open = msm->rssi >= sq.ro && msm->noise < sq.no && msm->glitch < sq.go;
  LOOT_Update(msm);

  if (msm->f == targetF - StepFrequencyTable[range.step]) {
    tgt[0] = *msm;
  }
  if (msm->f == targetF) {
    tgt[1] = *msm;
  }
  if (msm->f == targetF + StepFrequencyTable[range.step]) {
    tgt[2] = *msm;
  }
}

void updateListening() {
  static uint32_t lastListenUpdate;
  if (Now() - lastListenUpdate >= SQL_DELAY) {
    measure();
    lastListenUpdate = Now();
  }
}

void updateScan() {
  RADIO_SetParam(ctx, PARAM_FREQUENCY, msm->f, false);
  RADIO_ApplySettings(ctx);
  SYSTICK_DelayUs(delay);

  measure();
  SP_AddPoint(msm);
  LOOT_Update(msm);

  if (still) {
    return;
  }

  msm->f += StepFrequencyTable[range.step];

  if (msm->f > range.end) {
    msm->f = range.start;
    gRedrawScreen = true;
  }
}

void NEWSCAN_update(void) {
  if (vfo->is_open) {
    updateListening();
  } else {
    updateScan();
  }
  if (vfo->is_open != msm->open) {
    // Log("OPEN=%u", msm->open);
    vfo->is_open = msm->open;
    gRedrawScreen = true;
    targetF = msm->f;
    RADIO_SwitchAudioToVFO(gRadioState, gRadioState->active_vfo_index);
  }
}
static void renderBottomFreq() {
  uint32_t leftF = range.start;
  uint32_t centerF = msm->f;
  uint32_t rightF = range.end;

  FSmall(1, LCD_HEIGHT - 2, POS_L, leftF);
  FSmall(LCD_XCENTER, LCD_HEIGHT - 2, POS_C, targetF);
  FSmall(LCD_WIDTH - 1, LCD_HEIGHT - 2, POS_R, rightF);
}

void NEWSCAN_render(void) {
  if (still) {
    PrintSmallEx(LCD_XCENTER, 12 + 6 * 3, POS_C, C_FILL, "%s", "STILL");
  }

  STATUSLINE_RenderRadioSettings();
  minMax = SP_GetMinMax();
  SP_Render(&range, minMax);
  PrintSmall(0, 12 + 6 * 0, "R %u", sq.ro);
  PrintSmall(0, 12 + 6 * 1, "N %u", sq.no);
  PrintSmall(0, 12 + 6 * 2, "G %u", sq.go);
  PrintSmall(0, 12 + 6 * 3, "STP %u", stp);

  PrintSmallEx(LCD_WIDTH - 1, 12 + 6 * 0, POS_R, C_FILL, "%u", SP_GetRssiMax());
  PrintSmallEx(LCD_WIDTH - 1, 12 + 6 * 1, POS_R, C_FILL, "%u",
               SP_GetNoiseFloor());

  PrintSmallEx(LCD_WIDTH - 1, 12 + 6 * 2, POS_R, C_FILL, "%uus", delay);
  PrintSmallEx(LCD_WIDTH - 1, 12 + 6 * 3, POS_R, C_FILL, "%s",
               vfo->is_open ? "OPEN" : "...");

  PrintSmallEx(LCD_XCENTER, 12 + 6 * 0, POS_C, C_FILL, "%3u %3u %3u",
               tgt[0].rssi, tgt[1].rssi, tgt[2].rssi);
  PrintSmallEx(LCD_XCENTER, 12 + 6 * 1, POS_C, C_FILL, "%3u %3u %3u",
               tgt[0].noise, tgt[1].noise, tgt[2].noise);
  PrintSmallEx(LCD_XCENTER, 12 + 6 * 2, POS_C, C_FILL, "%3u %3u %3u",
               tgt[0].glitch, tgt[1].glitch, tgt[2].glitch);

  renderBottomFreq();

  REGSMENU_Draw();
}
