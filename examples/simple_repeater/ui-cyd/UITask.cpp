#include "UITask.h"
#include "../MyMesh.h"
#include "target.h"
#include <Preferences.h>
#include <helpers/TxtDataHelpers.h>

extern MyMesh the_mesh;

#define SPLASH_MILLIS      2500
#define LONG_PRESS_MILLIS  900
#define TAP_SLOP           12
#define TICK_MILLIS        1000
#define D_TILES   0x01
#define D_LINES   0x02
#define D_LIST    0x01
#define NB_ROW_H  34
#define MORE_ROW_H 21
#define MORE_ROWS 18

// ---------------------------------------------------------------- helpers

static void txt(LGFX_CYD* g, int x, int y, const char* s, uint16_t color, textdatum_t datum = textdatum_t::top_left, float size = 1.0f, const lgfx::IFont* font = NULL) {
  g->setFont(font ? font : FONT_BODY);
  g->setTextSize(1);
  g->setTextColor(color);
  g->setTextDatum(datum);
  g->drawString(s, x, y);
}

void UITask::loadTheme() {
  Preferences p;
  if (p.begin("uicyd", true)) { _themeIdx = p.getUChar("theme", 0); p.end(); }
  if (_themeIdx >= THEME_COUNT) _themeIdx = 0;
  g_theme = &THEMES[_themeIdx];
}
void UITask::saveTheme() {
  Preferences p;
  if (p.begin("uicyd", false)) { p.putUChar("theme", _themeIdx); p.end(); }
}

int UITask::battPercent() {
  int mv = _board->getBattMilliVolts();
  if (mv <= 0) return -1;
  if (_battEma < 0) _battEma = mv; else _battEma += (mv - _battEma) / 8;
  int p = (_battEma - 3000) * 100 / 1200;
  p = p < 0 ? 0 : (p > 100 ? 100 : p);
  return (p / 5) * 5;
}

void UITask::fmtAgo(uint32_t ts, char* out, size_t n) {
  uint32_t now = rtc_clock.getCurrentTime();
  if (ts == 0 || ts > now) { snprintf(out, n, "-"); return; }
  uint32_t s = now - ts;
  if (s < 60) snprintf(out, n, "%us", (unsigned)s);
  else if (s < 3600) snprintf(out, n, "%um", (unsigned)(s / 60));
  else if (s < 86400) snprintf(out, n, "%uh", (unsigned)(s / 3600));
  else snprintf(out, n, "%ud", (unsigned)(s / 86400));
}

static void fmtUptime(uint32_t secs, char* out, size_t n) {
  if (secs < 3600) snprintf(out, n, "up %um", (unsigned)(secs / 60));
  else if (secs < 86400) snprintf(out, n, "up %uh %um", (unsigned)(secs / 3600), (unsigned)((secs % 3600) / 60));
  else snprintf(out, n, "up %ud %uh", (unsigned)(secs / 86400), (unsigned)((secs % 86400) / 3600));
}

bool UITask::unlocked() {
#ifdef PIN_USER_BTN
  if (user_btn.isPressed()) return true;
#endif
  showAlert("Hold BOOT to change", 1200);
  return false;
}

// Apply a setting through the repeater's own CLI (sender 0 = local admin), so every
// range check, timer update and prefs save is the same one the serial console gets.
void UITask::runCli(const char* cmd) {
  char c[64], reply[160];
  StrHelper::strncpy(c, cmd, sizeof(c));
  reply[0] = 0;
  the_mesh.handleCommand(0, c, reply);
  if (reply[0]) showAlert(reply, 1200);
  _dirty |= D_LIST; _statusCache[0] = 0;
}

// ---------------------------------------------------------------- lifecycle

void UITask::begin(NodePrefs* node_prefs, const char* build_date, const char* firmware_version) {
  _prefs = node_prefs;
  _gfx = &_display->device();
  g_geo.W = _gfx->width(); g_geo.H = _gfx->height();
  float fx = g_geo.W / 320.0f, fy = g_geo.H / 240.0f;
  g_geo.fs = fx < fy ? fx : fy;
  StrHelper::strncpy(_version, firmware_version, sizeof(_version));
  StrHelper::strncpy(_build, build_date, sizeof(_build));
#ifdef PIN_USER_BTN
  user_btn.begin();
#endif
  loadTheme();
  _ui_started_at = millis();
  _splash_until = millis() + SPLASH_MILLIS;
  _display->turnOn();
  setScreen(Screen::SPLASH);
}

void UITask::setScreen(Screen s) {
#ifdef SPECTRUM_ANALYZER
  if (_screen == Screen::SPECTRUM && s != Screen::SPECTRUM) exitSpectrum();
#endif
  _screen = s;
  _fullRedraw = true; _dirty = 0; _listScroll = 0; _confirmReboot = false;
  _statusCache[0] = 0; _heapCacheKb = -1;
  memset(_tileCache, 0, sizeof(_tileCache)); memset(_lineCache, 0, sizeof(_lineCache)); memset(_moreRowCache, 0, sizeof(_moreRowCache));
}

void UITask::showAlert(const char* text, int ms) {
  StrHelper::strncpy(_alert, text, sizeof(_alert));
  _alert_expiry = millis() + ms;
  drawAlert();
}

// ---------------------------------------------------------------- shared chrome

