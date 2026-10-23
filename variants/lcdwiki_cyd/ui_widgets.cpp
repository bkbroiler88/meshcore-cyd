#include "ui_widgets.h"

static void wtxt(LGFX_CYD* g, int x, int y, const char* s, uint16_t color, textdatum_t datum, const lgfx::IFont* font = NULL) {
  g->setFont(font ? font : FONT_BODY);
  g->setTextSize(1);
  g->setTextColor(color);
  g->setTextDatum(datum);
  g->drawString(s, x, y);
}

// ---------------------------------------------------------------- SettingsList

int SettingsList::header(const char* label) {
  if (_count >= MAX_ITEMS) return -1;
  SetItem& it = _items[_count];
  it.type = SET_HEADER; it.id = 255; it.danger = false; it.label = label; it.value[0] = 0;
  return _count++;
}
int SettingsList::stepper(uint8_t id, const char* label, const char* value) {
  if (_count >= MAX_ITEMS) return -1;
  SetItem& it = _items[_count];
  it.type = SET_STEPPER; it.id = id; it.danger = false; it.label = label;
  strncpy(it.value, value, sizeof(it.value) - 1); it.value[sizeof(it.value) - 1] = 0;
  return _count++;
}
int SettingsList::button(uint8_t id, const char* label, const char* caption, bool danger) {
  if (_count >= MAX_ITEMS) return -1;
  SetItem& it = _items[_count];
  it.type = SET_BUTTON; it.id = id; it.danger = danger; it.label = label;
  strncpy(it.value, caption, sizeof(it.value) - 1); it.value[sizeof(it.value) - 1] = 0;
  return _count++;
}
void SettingsList::setValue(uint8_t id, const char* value) {
  for (int i = 0; i < _count; i++) if (_items[i].id == id) {
    strncpy(_items[i].value, value, sizeof(_items[i].value) - 1); _items[i].value[sizeof(_items[i].value) - 1] = 0;
  }
}
const char* SettingsList::valueOf(uint8_t id) const {
  for (int i = 0; i < _count; i++) if (_items[i].id == id) return _items[i].value;
  return "";
}

int SettingsList::rowY(int i, int top) const {
  if (i < _scroll) return -1;
  int y = top;
  for (int k = _scroll; k < i; k++) y += rowHeight(k);
  return y;
}

void SettingsList::drawRow(LGFX_CYD* g, int i, int y, bool frame, bool pMinus, bool pPlus, bool pBtn) {
  const SetItem& it = _items[i];
  int h = rowHeight(i);
  if (it.type == SET_HEADER) {
    if (frame) {
      g->fillRect(0, y, GW, h, TH.bg);
      wtxt(g, S(8), y + h / 2, it.label, TH.muted, textdatum_t::middle_left, FONT_SMALL);
    }
    return;
  }
  int cy = y + h / 2;
  if (frame) {
    g->fillRect(0, y, GW, h, TH.bg);
    g->drawFastHLine(0, y + h - 1, GW, TH.line);
    g->setClipRect(0, y, GW - S(8 + STEP_W + 4 + VAL_W + 4 + STEP_W) - S(6), h);
    wtxt(g, S(8), cy, it.label, TH.text, textdatum_t::middle_left);
    g->clearClipRect();
  }
  int bh = S(BTN_H), by = cy - bh / 2;
  if (it.type == SET_STEPPER) {
    int plusX = GW - S(8) - S(STEP_W);
    int valX = plusX - S(4) - S(VAL_W);
    int minusX = valX - S(4) - S(STEP_W);
    g->fillRoundRect(minusX, by, S(STEP_W), bh, S(6), pMinus ? TH.keyDown : TH.key);
    wtxt(g, minusX + S(STEP_W) / 2, cy, "-", pMinus ? TH.bg : TH.accent, textdatum_t::middle_center, FONT_BIG);
    g->fillRect(valX, by, S(VAL_W), bh, TH.bg);
    g->setClipRect(valX, by, S(VAL_W), bh);
    wtxt(g, valX + S(VAL_W) / 2, cy, it.value, TH.text, textdatum_t::middle_center);
    g->clearClipRect();
    g->fillRoundRect(plusX, by, S(STEP_W), bh, S(6), pPlus ? TH.keyDown : TH.key);
    wtxt(g, plusX + S(STEP_W) / 2, cy, "+", pPlus ? TH.bg : TH.accent, textdatum_t::middle_center, FONT_BIG);
  } else {
    int bx = GW - S(8) - S(BTN_W);
    g->fillRoundRect(bx, by, S(BTN_W), bh, S(6), pBtn ? TH.keyDown : TH.key);
    g->setClipRect(bx, by, S(BTN_W), bh);
    wtxt(g, bx + S(BTN_W) / 2, cy, it.value, pBtn ? TH.bg : (it.danger ? TH.danger : TH.accent), textdatum_t::middle_center);
    g->clearClipRect();
  }
}

