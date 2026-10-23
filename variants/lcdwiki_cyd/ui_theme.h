#pragma once

// Colour theme + geometry for the CYD touch UIs (companion ui-cyd and repeater ui-cyd).
// Palette table and geometry live in ui_theme.cpp (compiled with the variant).
#include <stdint.h>

// Runtime geometry. The layout is authored in 320x240 units; S() scales SIZES by fs
// (= min(W/320, H/240), so vertical positions map exactly on both panels), while
// horizontal positions anchor on the real width GW. 2.8": 320x240 fs=1.0;
// 3.5": 480x320 fs=1.333 - a 3:2 panel, NOT a 4:3 one scaled up.
struct Geo { int W, H; float fs; };
extern Geo g_geo;
#define GW (g_geo.W)
#define GH (g_geo.H)
static inline int S(int v) { return (int)(v * g_geo.fs + 0.5f); }

// Anti-aliased fonts by role, picked per panel (no bitmap scaling)
#define FONT_BODY  ((g_geo.fs >= 1.25f) ? (const lgfx::IFont*)&fonts::DejaVu18 : (const lgfx::IFont*)&fonts::DejaVu12)
#define FONT_BIG   ((g_geo.fs >= 1.25f) ? (const lgfx::IFont*)&fonts::DejaVu24 : (const lgfx::IFont*)&fonts::DejaVu18)
#define FONT_SMALL ((g_geo.fs >= 1.25f) ? (const lgfx::IFont*)&fonts::DejaVu12 : (const lgfx::IFont*)&fonts::DejaVu9)

constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

struct Palette {
  const char* name;
  uint16_t bg, bar, line, text, muted, accent, warn, danger, ok;
  uint16_t tileBlue, tileBlueFg, tileGreen, tileGreenFg, tileAmber, tileAmberFg, tilePurple, tilePurpleFg;
  uint16_t bubbleIn, bubbleOut, bubbleOutFg, key, keyDown, field;
};
#define THEME_COUNT 5
extern const Palette THEMES[THEME_COUNT];
extern const Palette* g_theme;
#define TH (*g_theme)

// Fixed layout (320x240 units)
namespace L {
  constexpr int W = 320, H = 240;
  constexpr int STATUS_H = 22;
  constexpr int TAB_H = 28;
  constexpr int ROW_H = 38;
#if defined(LCDWIKI_28_ST7789)
  constexpr int KEY_H = 33;      // 2.8": taller keys (user request); one line of history stays in view
  constexpr int KEY_GAP = 2;
#else
  constexpr int KEY_H = 22;
  constexpr int KEY_GAP = 3;
#endif
  constexpr int KB_ROWS = 4;
  constexpr int KB_H = KB_ROWS * (KEY_H + KEY_GAP) + 3;   // 103
  constexpr int FIELD_H = 24;
}