void UITask::drawStatusBar(bool force) {
  LGFX_CYD* g = _gfx;
  int cy = S(L::STATUS_H / 2);
  if (force) {
    g->fillRect(0, 0, GW, S(L::STATUS_H), TH.bar);
    g->setClipRect(0, 0, S(118), S(L::STATUS_H));
    txt(g, S(8), cy, _prefs->node_name, TH.text, textdatum_t::middle_left);
    g->clearClipRect();
    _heapCacheKb = -1; _statusCache[0] = 0;
  }
  int kb = (int)(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) / 1024);
  if (kb != _heapCacheKb) {
    _heapCacheKb = kb;
    char hb[8]; snprintf(hb, sizeof(hb), "%dk", kb);
    uint16_t pbg = kb < 16 ? TH.danger : (kb < 24 ? TH.tileAmber : TH.tileGreen);
    uint16_t pfg = kb < 16 ? TH.text : (kb < 24 ? TH.tileAmberFg : TH.tileGreenFg);
    g->fillRect(S(120), 0, S(46), S(L::STATUS_H), TH.bar);
    int pw = g->textWidth(hb) + S(10);
    g->fillRoundRect(S(122), cy - S(8), pw, S(16), S(8), pbg);
    txt(g, S(122) + pw / 2, cy, hb, pfg, textdatum_t::middle_center);
  }
  char up[16]; fmtUptime(the_mesh.getUptimeSecs(), up, sizeof(up));
  int b = battPercent();
  char line[40]; snprintf(line, sizeof(line), "%s|%d", up, b);
  if (strcmp(line, _statusCache) == 0) return;
  strcpy(_statusCache, line);
  g->setFont(FONT_BODY); g->setTextSize(1);
  g->setTextDatum(textdatum_t::middle_right);
  int x = GW - S(8);
  char bs[8]; if (b >= 0) snprintf(bs, sizeof(bs), "%3d%%", b); else strcpy(bs, "    ");
  g->setTextColor(TH.text, TH.bar); g->drawString(bs, x, cy);
  x -= g->textWidth("100%") + S(6);
  int bw = S(18), bh = S(9);
  g->fillRect(x - bw, cy - bh / 2, bw, bh, TH.bar);
  if (b >= 0) {
    g->drawRect(x - bw, cy - bh / 2, bw, bh, TH.text);
    g->fillRect(x, cy - S(2), S(2), S(4), TH.text);
    g->fillRect(x - bw + 2, cy - bh / 2 + 2, (bw - 4) * b / 100, bh - 4, b < 20 ? TH.danger : TH.ok);
  }
  x -= bw + S(8);
  char padded[16]; snprintf(padded, sizeof(padded), "%11s", up);
  g->setTextColor(TH.muted, TH.bar); g->drawString(padded, x, cy);
  g->setTextColor(TH.text);
}

void UITask::drawTabBar() {
  LGFX_CYD* g = _gfx;
  int y = GH - S(L::TAB_H);
  g->fillRect(0, y, GW, S(L::TAB_H), TH.bar);
  static const char* names[5] = { "Status", "Neighbours", "Spectrum", "Stats", "Settings" };
  Screen tabs[5] = { Screen::STATUS, Screen::NEIGHBOURS, Screen::SPECTRUM, Screen::STATS, Screen::SETTINGS };
  int cy = y + S(L::TAB_H / 2), cw = GW / 5;
  for (int i = 0; i < 5; i++) {
    bool sel = (_screen == tabs[i]);
    txt(g, i * cw + cw / 2, cy, names[i], sel ? TH.accent : TH.muted, textdatum_t::middle_center, 1.0f, FONT_SMALL);
    if (sel) g->fillRect(i * cw + cw / 8, y + S(L::TAB_H - 3), cw * 3 / 4, S(2), TH.accent);
  }
}

void UITask::drawAlert() {
  if (!_gfx || !_display->isOn()) return;
  LGFX_CYD* g = _gfx;
  g->fillRect(0, 0, GW, S(L::STATUS_H), TH.accent);
  g->setClipRect(S(6), 0, GW - S(12), S(L::STATUS_H));
  txt(g, GW / 2, S(L::STATUS_H / 2), _alert, TH.bg, textdatum_t::middle_center);
  g->clearClipRect();
  _alertShown = true;
}

void UITask::drawSplash() {
  LGFX_CYD* g = _gfx;
  g->fillScreen(TH.bg);
  txt(g, GW / 2, S(70), "MeshCore", TH.accent, textdatum_t::middle_center, 1.0f, FONT_BIG);
  txt(g, GW / 2, S(100), "Repeater", TH.text, textdatum_t::middle_center, 1.0f, FONT_BIG);
  txt(g, GW / 2, S(135), _version, TH.text, textdatum_t::middle_center);
  txt(g, GW / 2, S(158), _build, TH.muted, textdatum_t::middle_center);
  txt(g, GW / 2, S(205), _prefs->node_name, TH.muted, textdatum_t::middle_center);
}

// ---------------------------------------------------------------- status

static int tileW() { return (GW - 4 * S(8)) / 3; }
static int tileX(int col) { return S(8) + col * (tileW() + S(8)); }
static void tile(LGFX_CYD* g, int col, int y, uint16_t bg, uint16_t fg, const char* big, const char* label, char* cache) {
  if (cache && strcmp(cache, big) == 0) return;
  if (cache) strncpy(cache, big, 15);
  int x = tileX(col), w = tileW();
  g->fillRoundRect(x, S(y), w, S(58), S(6), bg);
  txt(g, x + w / 2, S(y + 22), big, fg, textdatum_t::middle_center, 1.0f, FONT_BIG);
  txt(g, x + w / 2, S(y + 45), label, fg, textdatum_t::middle_center);
}

// one cached text line: repaint only when the text changed
static void lineOut(LGFX_CYD* g, int y, const char* label, const char* value, uint16_t vcol, char* cache) {
  char key[80]; snprintf(key, sizeof(key), "%s|%s", label, value);
  if (strcmp(key, cache) == 0) return;
  strncpy(cache, key, 63);
  g->fillRect(0, S(y), GW, S(26), TH.bg);
  txt(g, S(8), S(y), label, TH.muted, textdatum_t::top_left, 1.0f, FONT_SMALL);
  g->setClipRect(0, S(y + 11), GW, S(16));
  txt(g, S(8), S(y + 12), value, vcol);
  g->clearClipRect();
}

