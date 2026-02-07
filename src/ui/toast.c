#include "toast.h"
#include "../driver/systick.h"
#include "graphics.h"
#include <string.h>

#define MAX_EVENTS 3
#define EVENT_HEIGHT 10
#define EVENT_SLIDE_SPEED 1 // пикселей за кадр

typedef struct {
  char text[28];
  uint32_t timestamp;
  int8_t yOffset; // для анимации
  bool active;
} ToastEvent_t;

static ToastEvent_t eventQueue[MAX_EVENTS];

void TOAST_Push(const char *pattern, ...) {
  char text[32] = {0};
  va_list args;
  va_start(args, pattern);
  vsnprintf(text, 31, pattern, args);
  va_end(args);
  gRedrawScreen = true;
  // Сдвигаем существующие события вверх
  for (int i = MAX_EVENTS - 1; i > 0; i--) {
    eventQueue[i] = eventQueue[i - 1];
  }

  // Добавляем новое
  strncpy(eventQueue[0].text, text, sizeof(eventQueue[0].text) - 1);
  eventQueue[0].timestamp = Now();
  eventQueue[0].yOffset = EVENT_HEIGHT; // начинаем снизу
  eventQueue[0].active = true;

  gRedrawScreen = true;
}

void TOAST_Update(void) {
  uint32_t now = Now();
  bool hasActiveAnimations = false;

  for (int i = 0; i < MAX_EVENTS; i++) {
    if (!eventQueue[i].active)
      continue;

    uint32_t age = now - eventQueue[i].timestamp;

    // Удаляем старые
    if (age > 5000) {
      eventQueue[i].active = false;
      gRedrawScreen = true; // Перерисовка при удалении
      continue;
    }

    // Анимация въезда
    if (eventQueue[i].yOffset > 0) {
      eventQueue[i].yOffset -= EVENT_SLIDE_SPEED;
      if (eventQueue[i].yOffset < 0) {
        eventQueue[i].yOffset = 0;
      }
      hasActiveAnimations = true;
    }
  }

  // Перерисовываем если есть активные анимации
  if (hasActiveAnimations) {
    gRedrawScreen = true;
  }
}

void TOAST_Render(void) {
  int currentY = 64 - EVENT_HEIGHT;

  for (int i = 0; i < MAX_EVENTS; i++) {
    if (!eventQueue[i].active)
      continue;

    int y = currentY - (i * EVENT_HEIGHT) + eventQueue[i].yOffset;

    // Рисуем только видимые
    if (y >= 0 && y < LCD_HEIGHT) {
      FillRect(0, y, LCD_WIDTH, EVENT_HEIGHT - 1, C_CLEAR);
      DrawRect(0, y, LCD_WIDTH, EVENT_HEIGHT - 1, C_FILL);
      PrintSmall(2, y + 1 + 5, "%s", eventQueue[i].text);
    }
  }
}