void SettingsList::draw(LGFX_CYD* g, int top, int bottom, bool full) {
  if (full) { g->fillRect(0, top, GW, bottom - top, TH.bg); memset(_cache, 0, sizeof(_cache)); }
  int y = top;
  for (int i = _scroll; i < _count && y < bottom; i++) {
    int h = rowHeight(i);
    if (y + h > bottom) break;
    bool changed = strcmp(_cache[i], _items[i].value) != 0;
    if (full || changed) {
      strncpy(_cache[i], _items[i].value, 23); _cache[i][23] = 0;
      drawRow(g, i, y, full, false, false, false);
    }
    y += h;
  }
  // scroll thumb
  int total = 0; for (int i = 0; i < _count; i++) total += rowHeight(i);
  if (total > bottom - top) {
    int trackH = bottom - top;
    int thumbH = trackH * (bottom - top) / total; if (thumbH < S(10)) thumbH = S(10);
    int before = 0; for (int i = 0; i < _scroll; i++) before += rowHeight(i);
    int thumbY = top + (trackH - thumbH) * before / (total - (bottom - top) > 0 ? total - (bottom - top) : 1);
    g->fillRect(GW - S(3), top, S(3), trackH, TH.bg);
    g->fillRect(GW - S(3), thumbY, S(3), thumbH, TH.muted);
  }
}

int SettingsList::hit(int x, int y, int top, int bottom, SetPart* part) const {
  *part = PART_NONE;
  int ry = top;
  for (int i = _scroll; i < _count && ry < bottom; i++) {
    int h = rowHeight(i);
    if (y >= ry && y < ry + h) {
      const SetItem& it = _items[i];
      if (it.type == SET_HEADER) return -1;
      int bh = S(BTN_H), by = ry + h / 2 - bh / 2;
      if (y < by - S(4) || y >= by + bh + S(4)) return -1;   // outside the control band
      if (it.type == SET_STEPPER) {
        int plusX = GW - S(8) - S(STEP_W);
        int valX = plusX - S(4) - S(VAL_W);
        int minusX = valX - S(4) - S(STEP_W);
        if (x >= minusX - S(4) && x < valX) { *part = PART_MINUS; return it.id; }
        if (x >= plusX - S(2) && x < GW) { *part = PART_PLUS; return it.id; }
        return -1;
      }
      int bx = GW - S(8) - S(BTN_W);
      if (x >= bx - S(4)) { *part = PART_BUTTON; return it.id; }
      return -1;
    }
    ry += h;
  }
  return -1;
}

void SettingsList::flash(LGFX_CYD* g, int top, int bottom, uint8_t id, SetPart part) {
  for (int i = _scroll; i < _count; i++) {
    if (_items[i].id != id) continue;
    int y = rowY(i, top);
    if (y < 0 || y + rowHeight(i) > bottom) return;
    drawRow(g, i, y, false, part == PART_MINUS, part == PART_PLUS, part == PART_BUTTON);
    return;
  }
}

bool SettingsList::scrollBy(int rows, int top, int bottom) {
  int s = _scroll + rows;
  if (s < 0) s = 0;
  // do not scroll past the point where the last row is visible
  int maxScroll = 0, acc = 0;
  for (int i = _count - 1; i >= 0; i--) { acc += rowHeight(i); if (acc > bottom - top) { maxScroll = i + 1; break; } }
  if (s > maxScroll) s = maxScroll;
  if (s == _scroll) return false;
  _scroll = s;
  return true;
}

// ---------------------------------------------------------------- StatsList

void StatsList::row(const char* label, const char* value) {
  if (_count >= MAX_ROWS) return;
  _rows[_count].label = label;
  strncpy(_rows[_count].value, value, sizeof(_rows[_count].value) - 1); _rows[_count].value[sizeof(_rows[_count].value) - 1] = 0;
  _count++;
}

void StatsList::draw(LGFX_CYD* g, int top, int bottom, bool full) {
  int rh = S(ROW_H);
  bool relayout = full || _lastScroll != _scroll;
  if (relayout) { _lastScroll = _scroll; g->fillRect(0, top, GW, bottom - top, TH.bg); memset(_cache, 0, sizeof(_cache)); }
  int visible = (bottom - top) / rh;
  for (int r = 0; r < visible; r++) {
    int i = _scroll + r;
    if (i >= _count) break;
    int y = top + r * rh;
    if (relayout) {
      g->drawFastHLine(0, y + rh - 1, GW, TH.line);
      wtxt(g, S(8), y + rh / 2, _rows[i].label, TH.muted, textdatum_t::middle_left);
    }
    if (strcmp(_cache[i], _rows[i].value) == 0) continue;
    strncpy(_cache[i], _rows[i].value, 39); _cache[i][39] = 0;
    int vx = S(118);
    g->setClipRect(vx, y, GW - vx - S(8), rh - 1);
    g->fillRect(vx, y, GW - vx - S(8), rh - 1, TH.bg);
    wtxt(g, GW - S(8), y + rh / 2, _rows[i].value, TH.text, textdatum_t::middle_right);
    g->clearClipRect();
  }
  if (_count > visible) {
    int trackH = bottom - top, thumbH = trackH * visible / _count;
    int thumbY = top + (trackH - thumbH) * _scroll / (_count - visible);
    g->fillRect(GW - S(3), top, S(3), trackH, TH.bg);
    g->fillRect(GW - S(3), thumbY, S(3), thumbH, TH.muted);
  }
}

bool StatsList::scrollBy(int rows, int top, int bottom) {
  int visible = (bottom - top) / S(ROW_H);
  int maxScroll = _count > visible ? _count - visible : 0;
  int s = _scroll + rows; if (s < 0) s = 0; if (s > maxScroll) s = maxScroll;
  if (s == _scroll) return false;
  _scroll = s; return true;
}