void UITask::drawStatus(bool full) {
  LGFX_CYD* g = _gfx;
  if (full) {
    g->fillScreen(TH.bg);
    drawStatusBar(true);
    drawTabBar();
    memset(_tileCache, 0, sizeof(_tileCache)); memset(_lineCache, 0, sizeof(_lineCache));
    _dirty |= D_TILES | D_LINES;
  }
  if (_dirty & D_TILES) {
    char b[16];
    snprintf(b, sizeof(b), "%lu", (unsigned long)the_mesh.getNumSentFlood());
    tile(g, 0, 28, TH.tileBlue, TH.tileBlueFg, b, "flood tx", _tileCache[0]);
    const NeighbourInfo* nb[MAX_NEIGHBOURS];
    int nn = the_mesh.getNeighbourList(nb, MAX_NEIGHBOURS);
    snprintf(b, sizeof(b), "%d", nn);
    tile(g, 1, 28, TH.tileGreen, TH.tileGreenFg, b, "neighbours", _tileCache[1]);
    snprintf(b, sizeof(b), "%d", radio_driver.getNoiseFloor());
    tile(g, 2, 28, TH.tileAmber, TH.tileAmberFg, b, "noise dBm", _tileCache[2]);
  }
  if (_dirty & D_LINES) {
    char v[64];
    // airtime budget: remaining / (window * duty_cycle)
    float af = _prefs->airtime_factor;
    unsigned long window = the_mesh.getTxBudgetWindowMs();
    unsigned long maxBudget = (unsigned long)(window / (1.0f + af));
    unsigned long left = the_mesh.getRemainingTxBudget();
    int pct = maxBudget ? (int)(left * 100UL / maxBudget) : 0;
    snprintf(v, sizeof(v), "%d%% left   tx %lus  rx %lus", pct,
      (unsigned long)(the_mesh.getTotalAirTime() / 1000), (unsigned long)(the_mesh.getReceiveAirTime() / 1000));
    lineOut(g, 92, "airtime budget", v, pct < 20 ? TH.danger : TH.text, _lineCache[0]);
    {
      static char lastBar[16] = ""; char bk[16]; snprintf(bk, sizeof(bk), "%d", pct);
      if (full || strcmp(bk, lastBar) != 0) {
        strcpy(lastBar, bk);
        int bx = S(8), by = S(120), bw = GW - S(16), bh = S(6);
        g->fillRoundRect(bx, by, bw, bh, S(3), TH.line);
        g->fillRoundRect(bx, by, bw * pct / 100, bh, S(3), pct < 20 ? TH.danger : TH.ok);
      }
    }
    const NeighbourInfo* nb[1];
    if (the_mesh.getNeighbourList(nb, 1) > 0) {
      char hex[10]; mesh::Utils::toHex(hex, nb[0]->id.pub_key, 4);
      char ago[12]; fmtAgo(nb[0]->heard_timestamp, ago, sizeof(ago));
      snprintf(v, sizeof(v), "%s  %s  RSSI %d  SNR %.1f", hex, ago, (int)radio_driver.getLastRSSI(), radio_driver.getLastSNR());
    } else {
      strcpy(v, "nothing yet");
    }
    lineOut(g, 130, "last heard", v, TH.text, _lineCache[1]);
    snprintf(v, sizeof(v), "flood %lu  direct %lu  crc %lu  queue %d",
      (unsigned long)the_mesh.getFloodDups(), (unsigned long)the_mesh.getDirectDups(),
      (unsigned long)radio_driver.getPacketsRecvErrors(), the_mesh.getTxQueueLen());
    lineOut(g, 158, "dups dropped / errors", v, TH.text, _lineCache[2]);
    uint16_t e = the_mesh.getErrFlags();
    if (e == 0) strcpy(v, "none");
    else snprintf(v, sizeof(v), "%s%s%s", (e & ERR_EVENT_FULL) ? "queue full  " : "",
      (e & ERR_EVENT_CAD_TIMEOUT) ? "cad timeout  " : "", (e & ERR_EVENT_STARTRX_TIMEOUT) ? "rx stuck" : "");
    lineOut(g, 186, "error events", v, e ? TH.warn : TH.ok, _lineCache[3]);
  }
  _dirty = 0;
}

// ---------------------------------------------------------------- neighbours

void UITask::drawNeighbours(bool full) {
  LGFX_CYD* g = _gfx;
  if (full) { g->fillScreen(TH.bg); drawStatusBar(true); drawTabBar(); _dirty |= D_LIST; }
  if (!(_dirty & D_LIST)) return;
  int top = S(L::STATUS_H), bottom = GH - S(L::TAB_H);
  int rows = (bottom - top) / S(NB_ROW_H);
  g->fillRect(0, top, GW, bottom - top, TH.bg);
  const NeighbourInfo* nb[MAX_NEIGHBOURS];
  int n = the_mesh.getNeighbourList(nb, MAX_NEIGHBOURS);
  if (n == 0) { txt(g, GW / 2, (top + bottom) / 2, "No neighbours heard yet", TH.muted, textdatum_t::middle_center); _dirty = 0; return; }
  for (int r = 0; r < rows; r++) {
    int i = _listScroll + r;
    if (i >= n) break;
    int y = top + r * S(NB_ROW_H);
    g->drawFastHLine(0, y + S(NB_ROW_H) - 1, GW, TH.line);
    char hex[18]; mesh::Utils::toHex(hex, nb[i]->id.pub_key, 8);
    char heard[12], adv[12]; fmtAgo(nb[i]->heard_timestamp, heard, sizeof(heard)); fmtAgo(nb[i]->advert_timestamp, adv, sizeof(adv));
    float snr = nb[i]->snr / 4.0f;
    uint16_t sc = snr >= 5 ? TH.ok : (snr >= 0 ? TH.warn : TH.danger);
    txt(g, S(8), y + S(3), hex, TH.text);
    char meta[48]; snprintf(meta, sizeof(meta), "heard %s   advert %s", heard, adv);
    txt(g, S(8), y + S(19), meta, TH.muted, textdatum_t::top_left, 1.0f, FONT_SMALL);
    char s[16]; snprintf(s, sizeof(s), "SNR %.1f", snr);
    txt(g, GW - S(8), y + S(NB_ROW_H / 2), s, sc, textdatum_t::middle_right);
  }
  if (n > rows) {
    int trackH = bottom - top, thumbH = trackH * rows / n;
    int thumbY = top + (trackH - thumbH) * _listScroll / (n - rows);
    g->fillRect(GW - S(3), thumbY, S(3), thumbH, TH.muted);
  }
  _dirty = 0;
}

