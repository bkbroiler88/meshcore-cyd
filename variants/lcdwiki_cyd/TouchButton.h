#pragma once

#include <Arduino.h>
#include <helpers/ui/MomentaryButton.h>   // BUTTON_EVENT_* codes
#include "CYDDisplay.h"

// The XPT2046 touch panel treated as a momentary button: same click / long-press /
// double / triple events as MomentaryButton, plus the logical touch-down point so the
// caller can map screen regions onto keys.
class TouchButton {
  CYDDisplay* _display;
  bool prev;
  int _long_millis;
  unsigned long down_at;
  uint8_t _click_count;
  unsigned long _last_click_time;
  bool _pending_click;
  int _down_x, _down_y;

public:
  TouchButton(CYDDisplay* display, int long_press_millis = 1000)
    : _display(display), prev(false), _long_millis(long_press_millis), down_at(0),
      _click_count(0), _last_click_time(0), _pending_click(false), _down_x(0), _down_y(0) {}

  void begin() {}
  int check(int* x = NULL, int* y = NULL);   // returns BUTTON_EVENT_*; x/y = touch-down point
  bool isPressed() const;
};
