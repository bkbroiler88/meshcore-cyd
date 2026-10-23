#pragma once

// Finger-friendly list widgets shared by the companion and repeater colour UIs.
//  - SettingsList: 40 px rows, each with ONE control on the right (a -/+ stepper or a
//    labelled button). The label side is dead space, so a wobbly tap lands on nothing
//    rather than on the neighbouring row. Swipe scrolls by whole rows.
//  - StatsList: read-only label/value rows, per-row cached so only a changed value repaints.
// All sizes in 320x240 units through S(); positions anchor on the real panel width.

#include <Arduino.h>
#include "CYDDisplay.h"
#include "ui_theme.h"

enum SetType : uint8_t { SET_HEADER = 0, SET_STEPPER, SET_BUTTON };
enum SetPart : int8_t { PART_NONE = -1, PART_MINUS = 0, PART_PLUS = 1, PART_BUTTON = 2 };

struct SetItem {
  uint8_t type;
  uint8_t id;          // caller's action id
  bool danger;         // red button text
  const char* label;
  char value[24];      // stepper value or button caption
};

class SettingsList {
public:
  static const int MAX_ITEMS = 24;
  static const int ROW_H = 40;      // 320x240 units
  static const int HDR_H = 18;
  static const int BTN_H = 30;
  static const int BTN_W = 76;      // button
  static const int STEP_W = 40;     // -/+ key
  static const int VAL_W = 56;      // value box between the keys

  void clear() { _count = 0; _scroll = 0; memset(_cache, 0, sizeof(_cache)); }
  int  header(const char* label);
  int  stepper(uint8_t id, const char* label, const char* value);
  int  button(uint8_t id, const char* label, const char* caption, bool danger = false);
  void setValue(uint8_t id, const char* value);     // marks the row for repaint
  const char* valueOf(uint8_t id) const;

  // draw rows [scroll..] into [top,bottom); full = repaint everything
  void draw(LGFX_CYD* g, int top, int bottom, bool full);
  // hit test a tap; returns item id or -1, part tells which control
  int  hit(int x, int y, int top, int bottom, SetPart* part) const;
  void flash(LGFX_CYD* g, int top, int bottom, uint8_t id, SetPart part);  // pressed look
  bool scrollBy(int rows, int top, int bottom);   // true if moved
  int  scroll() const { return _scroll; }

private:
  SetItem _items[MAX_ITEMS];
  char _cache[MAX_ITEMS][24];
  int _count = 0, _scroll = 0;
  int rowHeight(int i) const { return S(_items[i].type == SET_HEADER ? HDR_H : ROW_H); }
  int rowY(int i, int top) const;   // y of row i given scroll, or -1 if above
  void drawRow(LGFX_CYD* g, int i, int y, bool frame, bool pressedMinus, bool pressedPlus, bool pressedBtn);
};

struct StatRow { const char* label; char value[40]; };

class StatsList {
public:
  static const int MAX_ROWS = 20;
  static const int ROW_H = 24;
  void clear() { _count = 0; }
  void row(const char* label, const char* value);
  void draw(LGFX_CYD* g, int top, int bottom, bool full);
  bool scrollBy(int rows, int top, int bottom);
private:
  StatRow _rows[MAX_ROWS];
  char _cache[MAX_ROWS][40];
  int _count = 0, _scroll = 0, _lastScroll = -1;
};