// ---------------------------------------------------------------- more

// ---------------------------------------------------------------- settings (finger-friendly list)

enum : uint8_t { SID_TX = 1, SID_AF, SID_TXDELAY, SID_DTXDELAY, SID_ADVINT, SID_RXBOOST, SID_SPECTRUM, SID_THEME, SID_INVERT, SID_RECAL, SID_FLOODADV, SID_REBOOT };

static const char* advIntLabel(uint8_t half_mins, char* out, size_t n) {
  if (half_mins == 0) snprintf(out, n, "off"); else snprintf(out, n, "%d min", half_mins * 2);
  return out;
}

void UITask::fillSettings(bool rebuild) {
  char v[24];
  SettingsList& L_ = _settings;
  if (rebuild) L_.clear();
  auto step = [&](uint8_t id, const char* label, const char* val) { if (rebuild) L_.stepper(id, label, val); else L_.setValue(id, val); };
  auto btn  = [&](uint8_t id, const char* label, const char* cap, bool danger = false) { if (rebuild) L_.button(id, label, cap, danger); else L_.setValue(id, cap); };
  if (rebuild) L_.header("radio  (hold BOOT to edit)");
  snprintf(v, sizeof(v), "%d dBm", _prefs->tx_power_dbm);                step(SID_TX, "TX power", v);
  snprintf(v, sizeof(v), "%.1f", _prefs->airtime_factor);                 step(SID_AF, "Airtime factor", v);
  snprintf(v, sizeof(v), "%.1f", _prefs->tx_delay_factor);                step(SID_TXDELAY, "TX delay factor", v);
  snprintf(v, sizeof(v), "%.1f", _prefs->direct_tx_delay_factor);         step(SID_DTXDELAY, "Direct TX delay", v);
  step(SID_ADVINT, "Advert every", advIntLabel(_prefs->advert_interval, v, sizeof(v)));
  btn(SID_RXBOOST, "RX boost gain", _prefs->rx_boosted_gain ? "on" : "off");
  if (rebuild) L_.header("display");
  btn(SID_THEME, "Theme", TH.name);
  btn(SID_INVERT, "Invert colours", _display->inverted() ? "on" : "off");
  btn(SID_RECAL, "Recalibrate touch", "Start");
  if (rebuild) L_.header("node  (hold BOOT)");
#ifdef SPECTRUM_ANALYZER
  btn(SID_SPECTRUM, "Spectrum analyser", "Open");
#endif
  btn(SID_FLOODADV, "Send flood advert", "Send");
  btn(SID_REBOOT, "Reboot", _confirmReboot ? "Confirm" : "Reboot", true);
}

void UITask::applySetting(uint8_t id, SetPart part) {
  int d = (part == PART_PLUS) ? 1 : (part == PART_MINUS ? -1 : 0);
  char cmd[48];
  bool locked = (id == SID_TX || id == SID_AF || id == SID_TXDELAY || id == SID_DTXDELAY || id == SID_ADVINT ||
                 id == SID_RXBOOST || id == SID_SPECTRUM || id == SID_FLOODADV || id == SID_REBOOT);
  if (locked && !unlocked()) return;
  switch (id) {
    case SID_TX:       snprintf(cmd, sizeof(cmd), "set tx %d", _prefs->tx_power_dbm + d); runCli(cmd); break;
    case SID_AF:       { float v = _prefs->airtime_factor + 0.5f * d; if (v < 0) v = 0; if (v > 9) v = 9; snprintf(cmd, sizeof(cmd), "set af %.1f", v); runCli(cmd); break; }
    case SID_TXDELAY:  { float v = _prefs->tx_delay_factor + 0.5f * d; if (v < 0) v = 0; snprintf(cmd, sizeof(cmd), "set txdelay %.1f", v); runCli(cmd); break; }
    case SID_DTXDELAY: { float v = _prefs->direct_tx_delay_factor + 0.5f * d; if (v < 0) v = 0; snprintf(cmd, sizeof(cmd), "set direct.txdelay %.1f", v); runCli(cmd); break; }
    case SID_ADVINT: {   // off, 60, 120, 240 min (CLI minimum is 60)
      static const int steps[4] = { 0, 60, 120, 240 };
      int cur = _prefs->advert_interval * 2, idx = 0;
      for (int i = 0; i < 4; i++) if (steps[i] <= cur) idx = i;
      idx += d; if (idx < 0) idx = 0; if (idx > 3) idx = 3;
      snprintf(cmd, sizeof(cmd), "set advert.interval %d", steps[idx]); runCli(cmd); break;
    }
    case SID_RXBOOST:  snprintf(cmd, sizeof(cmd), "set radio.rxgain %s", _prefs->rx_boosted_gain ? "off" : "on"); runCli(cmd); break;
#ifdef SPECTRUM_ANALYZER
    case SID_SPECTRUM: enterSpectrum(); return;
#endif
    case SID_THEME:    _themeIdx = (_themeIdx + 1) % THEME_COUNT; g_theme = &THEMES[_themeIdx]; saveTheme(); _fullRedraw = true; return;
    case SID_INVERT:   _display->setInverted(!_display->inverted()); break;
    case SID_RECAL:    _display->touchCalibrate(true); _fullRedraw = true; return;
    case SID_FLOODADV: the_mesh.sendSelfAdvertisement(1000, true); showAlert("Flood advert queued", 1200); break;
    case SID_REBOOT:   if (_confirmReboot) _board->reboot(); _confirmReboot = true; break;
    default: break;
  }
  if (id != SID_REBOOT) _confirmReboot = false;
  _dirty |= D_LIST;
}

