#include "TouchButton.h"

#define MULTI_CLICK_WINDOW_MS  280

bool TouchButton::isPressed() const {
  int x, y;
  return _display->touched(&x, &y);
}

int TouchButton::check(int* x, int* y) {
  int event = BUTTON_EVENT_NONE;
  int tx, ty;
  bool pressed = _display->touched(&tx, &ty);

  if (pressed != prev) {
    if (pressed) {
      down_at = millis();
      _down_x = tx;
      _down_y = ty;
    } else {
      if (down_at > 0 && (unsigned long)(millis() - down_at) < (unsigned long)_long_millis) {
        _click_count++;
        _last_click_time = millis();
        _pending_click = true;
      }
      down_at = 0;
    }
    prev = pressed;
  }

  if (_long_millis > 0 && down_at > 0 && (unsigned long)(millis() - down_at) >= (unsigned long)_long_millis) {
    if (_pending_click) {
      _click_count = 0;
      _last_click_time = 0;
      _pending_click = false;
      down_at = 0;
    } else {
      event = BUTTON_EVENT_LONG_PRESS;
      down_at = 0;
      _click_count = 0;
      _last_click_time = 0;
      _pending_click = false;
    }
  }

  if (_pending_click && (millis() - _last_click_time) >= MULTI_CLICK_WINDOW_MS) {
    if (down_at > 0) return event;   // still pressed - wait for release
    switch (_click_count) {
      case 1:  event = BUTTON_EVENT_CLICK; break;
      case 2:  event = BUTTON_EVENT_DOUBLE_CLICK; break;
      default: event = BUTTON_EVENT_TRIPLE_CLICK; break;
    }
    _click_count = 0;
    _last_click_time = 0;
    _pending_click = false;
  }

  if (event != BUTTON_EVENT_NONE) {
    if (x) *x = _down_x;
    if (y) *y = _down_y;
  }
  return event;
}