void UITask::drawSettings(bool full) {
  LGFX_CYD* g = _gfx;
  int top = S(L::STATUS_H), bottom = GH - S(L::TAB_H);
  if (full) { g->fillScreen(TH.bg); drawStatusBar(true); drawTabBar(); fillSettings(true); _settings.draw(g, top, bottom, true); _listRelayout = false; _dirty = 0; return; }
  if (_dirty & D_LIST) { fillSettings(false); _settings.draw(g, top, bottom, _listRelayout); _listRelayout = false; }
  _dirty = 0;
}

// ---------------------------------------------------------------- stats (read-only)

void UITask::fillStats() {
  char v[40];
  _stats.clear();
  snprintf(v, sizeof(v), "%.3f SF%d BW%.1f CR%d", _prefs->freq, _prefs->sf, _prefs->bw, _prefs->cr); _stats.row("Radio", v);
  snprintf(v, sizeof(v), "%d dBm", radio_driver.getNoiseFloor()); _stats.row("Noise floor", v);
  snprintf(v, sizeof(v), "%lu / %lu", (unsigned long)radio_driver.getPacketsRecv(), (unsigned long)radio_driver.getPacketsSent()); _stats.row("Packets rx / tx", v);
  snprintf(v, sizeof(v), "%lu / %lu", (unsigned long)the_mesh.getNumRecvFlood(), (unsigned long)the_mesh.getNumSentFlood()); _stats.row("Flood rx / tx", v);
  snprintf(v, sizeof(v), "%lu / %lu", (unsigned long)the_mesh.getNumRecvDirect(), (unsigned long)the_mesh.getNumSentDirect()); _stats.row("Direct rx / tx", v);
  snprintf(v, sizeof(v), "%lu / %lu", (unsigned long)the_mesh.getFloodDups(), (unsigned long)the_mesh.getDirectDups()); _stats.row("Dups flood / direct", v);
  snprintf(v, sizeof(v), "%lu", (unsigned long)radio_driver.getPacketsRecvErrors()); _stats.row("CRC errors", v);
  snprintf(v, sizeof(v), "%lu s / %lu s", (unsigned long)(the_mesh.getTotalAirTime() / 1000), (unsigned long)(the_mesh.getReceiveAirTime() / 1000)); _stats.row("Airtime tx / rx", v);
  if (_prefs->flood_advert_interval == 0) strcpy(v, "off"); else snprintf(v, sizeof(v), "every %d h", _prefs->flood_advert_interval); _stats.row("Flood advert", v);
  snprintf(v, sizeof(v), "%d", the_mesh.getTxQueueLen()); _stats.row("TX queue", v);
  uint16_t e = the_mesh.getErrFlags();
  if (e == 0) strcpy(v, "none"); else snprintf(v, sizeof(v), "%s%s%s", (e & ERR_EVENT_FULL) ? "full " : "", (e & ERR_EVENT_CAD_TIMEOUT) ? "cad " : "", (e & ERR_EVENT_STARTRX_TIMEOUT) ? "rx-stuck" : "");
  _stats.row("Error events", v);
  snprintf(v, sizeof(v), "%u KB / %u KB", (unsigned)(ESP.getFreeHeap() / 1024), (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) / 1024)); _stats.row("Heap free / largest", v);
}

void UITask::drawStats(bool full) {
  LGFX_CYD* g = _gfx;
  int top = S(L::STATUS_H), bottom = GH - S(L::TAB_H);
  if (full) { g->fillScreen(TH.bg); drawStatusBar(true); drawTabBar(); }
  if (full || (_dirty & D_LIST)) { fillStats(); _stats.draw(g, top, bottom, full); }
  _dirty = 0;
}

#ifdef SPECTRUM_ANALYZER
// ---------------------------------------------------------------- spectrum analyser

#define SPEC_TOP    26
#define SPEC_BOTTOM 136
#define SPEC_WF_Y   140
#define SPEC_WF_ROW_PX 4
#define SPEC_MIN_DBM (-124)
#define SPEC_MAX_DBM (-54)
#define SPEC_WF_FLOOR (-118)
#define SPEC_CHUNK  26     // bins per loop() pass: ~20 ms, so touch/BLE stay responsive
#define SPEC_SETTLE_US 600

void UITask::enterSpectrum() {
  // band from the configured frequency: US 902-928, EU 863-870, else +/-13 MHz
  float f = _prefs->freq;
  if (f >= 902.0f && f <= 928.0f) { _specHz0 = 902000000UL; _specStepHz = 26000000UL / SPEC_BINS; }
  else if (f >= 863.0f && f <= 870.0f) { _specHz0 = 863000000UL; _specStepHz = 7000000UL / SPEC_BINS; }
  else { _specHz0 = (uint32_t)((f - 13.0f) * 1e6f); _specStepHz = 26000000UL / SPEC_BINS; }
  for (int i = 0; i < SPEC_BINS; i++) { _specMax[i] = SPEC_MIN_DBM; _specCur[i] = SPEC_MIN_DBM; _specH[i] = 0; _specMaxH[i] = 0; }
  memset(_specWf, 0, sizeof(_specWf));
  _specWfHead = 0; _sweeps = 0; _sweepRate = 0; _sweepRateAt = millis();
  _specPeakDbm = -200; _specPeakHz = 0; _specPos = 0;
  _specOwn = radio_driver.spectrumBegin();
  setScreen(Screen::SPECTRUM);
  if (!_specOwn) showAlert("Radio busy, try again", 1500);
}

void UITask::exitSpectrum() {
  if (_specOwn) {
    radio_driver.spectrumEnd(_prefs->freq, _prefs->bw, _prefs->sf, _prefs->cr);
    _specOwn = false;
  }
}

static int dbmToH(int16_t dbm) {
  if (dbm < SPEC_MIN_DBM) dbm = SPEC_MIN_DBM;
  if (dbm > SPEC_MAX_DBM) dbm = SPEC_MAX_DBM;
  return (int)((dbm - SPEC_MIN_DBM) * S(SPEC_BOTTOM - SPEC_TOP) / (SPEC_MAX_DBM - SPEC_MIN_DBM));
}

// waterfall colour: theme bg -> ok -> warn -> danger
static uint16_t lerp565(uint16_t a, uint16_t b, int t /*0..255*/) {
  int ar = (a >> 11) & 31, ag = (a >> 5) & 63, ab = a & 31;
  int br = (b >> 11) & 31, bg = (b >> 5) & 63, bb = b & 31;
  int r = ar + (br - ar) * t / 255, g = ag + (bg - ag) * t / 255, bl = ab + (bb - ab) * t / 255;
  return (uint16_t)((r << 11) | (g << 5) | bl);
}
static uint16_t heat(uint8_t v) {
  if (v < 85)  return lerp565(TH.bg, TH.ok, v * 3);
  if (v < 170) return lerp565(TH.ok, TH.warn, (v - 85) * 3);
  return lerp565(TH.warn, TH.danger, (v - 170) * 3);
}

void UITask::drawWaterfall() {
  LGFX_CYD* g = _gfx;
  static uint16_t lineBuf[480];
  int w = GW;
  int y = S(SPEC_WF_Y);
  for (int r = 0; r < SPEC_WF_ROWS; r++) {
    int row = (_specWfHead + SPEC_WF_ROWS - 1 - r) % SPEC_WF_ROWS;   // newest on top
    for (int x = 0; x < w; x++) lineBuf[x] = heat(_specWf[row][x * SPEC_BINS / w]);
    for (int k = 0; k < S(SPEC_WF_ROW_PX); k++) g->pushImage(0, y++, w, 1, lineBuf);
  }
}

void UITask::spectrumStep() {
  LGFX_CYD* g = _gfx;
  int plotW = GW;
  int bottom = S(SPEC_BOTTOM);
  for (int n = 0; n < SPEC_CHUNK; n++) {
    int i = _specPos;
    uint32_t hz = _specHz0 + (uint32_t)i * _specStepHz;
    int16_t dbm = radio_driver.spectrumSample(hz);
    _specPos = (i + 1) % SPEC_BINS;
    if (dbm == INT16_MIN) continue;
    _specCur[i] = dbm;
    if (dbm > _specMax[i]) _specMax[i] = dbm;
    if (dbm > _specPeakDbm) { _specPeakDbm = dbm; _specPeakHz = hz; }
    uint8_t h = dbmToH(dbm), mh = dbmToH(_specMax[i]);
    if (h == _specH[i] && mh == _specMaxH[i]) continue;   // unchanged column: no repaint
    int x0 = i * plotW / SPEC_BINS, x1 = (i + 1) * plotW / SPEC_BINS;
    int w = x1 - x0; if (w < 1) w = 1;
    // repaint only the span between old and new tops
    int oldTop = bottom - (_specH[i] > _specMaxH[i] ? _specH[i] : _specMaxH[i]) - 2;
    int newTop = bottom - (h > mh ? h : mh) - 2;
    int top = (oldTop < newTop ? oldTop : newTop); if (top < S(SPEC_TOP)) top = S(SPEC_TOP);
    g->fillRect(x0, top, w, bottom - top, TH.bg);
    if (h > 0) g->fillRect(x0, bottom - h, w, h, TH.tileGreen);
    if (h > 1) g->fillRect(x0, bottom - h, w, 1, TH.ok);            // trace edge
    if (mh > 0) g->fillRect(x0, bottom - mh - 1, w, 1, TH.accent);    // max hold
    _specH[i] = h; _specMaxH[i] = mh;
  }
  if (_specPos == 0) {   // sweep complete
    _sweeps++;
#ifdef UI_HEAP_LOG
    static unsigned long nextSpecLog = 0;
    if (millis() > nextSpecLog) {
      nextSpecLog = millis() + 2000;
      Serial.printf("spec: bin0=%d bin52=%d bin103=%d peak=%d@%lu rate=%u\n", _specCur[0], _specCur[52], _specCur[103],
        _specPeakDbm, (unsigned long)_specPeakHz, _sweepRate);
    }
#endif
    uint8_t* wf = _specWf[_specWfHead];
    for (int i = 0; i < SPEC_BINS; i++) {
      int v = (_specCur[i] - SPEC_WF_FLOOR) * 255 / 48;   // -118 dBm = black, -70 dBm = red
      wf[i] = v < 0 ? 0 : (v > 255 ? 255 : v);
    }
    _specWfHead = (_specWfHead + 1) % SPEC_WF_ROWS;
    drawWaterfall();
    unsigned long now = millis();
    if (now - _sweepRateAt >= 1000) { _sweepRate = _sweeps; _sweeps = 0; _sweepRateAt = now; }
    char hdr[48];
    if (_specPeakHz) snprintf(hdr, sizeof(hdr), "%.3f MHz %d dBm  %u/s", _specPeakHz / 1e6f, _specPeakDbm, _sweepRate);
    else strcpy(hdr, "sweeping");
    g->fillRect(S(90), 0, (GW - S(90)), S(L::STATUS_H), TH.bar);
    txt(g, (GW - S(8)), S(L::STATUS_H / 2), hdr, TH.text, textdatum_t::middle_right);
  }
}

void UITask::drawSpectrum(bool full) {
  if (!full) return;
  LGFX_CYD* g = _gfx;
  g->fillScreen(TH.bg);
  g->fillRect(0, 0, GW, S(L::STATUS_H), TH.bar);
  txt(g, S(8), S(L::STATUS_H / 2), "<", TH.accent, textdatum_t::middle_left);
  txt(g, S(24), S(L::STATUS_H / 2), "Spectrum", TH.text, textdatum_t::middle_left);
  for (int d = SPEC_MIN_DBM + 14; d < SPEC_MAX_DBM; d += 20) {
    int y = S(SPEC_BOTTOM) - dbmToH(d);
    g->drawFastHLine(0, y, GW, TH.line);
    char lb[8]; snprintf(lb, sizeof(lb), "%d", d);
    txt(g, S(2), y - S(2), lb, TH.muted, textdatum_t::bottom_left);
  }
  char f0[12], f1[12], f2[12];
  snprintf(f0, sizeof(f0), "%.1f", _specHz0 / 1e6f);
  snprintf(f1, sizeof(f1), "%.1f", (_specHz0 + _specStepHz * (SPEC_BINS / 2)) / 1e6f);
  snprintf(f2, sizeof(f2), "%.1f", (_specHz0 + _specStepHz * SPEC_BINS) / 1e6f);
  int ly = S(SPEC_WF_Y + SPEC_WF_ROWS * SPEC_WF_ROW_PX + 4);
  txt(g, S(2), ly, f0, TH.muted);
  txt(g, (GW / 2), ly, f1, TH.muted, textdatum_t::top_center);
  txt(g, (GW - S(2)), ly, f2, TH.muted, textdatum_t::top_right);
  txt(g, (GW / 2), (GH - S(12)), "mesh paused  |  tap plot: reset max hold", TH.muted, textdatum_t::middle_center);
  for (int i = 0; i < SPEC_BINS; i++) { _specH[i] = 0; _specMaxH[i] = 0; }
  drawWaterfall();
}
#endif


// ---------------------------------------------------------------- input

void UITask::pollTouch() {
  unsigned long now = millis();
  if (now < _tNextPoll) return;
  _tNextPoll = now + 15;
  int x, y;
  bool down = _display->touchedNative(&x, &y);
  if (down && !_tDown) {
    _tDown = true; _tx0 = _txLast = x; _ty0 = _tyLast = y; _tDownAt = now;
    _tLongFired = false; _tDragging = false;
    if (!_display->isOn()) { _display->turnOn(); _fullRedraw = true; _tLongFired = true; }
    return;
  }
  if (down && _tDown) {
    int dy = y - _tyLast;
    if (!_tDragging && abs(y - _ty0) > S(TAP_SLOP)) _tDragging = true;
    if (_tDragging) { onDrag(dy); _txLast = x; _tyLast = y; }
    else if (!_tLongFired && now - _tDownAt >= LONG_PRESS_MILLIS) { _tLongFired = true; onLongPress(_tx0, _ty0); }
    return;
  }
  if (!down && _tDown) {
    _tDown = false;
    if (!_tDragging && !_tLongFired) onTap(_tx0, _ty0);
  }
}

void UITask::onDrag(int dy) {
  static int acc = 0;
  acc += dy;
  int top = S(L::STATUS_H), bottom = GH - S(L::TAB_H);
  if (_screen == Screen::NEIGHBOURS) {
    int rowH = S(NB_ROW_H);
    int rows = (bottom - top) / rowH;
    const NeighbourInfo* nb[MAX_NEIGHBOURS]; int count = the_mesh.getNeighbourList(nb, MAX_NEIGHBOURS);
    int maxScroll = count > rows ? count - rows : 0;
    while (acc <= -rowH) { acc += rowH; if (_listScroll < maxScroll) { _listScroll++; _dirty |= D_LIST; } }
    while (acc >= rowH)  { acc -= rowH; if (_listScroll > 0) { _listScroll--; _dirty |= D_LIST; } }
  } else if (_screen == Screen::SETTINGS) {
    int rowH = S(SettingsList::ROW_H);
    while (acc <= -rowH) { acc += rowH; if (_settings.scrollBy(1, top, bottom)) { _listRelayout = true; _dirty |= D_LIST; } }
    while (acc >= rowH)  { acc -= rowH; if (_settings.scrollBy(-1, top, bottom)) { _listRelayout = true; _dirty |= D_LIST; } }
  } else if (_screen == Screen::STATS) {
    int rowH = S(StatsList::ROW_H);
    while (acc <= -rowH) { acc += rowH; if (_stats.scrollBy(1, top, bottom)) _dirty |= D_LIST; }
    while (acc >= rowH)  { acc -= rowH; if (_stats.scrollBy(-1, top, bottom)) _dirty |= D_LIST; }
  } else {
    acc = 0;
  }
}

void UITask::onLongPress(int x, int y) {
  if (y < S(L::STATUS_H) && x < S(110)) {   // status-bar name: screenshot over serial
    _display->dumpScreenshot(Serial);
    return;
  }
  if (_screen == Screen::NEIGHBOURS && y >= S(L::STATUS_H) && y < GH - S(L::TAB_H)) {
    int row = _listScroll + (y - S(L::STATUS_H)) / S(NB_ROW_H);
    const NeighbourInfo* nb[MAX_NEIGHBOURS];
    int n = the_mesh.getNeighbourList(nb, MAX_NEIGHBOURS);
    if (row < n) {
      if (!unlocked()) return;
      the_mesh.removeNeighbor(nb[row]->id.pub_key, PUB_KEY_SIZE);
      showAlert("Neighbour forgotten", 1000);
      _dirty |= D_LIST;
    }
  }
}

void UITask::onTap(int x, int y) {
  if (_screen == Screen::SPLASH) { setScreen(Screen::STATUS); return; }
  if (_screen != Screen::SPECTRUM && y >= GH - S(L::TAB_H)) {
    int t = x / (GW / 5);
    if (t == 0 && _screen != Screen::STATUS) setScreen(Screen::STATUS);
    else if (t == 1 && _screen != Screen::NEIGHBOURS) setScreen(Screen::NEIGHBOURS);
    else if (t == 2) {
#ifdef SPECTRUM_ANALYZER
      if (unlocked()) enterSpectrum();
#endif
    }
    else if (t == 3 && _screen != Screen::STATS) setScreen(Screen::STATS);
    else if (t == 4 && _screen != Screen::SETTINGS) setScreen(Screen::SETTINGS);
    return;
  }
  switch (_screen) {
    case Screen::SETTINGS: {
      int top = S(L::STATUS_H), bottom = GH - S(L::TAB_H);
      SetPart part;
      int id = _settings.hit(x, y, top, bottom, &part);
      if (id < 0) { if (_confirmReboot) { _confirmReboot = false; _dirty |= D_LIST; } break; }
      _settings.flash(_gfx, top, bottom, (uint8_t)id, part);
      applySetting((uint8_t)id, part);
      if (!_fullRedraw && _screen == Screen::SETTINGS) _settings.flash(_gfx, top, bottom, (uint8_t)id, PART_NONE);
      break;
    }
    case Screen::STATS: break;
    case Screen::NEIGHBOURS: break;
    case Screen::STATUS: {
      if (y >= S(28) && y < S(86)) { int col = x / (GW / 3); if (col == 1) setScreen(Screen::NEIGHBOURS); }
      break;
    }
#ifdef SPECTRUM_ANALYZER
    case Screen::SPECTRUM: {
      if (y < S(L::STATUS_H)) { setScreen(Screen::STATUS); break; }
      for (int i = 0; i < SPEC_BINS; i++) _specMax[i] = SPEC_MIN_DBM;
      _specPeakDbm = -200; _specPeakHz = 0;
      _fullRedraw = true;
      break;
    }
#endif
    default: break;
  }
}

// ---------------------------------------------------------------- main loop

void UITask::loop() {
  if (!_gfx) return;
  unsigned long now = millis();

#ifdef SPECTRUM_ANALYZER
  if (_screen == Screen::SPECTRUM && _specOwn && !_fullRedraw && _display->isOn()) spectrumStep();
#endif

  MeshLock lock;

#ifdef PIN_USER_BTN
  int ev = user_btn.check();
  if (ev == BUTTON_EVENT_LONG_PRESS && _screen != Screen::SETTINGS && _screen != Screen::SPECTRUM) setScreen(Screen::STATUS);
#endif

  pollTouch();

  if (_screen == Screen::SPLASH && now >= _splash_until) setScreen(Screen::STATUS);

  if (now >= _next_tick) {
    _next_tick = now + TICK_MILLIS;
    if (_display->isOn() && !_fullRedraw && _screen != Screen::SPLASH && _screen != Screen::SPECTRUM && !_alert_expiry) drawStatusBar(false);
    if (_screen == Screen::STATUS && (now / TICK_MILLIS) % 2 == 0) _dirty |= D_TILES | D_LINES;
    if (_screen == Screen::NEIGHBOURS && (now / TICK_MILLIS) % 5 == 0) _dirty |= D_LIST;
    if ((_screen == Screen::SETTINGS || _screen == Screen::STATS) && (now / TICK_MILLIS) % 3 == 0) _dirty |= D_LIST;
#ifdef UI_HEAP_LOG
    static unsigned long next_heap_log = 0;
    if (now > next_heap_log) {
      Serial.printf("heap free=%u largest=%u min=%u ui_stack_free=%u\n", ESP.getFreeHeap(),
        heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), ESP.getMinFreeHeap(), (unsigned)uxTaskGetStackHighWaterMark(NULL));
      next_heap_log = now + 10000;
    }
#endif
  }

  if (_alert_expiry && now >= _alert_expiry) {
    _alert_expiry = 0;
    if (_alertShown && _display->isOn()) {
      _alertShown = false;
      if (_screen == Screen::SPECTRUM) _fullRedraw = true; else if (_screen != Screen::SPLASH) drawStatusBar(true);
    }
  }

  if (_display->isOn() && (_fullRedraw || _dirty)) {
    bool full = _fullRedraw; _fullRedraw = false;
    switch (_screen) {
      case Screen::SPLASH:     if (full) drawSplash(); _dirty = 0; break;
      case Screen::STATUS:     drawStatus(full); break;
      case Screen::NEIGHBOURS: drawNeighbours(full); break;
      case Screen::SETTINGS:   drawSettings(full); break;
      case Screen::STATS:      drawStats(full); break;
#ifdef SPECTRUM_ANALYZER
      case Screen::SPECTRUM:   drawSpectrum(full); _dirty = 0; break;
#endif
      default: _dirty = 0; break;
    }
    if (_alert_expiry && now < _alert_expiry && full) drawAlert();
  }
}
