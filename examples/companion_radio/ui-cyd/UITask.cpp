#include "UITask.h"
#include "ui_theme.h"
#include "../MyMesh.h"
#include "target.h"
#include <helpers/TxtDataHelpers.h>
#include <helpers/AdvertDataHelpers.h>
#include <RTClib.h>
#include <Preferences.h>

#ifndef AUTO_OFF_MILLIS
  #define AUTO_OFF_MILLIS 60000
#endif
#define SPLASH_MILLIS      2500
#define LONG_PRESS_MILLIS  900
#define TAP_SLOP           12
#define TICK_MILLIS        1000
#define MORE_ROWS          13
#define MORE_ROW_H         21

#define MF_OUT    0x01
#define MF_ACKED  0x02
#define MF_FAILED 0x04

// dirty bits
#define D_TILES    0x01
#define D_LASTHEARD 0x02
#define D_LIST     0x01
#define D_BUBBLES  0x01
#define D_FIELD    0x02
#define D_KEYBOARD 0x04

// ---------------------------------------------------------------- themes


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

// ---------------------------------------------------------------- conversations

const UiMsg* UiConvo::at(int i) const {
  if (i < 0 || i >= count) return NULL;
  int idx = (head + UI_MSGS_PER_CONVO - count + i) % UI_MSGS_PER_CONVO;
  return &msgs[idx];
}

int UITask::findConvo(const uint8_t* key, bool isChannel) {
  for (int i = 0; i < UI_MAX_CONVOS; i++) {
    if (!_convos[i].used || _convos[i].isChannel != isChannel) continue;
    if (isChannel ? (_convos[i].key[0] == key[0]) : (memcmp(_convos[i].key, key, 8) == 0)) return i;
  }
  return -1;
}

int UITask::openConvo(const uint8_t* key, bool isChannel, const char* name) {
  int ci = findConvo(key, isChannel);
  if (ci >= 0) {
    strncpy(_convos[ci].name, name, sizeof(_convos[ci].name) - 1);
    return ci;
  }
  // free slot, else evict least recently active
  int victim = -1;
  uint32_t oldest = 0xFFFFFFFF;
  for (int i = 0; i < UI_MAX_CONVOS; i++) {
    if (!_convos[i].used) { victim = i; break; }
    if (_convos[i].lastActivity < oldest) { oldest = _convos[i].lastActivity; victim = i; }
  }
  UiConvo* c = &_convos[victim];
  memset(c, 0, sizeof(*c));
  c->used = true;
  c->isChannel = isChannel;
  memcpy(c->key, key, 8);
  strncpy(c->name, name, sizeof(c->name) - 1);
  c->lastActivity = millis();
  if (_chatConvo == victim) _chatConvo = -1;
  return victim;
}

void UITask::addMsg(int ci, const char* text, bool outgoing, uint32_t ack) {
  UiConvo* c = &_convos[ci];
  UiMsg* m = &c->msgs[c->head];
  c->head = (c->head + 1) % UI_MSGS_PER_CONVO;
  if (c->count < UI_MSGS_PER_CONVO) c->count++;
  m->ts = rtc_clock.getCurrentTime();
  m->ack = ack;
  m->flags = outgoing ? MF_OUT : 0;
  StrHelper::strncpy(m->text, text, sizeof(m->text));
  c->lastActivity = millis();
}

int UITask::totalUnread() const {
  int n = 0;
  for (int i = 0; i < UI_MAX_CONVOS; i++) if (_convos[i].used) n += _convos[i].unread;
  return n;
}

// ---------------------------------------------------------------- lifecycle

void UITask::begin(DisplayDriver* display, SensorManager* sensors, NodePrefs* node_prefs) {
  _display = (CYDDisplay*)display;
  _sensors = sensors;
  _prefs = node_prefs;
  _gfx = _display ? &_display->device() : NULL;
  if (_gfx) {
    g_geo.W = _gfx->width();
    g_geo.H = _gfx->height();
    float fx = g_geo.W / 320.0f, fy = g_geo.H / 240.0f;
    g_geo.fs = fx < fy ? fx : fy;
  }
#ifdef PIN_USER_BTN
  user_btn.begin();
#endif
  loadTheme();
  _ui_started_at = millis();
  _auto_off = millis() + AUTO_OFF_MILLIS;
  _splash_until = millis() + SPLASH_MILLIS;
  if (_display) _display->turnOn();
  setScreen(Screen::SPLASH);
}

void UITask::setScreen(Screen s) {
#ifdef SPECTRUM_ANALYZER
  if (_screen == Screen::SPECTRUM && s != Screen::SPECTRUM) exitSpectrum();
#endif
  _screen = s;
  _fullRedraw = true;
  _dirty = 0;
  _listScroll = 0;
  _confirmHibernate = false;
  _statusCache[0] = 0; _heapCacheKb = -1;
  memset(_tileCache, 0, sizeof(_tileCache)); _moreCache[0] = 0;
  if (s == Screen::CONTACTS) rebuildContactList();
  if (s == Screen::CHANNELS) rebuildChannelList();
  if (s == Screen::CHAT && _chatConvo >= 0) _convos[_chatConvo].unread = 0;
}

void UITask::showAlert(const char* text, int duration_millis) {
  StrHelper::strncpy(_alert, text, sizeof(_alert));
  _alert_expiry = millis() + duration_millis;
  _alertPending = true;   // painted by the UI loop on core 1
}

void UITask::notify(UIEventType t) { }

bool UITask::isButtonPressed() const {
#ifdef PIN_USER_BTN
  return user_btn.isPressed();
#else
  return false;
#endif
}

void UITask::shutdown(bool restart) {
  if (restart) {
    _board->reboot();
  } else {
    if (_gfx) _gfx->fillScreen(TFT_BLACK);
    _board->powerOff();
  }
}

void UITask::wakeDisplay() {
  if (_display && !_display->isOn()) {
    _display->turnOn();
    _fullRedraw = true;
  }
  _auto_off = millis() + AUTO_OFF_MILLIS;
}

// ---------------------------------------------------------------- mesh events

void UITask::onMessageRecv(mesh::Packet *pkt, const ContactInfo &from, uint8_t txt_type, uint32_t sender_timestamp, const char* text) {
  if (!(txt_type == TXT_TYPE_PLAIN || txt_type == TXT_TYPE_SIGNED_PLAIN)) return;
  int ci = openConvo(from.id.pub_key, false, from.name);
  addMsg(ci, text, false, 0);
  if (_screen == Screen::CHAT && _chatConvo == ci) {
    _dirty |= D_BUBBLES;
  } else {
    _convos[ci].unread++;
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "%s: %s", from.name, text);
    _dirty |= D_TILES | D_LIST;
    showAlert(tmp, 2500);
  }
  if (!hasConnection()) _wakePending = true;
}

void UITask::onChannelMessageRecv(mesh::Packet *pkt, ChannelDetails& channel_details, const char* text) {
  int idx = the_mesh.findChannelIdx(channel_details.channel);
  uint8_t key[8] = {0};
  key[0] = (idx < 0) ? 0xFF : (uint8_t)idx;
  int ci = openConvo(key, true, channel_details.name);
  addMsg(ci, text, false, 0);
  if (_screen == Screen::CHAT && _chatConvo == ci) {
    _dirty |= D_BUBBLES;
  } else {
    _convos[ci].unread++;
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "#%s: %s", channel_details.name, text);
    _dirty |= D_TILES | D_LIST;
    showAlert(tmp, 2500);
  }
  if (!hasConnection()) _wakePending = true;
}

void UITask::onQueueSizeChanged(int msgcount) {
  _msgcount = msgcount;
  _dirty |= D_TILES;
}

void UITask::onDiscoveredContact(ContactInfo &contact, bool is_new, uint8_t path_len, const uint8_t* path) {
  if (_screen == Screen::CONTACTS) rebuildContactList();
  _dirty |= D_TILES | D_LASTHEARD | D_LIST;
}

void UITask::onACKRecv(uint32_t ack_crc) {
  for (int i = 0; i < UI_MAX_CONVOS; i++) {
    UiConvo* c = &_convos[i];
    if (!c->used) continue;
    for (int k = 0; k < UI_MSGS_PER_CONVO; k++) {
      if ((c->msgs[k].flags & MF_OUT) && c->msgs[k].ack == ack_crc && ack_crc != 0) {
        c->msgs[k].flags |= MF_ACKED;
        if (_screen == Screen::CHAT && _chatConvo == i) _dirty |= D_BUBBLES;
        return;
      }
    }
  }
}

// ---------------------------------------------------------------- lists

void UITask::rebuildContactList() {
  _contactCount = 0;
  uint32_t mods[64];
  int total = the_mesh.getTotalContactSlots();
  ContactInfo tmp;
  for (int idx = MAX_ANON_CONTACTS; idx < total; idx++) {
    if (!the_mesh.getContactByIdx(idx, tmp)) continue;
    if (tmp.type == ADV_TYPE_NONE) continue;
    uint32_t m = tmp.lastmod;
    // insertion into descending-lastmod array, capped at 64
    int pos = _contactCount;
    while (pos > 0 && mods[pos - 1] < m) pos--;
    if (pos >= 64) continue;
    int end = (_contactCount < 64) ? _contactCount : 63;
    for (int j = end; j > pos; j--) { mods[j] = mods[j - 1]; _contactIdx[j] = _contactIdx[j - 1]; }
    mods[pos] = m;
    _contactIdx[pos] = (uint16_t)idx;
    if (_contactCount < 64) _contactCount++;
  }
}

void UITask::rebuildChannelList() {
  _channelCount = 0;
  ChannelDetails ch;
  for (int i = 0; i < MAX_GROUP_CHANNELS && _channelCount < 16; i++) {
    if (the_mesh.getChannel(i, ch) && ch.name[0]) _channelIdx[_channelCount++] = (uint8_t)i;
  }
}

void UITask::openChatForContact(int row) {
  if (row < 0 || row >= _contactCount) return;
  ContactInfo c;
  if (!the_mesh.getContactByIdx(_contactIdx[row], c)) return;
  _chatConvo = openConvo(c.id.pub_key, false, c.name);
  _compose[0] = 0; _composeLen = 0; _kbVisible = true;
  setScreen(Screen::CHAT);
}

void UITask::openChatForChannel(int row) {
  if (row < 0 || row >= _channelCount) return;
  ChannelDetails ch;
  if (!the_mesh.getChannel(_channelIdx[row], ch)) return;
  uint8_t key[8] = {0};
  key[0] = _channelIdx[row];
  _chatConvo = openConvo(key, true, ch.name);
  _compose[0] = 0; _composeLen = 0; _kbVisible = true;
  setScreen(Screen::CHAT);
}

// ---------------------------------------------------------------- compose / send

static const char* KB_ROW0[3] = { "qwertyuiop", "QWERTYUIOP", "1234567890" };
static const char* KB_ROW1[3] = { "asdfghjkl",  "ASDFGHJKL",  "-/:;()$&@" };
static const char* KB_ROW2[3] = { "zxcvbnm",    "ZXCVBNM",    ".,?!'+=" };
// key ids: 0-9 row0, 10-18 row1, 19 shift, 20-26 row2, 27 backspace, 28 mode, 29 space, 30 send
#define K_SHIFT 19
#define K_BKSP  27
#define K_MODE  28
#define K_SPACE 29
#define K_SEND  30

// Keys are laid out in "steps": ten across the full width. All sizes in native pixels.
static void keyRect(int k, int& x, int& y, int& w, int& h) {
  const int m = S(2), gap = S(L::KEY_GAP);
  const int step = (GW - 2 * m) / 10;
  const int kw = step - gap;
  const int rowH = S(L::KEY_H) + S(L::KEY_GAP);
  const int kbY = GH - S(L::KB_H) + S(2);
  h = S(L::KEY_H);
  if (k <= 9)            { x = m + k * step;                       y = kbY;            w = kw; }
  else if (k <= 18)      { x = m + step / 2 + (k - 10) * step;     y = kbY + rowH;     w = kw; }
  else if (k == K_SHIFT) { x = m;                                  y = kbY + 2 * rowH; w = step + step / 2 - gap; }
  else if (k <= 26)      { x = m + step + step / 2 + (k - 20) * step; y = kbY + 2 * rowH; w = kw; }
  else if (k == K_BKSP)  { x = m + 8 * step + step / 2;            y = kbY + 2 * rowH; w = step + step / 2 - gap; }
  else if (k == K_MODE)  { x = m;                                  y = kbY + 3 * rowH; w = step + step / 2 - gap; }
  else if (k == K_SPACE) { x = m + step + step / 2;                y = kbY + 3 * rowH; w = 4 * step + step / 2 - gap; }
  else                   { x = m + 6 * step;                       y = kbY + 3 * rowH; w = 4 * step - gap; }
}

int UITask::keyAt(int x, int y) const {
  for (int k = 0; k <= K_SEND; k++) {
    int kx, ky, kw, kh;
    keyRect(k, kx, ky, kw, kh);
    if (x >= kx && x < kx + kw && y >= ky && y < ky + kh) return k;
  }
  return -1;
}

const char* UITask::keyLabel(int k, char* buf) const {
  buf[1] = 0;
  if (k <= 9)       { buf[0] = KB_ROW0[_kbMode][k]; return buf; }
  if (k <= 18)      { buf[0] = KB_ROW1[_kbMode][k - 10]; return buf; }
  if (k == K_SHIFT) return (_kbMode == 1) ? "abc" : "ABC";
  if (k <= 26)      { buf[0] = KB_ROW2[_kbMode][k - 20]; return buf; }
  if (k == K_BKSP)  return "<x";
  if (k == K_MODE)  return (_kbMode == 2) ? "abc" : "123";
  if (k == K_SPACE) return "space";
  return "send";
}

void UITask::keyPress(int k) {
  if (k == K_SHIFT) { _kbMode = (_kbMode == 1) ? 0 : 1; _dirty |= D_KEYBOARD; return; }
  if (k == K_MODE)  { _kbMode = (_kbMode == 2) ? 0 : 2; _dirty |= D_KEYBOARD; return; }
  if (k == K_BKSP)  { if (_composeLen > 0) _compose[--_composeLen] = 0; _dirty |= D_FIELD; return; }
  if (k == K_SEND)  { sendCompose(); return; }
  char lb[2];
  char ch = (k == K_SPACE) ? ' ' : keyLabel(k, lb)[0];
  if (_composeLen < UI_MSG_TEXT - 1) {
    _compose[_composeLen++] = ch;
    _compose[_composeLen] = 0;
    if (_kbMode == 1) { _kbMode = 0; _dirty |= D_KEYBOARD; }   // shift is one-shot
  }
  _dirty |= D_FIELD;
}

void UITask::sendCompose() {
  if (_composeLen == 0 || _chatConvo < 0) return;
  UiConvo* c = &_convos[_chatConvo];
  uint32_t now = rtc_clock.getCurrentTime();
  if (c->isChannel) {
    ChannelDetails ch;
    if (!the_mesh.getChannel(c->key[0], ch)) { showAlert("Channel gone", 1200); return; }
    bool ok = the_mesh.sendGroupMessage(now, ch.channel, _prefs->node_name, _compose, _composeLen);
    char tmp[UI_MSG_TEXT];
    snprintf(tmp, sizeof(tmp), "%s: %s", _prefs->node_name, _compose);
    addMsg(_chatConvo, tmp, true, 0);
    if (!ok) c->msgs[(c->head + UI_MSGS_PER_CONVO - 1) % UI_MSGS_PER_CONVO].flags |= MF_FAILED;
  } else {
    ContactInfo* ct = the_mesh.lookupContactByPubKey(c->key, 8);
    if (!ct) { showAlert("Contact not found", 1200); return; }
    uint32_t ack = 0, timeout = 0;
    int rc = the_mesh.sendMessage(*ct, now, 0, _compose, ack, timeout);
    addMsg(_chatConvo, _compose, true, ack);
    if (rc == MSG_SEND_FAILED) c->msgs[(c->head + UI_MSGS_PER_CONVO - 1) % UI_MSGS_PER_CONVO].flags |= MF_FAILED;
  }
  _compose[0] = 0; _composeLen = 0;
  _dirty |= D_BUBBLES | D_FIELD;
}

// ---------------------------------------------------------------- gps (mirrors ui-new)

bool UITask::getGPSState() {
  if (_sensors) {
    int num = _sensors->getNumSettings();
    for (int i = 0; i < num; i++)
      if (strcmp(_sensors->getSettingName(i), "gps") == 0) return !strcmp(_sensors->getSettingValue(i), "1");
  }
  return false;
}

void UITask::toggleGPS() {
  if (!_sensors) return;
  int num = _sensors->getNumSettings();
  for (int i = 0; i < num; i++) {
    if (strcmp(_sensors->getSettingName(i), "gps") == 0) {
      bool on = !strcmp(_sensors->getSettingValue(i), "1");
      _sensors->setSettingValue("gps", on ? "0" : "1");
      _prefs->gps_enabled = on ? 0 : 1;
      the_mesh.savePrefs();
      showAlert(_prefs->gps_enabled ? "GPS enabled" : "GPS disabled", 900);
      return;
    }
  }
  showAlert("No GPS detected", 1200);
}

// ---------------------------------------------------------------- formatting

void UITask::fmtAgo(uint32_t ts, char* out, size_t n) const {
  uint32_t now = rtc_clock.getCurrentTime();
  if (ts == 0 || ts > now) { snprintf(out, n, "-"); return; }
  uint32_t s = now - ts;
  if (s < 60) snprintf(out, n, "%us", (unsigned)s);
  else if (s < 3600) snprintf(out, n, "%um", (unsigned)(s / 60));
  else if (s < 86400) snprintf(out, n, "%uh", (unsigned)(s / 3600));
  else snprintf(out, n, "%ud", (unsigned)(s / 86400));
}

int UITask::battPercent() const {
  int mv = getBattMilliVolts();
  if (mv <= 0) return -1;
  UITask* self = const_cast<UITask*>(this);
  if (self->_battEma < 0) self->_battEma = mv;
  else self->_battEma += (mv - self->_battEma) / 8;   // ~8-sample smoothing
  int p = (self->_battEma - 3000) * 100 / 1200;
  p = p < 0 ? 0 : (p > 100 ? 100 : p);
  return (p / 5) * 5;   // 5 % steps: the ADC jitter must not repaint the bar every second
}

static const char* typeName(uint8_t t) {
  switch (t) {
    case ADV_TYPE_CHAT: return "chat";
    case ADV_TYPE_REPEATER: return "repeater";
    case ADV_TYPE_ROOM: return "room";
    case ADV_TYPE_SENSOR: return "sensor";
    default: return "node";
  }
}

// text helpers
static void txt(LGFX_CYD* g, int x, int y, const char* s, uint16_t color, textdatum_t datum = textdatum_t::top_left, float size = 1.0f, const lgfx::IFont* font = NULL) {
  g->setFont(font ? font : FONT_BODY);
  g->setTextSize(1);
  g->setTextColor(color);
  g->setTextDatum(datum);
  g->drawString(s, x, y);
}

// word-wrap into lines (returns count); lines point into a scratch buffer
static int wrapText(LGFX_CYD* g, const char* text, int maxW, char lines[][UI_MSG_TEXT], int maxLines) {
  g->setFont(FONT_BODY);
  g->setTextSize(1);
  int n = 0;
  const char* p = text;
  while (*p && n < maxLines) {
    char* out = lines[n];
    int len = 0;
    int lastSpace = -1;
    while (*p && len < UI_MSG_TEXT - 1) {
      out[len] = *p; out[len + 1] = 0;
      if (g->textWidth(out) > maxW) {
        if (lastSpace > 0) { out[lastSpace] = 0; p -= (len - lastSpace - 1); }
        else { out[len] = 0; }
        break;
      }
      if (*p == ' ') lastSpace = len;
      len++; p++;
    }
    if (*p == ' ') p++;
    n++;
  }
  return n;
}

// ---------------------------------------------------------------- drawing: shared

void UITask::drawStatusBar(bool force) {
  LGFX_CYD* g = _gfx;
  int cy = S(L::STATUS_H / 2);
  if (_screen == Screen::CHAT && _chatConvo >= 0) {
    UiConvo* c = &_convos[_chatConvo];
    char extra[24] = "";
    if (!c->isChannel) {
      ContactInfo* ct = the_mesh.lookupContactByPubKey(c->key, 8);
      if (ct) {
        if (ct->out_path_len == OUT_PATH_UNKNOWN) strcpy(extra, "flood");
        else if (ct->out_path_len == 0) strcpy(extra, "direct");
        else snprintf(extra, sizeof(extra), "%d hops", ct->out_path_len);
      }
    } else {
      strcpy(extra, "channel");
    }
    char line[48];
    snprintf(line, sizeof(line), "%s|%s", c->name, extra);
    if (!force && strcmp(line, _statusCache) == 0) return;
    strcpy(_statusCache, line);
    g->fillRect(0, 0, GW, S(L::STATUS_H), TH.bar);
    txt(g, S(8), cy, "<", TH.accent, textdatum_t::middle_left);
    g->setClipRect(S(24), 0, S(200), S(L::STATUS_H));
    txt(g, S(24), cy, c->name, TH.text, textdatum_t::middle_left);
    g->clearClipRect();
    txt(g, (GW - S(8)), cy, extra, TH.muted, textdatum_t::middle_right);
    return;
  }

  // --- normal screens: three independently cached regions
  if (force) {
    g->fillRect(0, 0, GW, S(L::STATUS_H), TH.bar);
    g->setClipRect(0, 0, S(118), S(L::STATUS_H));
    txt(g, S(8), cy, _prefs->node_name, TH.text, textdatum_t::middle_left);
    g->clearClipRect();
    _heapCacheKb = -1;
    _statusCache[0] = 0;
  }
  // heap pill: largest free 8-bit block in KB (what BLE and the phone sync live on)
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
  // right cluster: BLE, clock, battery
  int b = battPercent();
  uint32_t now = rtc_clock.getCurrentTime() + (int32_t)_prefs->tz_offset * 3600;
  char clock[8] = "";
  if (now > 1700000000UL) { DateTime dt(now); snprintf(clock, sizeof(clock), "%02d:%02d", dt.hour(), dt.minute()); }
  char line[32];
  snprintf(line, sizeof(line), "%c%c|%s|%d", isBluetoothEnabled() ? 'B' : '-', hasConnection() ? '+' : ' ', clock, b);
  if (strcmp(line, _statusCache) == 0) return;
  strcpy(_statusCache, line);
  // fixed slots, text drawn WITH its background colour: no clear-then-draw flash
  g->setFont(FONT_BODY); g->setTextSize(1);
  int x = (GW - S(8));
  {
    char bs[8];
    if (b >= 0) snprintf(bs, sizeof(bs), "%3d%%", b); else strcpy(bs, "    ");
    g->setTextDatum(textdatum_t::middle_right);
    g->setTextColor(TH.text, TH.bar);
    g->drawString(bs, x, cy);
    x -= g->textWidth("100%") + S(6);
    int bw = S(18), bh = S(9);
    g->fillRect(x - bw, cy - bh / 2, bw, bh, TH.bar);
    if (b >= 0) {
      g->drawRect(x - bw, cy - bh / 2, bw, bh, TH.text);
      g->fillRect(x, cy - S(2), S(2), S(4), TH.text);
      int fill = (bw - 4) * b / 100;
      g->fillRect(x - bw + 2, cy - bh / 2 + 2, fill, bh - 4, b < 20 ? TH.danger : TH.ok);
    }
    x -= bw + S(8);
  }
  {
    char cs[8]; snprintf(cs, sizeof(cs), "%5s", clock[0] ? clock : "");
    g->setTextDatum(textdatum_t::middle_right);
    g->setTextColor(TH.text, TH.bar);
    g->drawString(cs, x, cy);
    x -= g->textWidth("00:00") + S(8);
  }
  g->setTextDatum(textdatum_t::middle_right);
  g->setTextColor(isBluetoothEnabled() ? TH.accent : TH.muted, TH.bar);
  g->drawString("B", x, cy);
  g->fillCircle(x + S(3), cy - S(6), S(2), hasConnection() ? TH.ok : TH.bar);
  g->setTextColor(TH.text);
}

void UITask::drawTabBar() {
  LGFX_CYD* g = _gfx;
  int y = GH - S(L::TAB_H);
  g->fillRect(0, y, GW, S(L::TAB_H), TH.bar);
  static const char* names[5] = { "Home", "Contacts", "Channels", "Stats", "Settings" };
  Screen tabs[5] = { Screen::HOME, Screen::CONTACTS, Screen::CHANNELS, Screen::STATS, Screen::SETTINGS };
  int cy = y + S(L::TAB_H / 2), cw = GW / 5;
  for (int i = 0; i < 5; i++) {
    bool sel = (_screen == tabs[i]);
    txt(g, i * cw + cw / 2, cy, names[i], sel ? TH.accent : TH.muted, textdatum_t::middle_center, 1.0f, FONT_SMALL);
    if (sel) g->fillRect(i * cw + cw / 8, y + S(L::TAB_H - 3), cw * 3 / 4, S(2), TH.accent);
  }
  if (totalUnread() > 0) g->fillCircle(cw + cw / 2 + S(22), cy - S(7), S(4), TH.danger);
}

void UITask::drawAlert() {
  if (!_gfx || !_display || !_display->isOn()) return;
  LGFX_CYD* g = _gfx;
  g->fillRect(0, 0, GW, S(L::STATUS_H), TH.accent);
  g->setClipRect(S(6), 0, (GW - S(12)), S(L::STATUS_H));
  txt(g, (GW / 2), S(L::STATUS_H / 2), _alert, TH.bg, textdatum_t::middle_center);
  g->clearClipRect();
  _alertShown = true;
}

// ---------------------------------------------------------------- drawing: screens

void UITask::drawSplash() {
  LGFX_CYD* g = _gfx;
  g->fillScreen(TH.bg);
  txt(g, (GW / 2), S(80), "MeshCore", TH.accent, textdatum_t::middle_center, 1.0f, FONT_BIG);
  const char* ver = FIRMWARE_VERSION;
  txt(g, (GW / 2), S(120), ver, TH.text, textdatum_t::middle_center);
  txt(g, (GW / 2), S(145), FIRMWARE_BUILD_DATE, TH.muted, textdatum_t::middle_center);
  txt(g, (GW / 2), S(200), "LCDWIKI CYD", TH.muted, textdatum_t::middle_center);
}

static int tileW() { return (GW - 4 * S(8)) / 3; }
static int tileX(int col) { return S(8) + col * (tileW() + S(8)); }
static void tile(LGFX_CYD* g, int col, int y, uint16_t bg, uint16_t fg, const char* big, const char* label, char* cache) {
  if (cache && strcmp(cache, big) == 0) return;
  if (cache) strncpy(cache, big, 15);
  int x = tileX(col), w = tileW();
  g->fillRoundRect(x, S(y), w, S(62), S(6), bg);
  txt(g, x + w / 2, S(y + 24), big, fg, textdatum_t::middle_center, 1.0f, FONT_BIG);
  txt(g, x + w / 2, S(y + 48), label, fg, textdatum_t::middle_center);
}
static void actionTile(LGFX_CYD* g, int col, int y, const char* label) {
  int x = tileX(col), w = tileW();
  g->fillRoundRect(x, S(y), w, S(62), S(6), TH.bar);
  txt(g, x + w / 2, S(y + 31), label, TH.accent, textdatum_t::middle_center);
}

void UITask::drawHome(bool full) {
  LGFX_CYD* g = _gfx;
  if (full) {
    g->fillScreen(TH.bg);
    drawStatusBar(true);
    drawTabBar();
    memset(_tileCache, 0, sizeof(_tileCache));
    actionTile(g, 0, 100, "Advert");
    actionTile(g, 1, 100, "Discover");
    actionTile(g, 2, 100, "Radio");
    _dirty |= D_TILES | D_LASTHEARD;
  }
  if (_dirty & D_TILES) {
    char b[16];
    snprintf(b, sizeof(b), "%d", totalUnread());   // device-side unread only; the phone queue (_msgcount) drains on sync, not on reading here
    tile(g, 0, 30, TH.tileBlue, TH.tileBlueFg, b, "unread", _tileCache[0]);
    snprintf(b, sizeof(b), "%d", the_mesh.getNumContacts());
    tile(g, 1, 30, TH.tileGreen, TH.tileGreenFg, b, "contacts", _tileCache[1]);
    snprintf(b, sizeof(b), "%d", radio_driver.getNoiseFloor());
    tile(g, 2, 30, TH.tileAmber, TH.tileAmberFg, b, "noise dBm", _tileCache[2]);
  }
  if (_dirty & D_LASTHEARD) {
    static char lastLine[64] = "";
    AdvertPath recent[1];
    the_mesh.getRecentlyHeard(recent, 1);
    char line[64];
    if (recent[0].name[0]) {
      char ago[12]; fmtAgo(recent[0].recv_timestamp, ago, sizeof(ago));
      snprintf(line, sizeof(line), "%s  %s  SNR %.1f", recent[0].name, ago, radio_driver.getLastSNR());
    } else {
      strcpy(line, "nothing yet");
    }
    if (full || strcmp(line, lastLine) != 0) {
      strcpy(lastLine, line);
      g->fillRect(0, S(168), GW, S(42), TH.bg);
      txt(g, S(8), S(172), "last heard", TH.muted);
      g->setClipRect(0, S(186), GW, S(22));
      txt(g, S(8), S(190), line, recent[0].name[0] ? TH.text : TH.muted);
      g->clearClipRect();
    }
  }
  _dirty = 0;
}

void UITask::drawContacts(bool full) {
  LGFX_CYD* g = _gfx;
  if (full) {
    g->fillScreen(TH.bg);
    drawStatusBar(true);
    drawTabBar();
    _dirty |= D_LIST;
  }
  if (!(_dirty & D_LIST)) return;
  int top = S(L::STATUS_H), bottom = (GH - S(L::TAB_H));
  int rows = (GH - S(L::STATUS_H) - S(L::TAB_H)) / S(L::ROW_H);
  g->fillRect(0, top, GW, bottom - top, TH.bg);
  if (_contactCount == 0) {
    txt(g, (GW / 2), (top + bottom) / 2, "No contacts yet", TH.muted, textdatum_t::middle_center);
    _dirty = 0; return;
  }
  ContactInfo c;
  for (int r = 0; r < rows; r++) {
    int i = _listScroll + r;
    if (i >= _contactCount) break;
    if (!the_mesh.getContactByIdx(_contactIdx[i], c)) continue;
    int y = top + S(r * L::ROW_H);
    g->drawFastHLine(0, y + S(L::ROW_H) - 1, GW, TH.line);
    // avatar
    uint16_t av[4] = { TH.tileBlue, TH.tileGreen, TH.tileAmber, TH.tilePurple };
    uint16_t avfg[4] = { TH.tileBlueFg, TH.tileGreenFg, TH.tileAmberFg, TH.tilePurpleFg };
    int k = c.id.pub_key[0] & 3;
    g->fillCircle(S(21), y + S(L::ROW_H / 2), S(13), av[k]);
    char ini[3] = { c.name[0] ? c.name[0] : '?', 0, 0 };
    txt(g, S(21), y + S(L::ROW_H / 2), ini, avfg[k], textdatum_t::middle_center);
    // name + meta
    g->setClipRect(S(42), y, S(190), S(L::ROW_H));
    txt(g, S(42), y + S(4), c.name, TH.text);
    char ago[12]; fmtAgo(c.lastmod, ago, sizeof(ago));
    char meta[48];
    if (c.out_path_len == OUT_PATH_UNKNOWN) snprintf(meta, sizeof(meta), "%s  %s ago", typeName(c.type), ago);
    else if (c.out_path_len == 0) snprintf(meta, sizeof(meta), "%s  direct  %s ago", typeName(c.type), ago);
    else snprintf(meta, sizeof(meta), "%s  %d hops  %s ago", typeName(c.type), c.out_path_len, ago);
    txt(g, S(42), y + S(21), meta, TH.muted);
    g->clearClipRect();
    // unread badge
    int ci = findConvo(c.id.pub_key, false);
    if (ci >= 0 && _convos[ci].unread > 0) {
      char ub[8]; snprintf(ub, sizeof(ub), "%d", _convos[ci].unread);
      g->fillRoundRect((GW - S(40)), y + S(10), S(32), S(18), S(9), TH.danger);
      txt(g, (GW - S(24)), y + S(19), ub, TH.text, textdatum_t::middle_center);
    }
  }
  // scroll hint
  if (_contactCount > rows) {
    int trackH = bottom - top;
    int thumbH = trackH * rows / _contactCount;
    int thumbY = top + (trackH - thumbH) * _listScroll / (_contactCount - rows);
    g->fillRect(GW - S(3), thumbY, S(3), thumbH, TH.muted);
  }
  _dirty = 0;
}

void UITask::drawChannels(bool full) {
  LGFX_CYD* g = _gfx;
  if (full) {
    g->fillScreen(TH.bg);
    drawStatusBar(true);
    drawTabBar();
    _dirty |= D_LIST;
  }
  if (!(_dirty & D_LIST)) return;
  int top = S(L::STATUS_H), bottom = (GH - S(L::TAB_H));
  g->fillRect(0, top, GW, bottom - top, TH.bg);
  if (_channelCount == 0) {
    txt(g, (GW / 2), (top + bottom) / 2, "No channels (add in app)", TH.muted, textdatum_t::middle_center);
    _dirty = 0; return;
  }
  ChannelDetails ch;
  for (int r = 0; r < _channelCount; r++) {
    if (!the_mesh.getChannel(_channelIdx[r], ch)) continue;
    int y = top + S(r * L::ROW_H);
    if (y + S(L::ROW_H) > bottom) break;
    g->drawFastHLine(0, y + S(L::ROW_H) - 1, GW, TH.line);
    txt(g, S(21), y + S(L::ROW_H / 2), "#", TH.accent, textdatum_t::middle_center, 1.0f, FONT_BIG);
    g->setClipRect(S(42), y, S(220), S(L::ROW_H));
    txt(g, S(42), y + S(L::ROW_H / 2), ch.name, TH.text, textdatum_t::middle_left);
    g->clearClipRect();
    uint8_t key[8] = { _channelIdx[r] };
    int ci = findConvo(key, true);
    if (ci >= 0 && _convos[ci].unread > 0) {
      char ub[8]; snprintf(ub, sizeof(ub), "%d", _convos[ci].unread);
      g->fillRoundRect((GW - S(40)), y + S(10), S(32), S(18), S(9), TH.danger);
      txt(g, (GW - S(24)), y + S(19), ub, TH.text, textdatum_t::middle_center);
    }
  }
  _dirty = 0;
}

void UITask::drawKey(int k, bool pressed) {
  LGFX_CYD* g = _gfx;
  char buf[2];
  int x, y, w, h;
  keyRect(k, x, y, w, h);
  uint16_t bg = pressed ? TH.keyDown : (k == K_SEND ? TH.tileBlue : TH.key);
  uint16_t fg = pressed ? TH.bg : (k == K_SEND ? TH.tileBlueFg : TH.text);
  g->fillRoundRect(x, y, w, h, S(3), bg);
  txt(g, x + w / 2, y + h / 2, keyLabel(k, buf), fg, textdatum_t::middle_center);
}

void UITask::drawKeyboard() {
  LGFX_CYD* g = _gfx;
  int kbY = (GH - S(L::KB_H));
  g->fillRect(0, kbY, GW, S(L::KB_H), TH.bar);
  for (int k = 0; k <= K_SEND; k++) drawKey(k, k == _kbFlashKey);
}

void UITask::drawField() {
  LGFX_CYD* g = _gfx;
  int fy = _kbVisible ? (GH - S(L::KB_H + L::FIELD_H + 2)) : (GH - S(L::FIELD_H + 2));
  int fx = S(6), fw = (GW - S(12)), fh = S(L::FIELD_H);
  static int lastFy = -1;
  if (lastFy != fy) {   // field moved (keyboard shown/hidden): draw the frame once
    g->fillRect(0, fy - S(2), GW, fh + S(4), TH.bg);
    g->fillRoundRect(fx, fy, fw, fh, S(4), TH.field);
    g->drawRoundRect(fx, fy, fw, fh, S(4), TH.line);
    lastFy = fy;
  }
  int ix = fx + S(6), iy = fy + 2, iw = fw - S(12), ih = fh - 4;
  g->setClipRect(ix, iy, iw, ih);
  g->setFont(FONT_BODY); g->setTextSize(1);
  g->setTextDatum(textdatum_t::middle_left);
  int endX;
  if (_composeLen == 0) {
    const char* hint = _kbVisible ? "Type a message" : "Tap to type";
    g->setTextColor(TH.muted, TH.field);
    g->drawString(hint, ix, fy + fh / 2);
    endX = ix + g->textWidth(hint);
  } else {
    const char* p = _compose;
    while (*p && g->textWidth(p) + S(8) > iw) p++;   // show the tail if it overflows
    char shown[UI_MSG_TEXT + 2];
    snprintf(shown, sizeof(shown), "%s|", p);
    g->setTextColor(TH.text, TH.field);
    g->drawString(shown, ix, fy + fh / 2);
    endX = ix + g->textWidth(shown);
  }
  if (endX < ix + iw) g->fillRect(endX, iy, ix + iw - endX, ih, TH.field);   // clear the tail only
  g->clearClipRect();
  g->setTextColor(TH.text);
}

void UITask::drawBubbles() {
  LGFX_CYD* g = _gfx;
  int top = S(L::STATUS_H);
  int bottom = _kbVisible ? (GH - S(L::KB_H + L::FIELD_H + 4)) : (GH - S(L::FIELD_H + 4));
  g->fillRect(0, top, GW, bottom - top, TH.bg);
  if (_chatConvo < 0) return;
  UiConvo* c = &_convos[_chatConvo];
  if (c->count == 0) {
    txt(g, (GW / 2), (top + bottom) / 2, "No messages yet", TH.muted, textdatum_t::middle_center);
    return;
  }
  const int lineH = (g_geo.fs >= 1.25f) ? S(17) : S(15), padX = S(8), padY = S(4), maxW = GW * 2 / 3;
  static char lines[4][UI_MSG_TEXT];
  int y = bottom - S(4);
  for (int i = c->count - 1; i >= 0 && y > top; i--) {
    const UiMsg* m = c->at(i);
    bool out = m->flags & MF_OUT;
    int n = wrapText(g, m->text, maxW - 2 * padX, lines, 4);
    int w = 0;
    for (int k = 0; k < n; k++) { int tw = g->textWidth(lines[k]); if (tw > w) w = tw; }
    w += 2 * padX;
    int h = n * lineH + 2 * padY;
    y -= h;
    if (y < top) break;
    int x = out ? (GW - S(6)) - w : S(6);
    uint16_t bg = out ? TH.bubbleOut : TH.bubbleIn;
    uint16_t fg = out ? TH.bubbleOutFg : TH.text;
    g->fillRoundRect(x, y, w, h, S(6), bg);
    for (int k = 0; k < n; k++) txt(g, x + padX, y + padY + k * lineH, lines[k], fg);
    if (out && !c->isChannel) {
      const char* mark = (m->flags & MF_FAILED) ? "x" : ((m->flags & MF_ACKED) ? "vv" : "v");
      uint16_t mc = (m->flags & MF_FAILED) ? TH.danger : ((m->flags & MF_ACKED) ? TH.accent : TH.muted);
      txt(g, x - S(4), y + h - S(2), mark, mc, textdatum_t::bottom_right);
    }
    y -= S(6);
  }
}

void UITask::drawChat(bool full) {
  if (full) {
    _gfx->fillScreen(TH.bg);
    drawStatusBar(true);
    _dirty |= D_BUBBLES | D_FIELD | D_KEYBOARD;
  }
  if (_dirty & D_KEYBOARD) { if (_kbVisible) drawKeyboard(); }
  if (_dirty & D_FIELD) drawField();
  if (_dirty & D_BUBBLES) drawBubbles();
  _dirty = 0;
}

// ---------------------------------------------------------------- settings (finger-friendly list)

enum : uint8_t { SID_TX = 1, SID_RXBOOST, SID_BT, SID_GPS, SID_TZ, SID_ADVERT, SID_SPECTRUM, SID_THEME, SID_INVERT, SID_RECAL, SID_HIBERNATE };

void UITask::fillSettings(bool rebuild) {
  char v[24];
  SettingsList& L_ = _settings;
  if (rebuild) L_.clear();
  auto step = [&](uint8_t id, const char* label, const char* val) { if (rebuild) L_.stepper(id, label, val); else L_.setValue(id, val); };
  auto btn  = [&](uint8_t id, const char* label, const char* cap, bool danger = false) { if (rebuild) L_.button(id, label, cap, danger); else L_.setValue(id, cap); };
  if (rebuild) L_.header("radio");
  snprintf(v, sizeof(v), "%d dBm", _prefs->tx_power_dbm);           step(SID_TX, "TX power", v);
  btn(SID_RXBOOST, "RX boost gain", _prefs->rx_boosted_gain ? "on" : "off");
  if (rebuild) L_.header("node");
  btn(SID_BT, "Bluetooth", isBluetoothEnabled() ? "on" : "off");
#if ENV_INCLUDE_GPS
  btn(SID_GPS, "GPS", getGPSState() ? "on" : "off");
#endif
  snprintf(v, sizeof(v), "UTC%+d", (int)_prefs->tz_offset);          step(SID_TZ, "Time zone", v);
  btn(SID_ADVERT, "Send advert", "Send");
#ifdef SPECTRUM_ANALYZER
  btn(SID_SPECTRUM, "Spectrum analyser", "Open");
#endif
  if (rebuild) L_.header("display");
  btn(SID_THEME, "Theme", TH.name);
  btn(SID_INVERT, "Invert colours", _display->inverted() ? "on" : "off");
  btn(SID_RECAL, "Recalibrate touch", "Start");
  if (rebuild) L_.header("power");
  btn(SID_HIBERNATE, "Hibernate", _confirmHibernate ? "Confirm" : "Sleep", true);
}

void UITask::applySetting(uint8_t id, SetPart part) {
  int d = (part == PART_PLUS) ? 1 : (part == PART_MINUS ? -1 : 0);
  switch (id) {
    case SID_TX: {
      int t = _prefs->tx_power_dbm + d;
      if (t < -9) t = -9; if (t > MAX_LORA_TX_POWER) t = MAX_LORA_TX_POWER;
      _prefs->tx_power_dbm = (int8_t)t; the_mesh.savePrefs(); radio_driver.setTxPower(t);
      break;
    }
    case SID_RXBOOST:
      _prefs->rx_boosted_gain = !_prefs->rx_boosted_gain; the_mesh.savePrefs();
      radio_driver.setRxBoostedGainMode(_prefs->rx_boosted_gain);
      break;
    case SID_BT: if (isBluetoothEnabled()) disableBluetooth(); else enableBluetooth(); _statusCache[0] = 0; break;
    case SID_GPS: toggleGPS(); break;
    case SID_TZ: {
      int t = _prefs->tz_offset + d; if (t < -12) t = -12; if (t > 14) t = 14;
      _prefs->tz_offset = (int8_t)t; the_mesh.savePrefs(); _statusCache[0] = 0;
      break;
    }
    case SID_ADVERT: showAlert(the_mesh.advert() ? "Advert sent" : "Advert failed", 1200); break;
#ifdef SPECTRUM_ANALYZER
    case SID_SPECTRUM: enterSpectrum(); return;
#endif
    case SID_THEME: _themeIdx = (_themeIdx + 1) % THEME_COUNT; g_theme = &THEMES[_themeIdx]; saveTheme(); _fullRedraw = true; return;
    case SID_INVERT: _display->setInverted(!_display->inverted()); break;
    case SID_RECAL: _display->touchCalibrate(true); _fullRedraw = true; return;
    case SID_HIBERNATE: if (_confirmHibernate) shutdown(); _confirmHibernate = true; break;
    default: break;
  }
  if (id != SID_HIBERNATE) _confirmHibernate = false;
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
  snprintf(v, sizeof(v), "%.1f dB / %.0f dBm", radio_driver.getLastSNR(), radio_driver.getLastRSSI()); _stats.row("Last SNR / RSSI", v);
  snprintf(v, sizeof(v), "%d", the_mesh.getNumContacts()); _stats.row("Contacts", v);
  if (isBluetoothEnabled()) snprintf(v, sizeof(v), "%u", (unsigned)the_mesh.getBLEPin()); else strcpy(v, "off"); _stats.row("Bluetooth pin", v);
#if ENV_INCLUDE_GPS
  {
    LocationProvider* lp = _sensors ? _sensors->getLocationProvider() : NULL;
    if (!getGPSState()) strcpy(v, "off");
    else if (lp && lp->isValid()) snprintf(v, sizeof(v), "%.5f %.5f", lp->getLatitude() / 1e6, lp->getLongitude() / 1e6);
    else snprintf(v, sizeof(v), "no fix, %d sats", lp ? (int)lp->satellitesCount() : 0);
    _stats.row("GPS", v);
  }
#endif
  snprintf(v, sizeof(v), "%lu / %lu", (unsigned long)radio_driver.getPacketsRecv(), (unsigned long)radio_driver.getPacketsSent()); _stats.row("Packets rx / tx", v);
  snprintf(v, sizeof(v), "%lu / %lu", (unsigned long)the_mesh.getNumRecvFlood(), (unsigned long)the_mesh.getNumSentFlood()); _stats.row("Flood rx / tx", v);
  snprintf(v, sizeof(v), "%lu / %lu", (unsigned long)the_mesh.getNumRecvDirect(), (unsigned long)the_mesh.getNumSentDirect()); _stats.row("Direct rx / tx", v);
  snprintf(v, sizeof(v), "%lu", (unsigned long)radio_driver.getPacketsRecvErrors()); _stats.row("CRC errors", v);
  snprintf(v, sizeof(v), "%lu s", (unsigned long)(radio_driver.getLastRSSI() == 0 ? 0 : 0) + (unsigned long)(millis() / 1000)); _stats.row("Uptime", v);
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
    if (!_display->isOn()) { wakeDisplay(); _tLongFired = true; /* consume this touch */ }
    return;
  }
  if (down && _tDown) {
    int dy = y - _tyLast;
    if (!_tDragging && (abs(y - _ty0) > S(TAP_SLOP))) _tDragging = true;
    if (_tDragging) { onDrag(dy); _txLast = x; _tyLast = y; }
    else if (!_tLongFired && now - _tDownAt >= LONG_PRESS_MILLIS) { _tLongFired = true; onLongPress(_tx0, _ty0); }
    return;
  }
  if (!down && _tDown) {
    _tDown = false;
    _auto_off = now + AUTO_OFF_MILLIS;
    if (!_tDragging && !_tLongFired) onTap(_tx0, _ty0);
  }
}

void UITask::onDrag(int dy) {
  static int acc = 0;
  acc += dy;
  int top = S(L::STATUS_H), bottom = GH - S(L::TAB_H);
  if (_screen == Screen::CONTACTS) {
    int rowPx = S(L::ROW_H);
    int rows = (bottom - top) / rowPx;
    int maxScroll = _contactCount > rows ? _contactCount - rows : 0;
    while (acc <= -rowPx) { acc += rowPx; if (_listScroll < maxScroll) { _listScroll++; _dirty |= D_LIST; } }
    while (acc >= rowPx)  { acc -= rowPx; if (_listScroll > 0) { _listScroll--; _dirty |= D_LIST; } }
  } else if (_screen == Screen::SETTINGS) {
    int rowPx = S(SettingsList::ROW_H);
    while (acc <= -rowPx) { acc += rowPx; if (_settings.scrollBy(1, top, bottom)) { _listRelayout = true; _dirty |= D_LIST; } }
    while (acc >= rowPx)  { acc -= rowPx; if (_settings.scrollBy(-1, top, bottom)) { _listRelayout = true; _dirty |= D_LIST; } }
  } else if (_screen == Screen::STATS) {
    int rowPx = S(StatsList::ROW_H);
    while (acc <= -rowPx) { acc += rowPx; if (_stats.scrollBy(1, top, bottom)) _dirty |= D_LIST; }
    while (acc >= rowPx)  { acc -= rowPx; if (_stats.scrollBy(-1, top, bottom)) _dirty |= D_LIST; }
  } else {
    acc = 0;
  }
}

void UITask::onLongPress(int x, int y) {
  if (y < S(L::STATUS_H) && x < S(110) && _screen != Screen::CHAT) {   // status-bar name: screenshot over serial
    _display->dumpScreenshot(Serial);
    return;
  }
  if (millis() - _ui_started_at < 8000) {
    the_mesh.enterCLIRescue();
    showAlert("CLI rescue mode", 1500);
    return;
  }
  if (_screen == Screen::CHAT) {
    // long press on the field clears it
    _compose[0] = 0; _composeLen = 0; _dirty |= D_FIELD;
  }
}

void UITask::onTap(int x, int y) {
  if (_screen == Screen::SPLASH) { setScreen(Screen::HOME); return; }

  // bottom tab bar (all screens except chat)
  if (_screen != Screen::CHAT && y >= (GH - S(L::TAB_H))) {
    int t = x / (GW / 5);
    Screen tabs[5] = { Screen::HOME, Screen::CONTACTS, Screen::CHANNELS, Screen::STATS, Screen::SETTINGS };
    if (t >= 0 && t < 5 && tabs[t] != _screen) setScreen(tabs[t]);
    return;
  }

  switch (_screen) {
    case Screen::HOME: {
      int col = x / (GW / 3);
      if (y >= S(30) && y < S(92)) {
        if (col == 0) setScreen(Screen::CONTACTS);
        else if (col == 1) setScreen(Screen::CONTACTS);
        else setScreen(Screen::STATS);
      } else if (y >= S(100) && y < S(162)) {
        if (col == 0) {
          showAlert(the_mesh.advert() ? "Advert sent" : "Advert failed", 1200);
        } else if (col == 1) {
          uint8_t cmd[6];
          cmd[0] = CTL_TYPE_NODE_DISCOVER_REQ | 1;
          cmd[1] = 0xFF;
          uint32_t tag; the_mesh.getRNG()->random((uint8_t*)&tag, 4);
          memcpy(&cmd[2], &tag, 4);
          mesh::Packet* req = the_mesh.createControlData(cmd, sizeof(cmd));
          if (req) { the_mesh.sendZeroHop(req); showAlert("Discover sent", 1200); }
          else showAlert("Discover failed", 1200);
        } else {
#ifdef SPECTRUM_ANALYZER
          enterSpectrum();
#else
          setScreen(Screen::SETTINGS);
#endif
        }
      }
      break;
    }
    case Screen::CONTACTS: {
      int row = (y - S(L::STATUS_H)) / S(L::ROW_H);
      if (y >= S(L::STATUS_H)) openChatForContact(_listScroll + row);
      break;
    }
    case Screen::CHANNELS: {
      int row = (y - S(L::STATUS_H)) / S(L::ROW_H);
      if (y >= S(L::STATUS_H)) openChatForChannel(row);
      break;
    }
    case Screen::SETTINGS: {
      int top = S(L::STATUS_H), bottom = GH - S(L::TAB_H);
      SetPart part;
      int id = _settings.hit(x, y, top, bottom, &part);
      if (id < 0) { if (_confirmHibernate) { _confirmHibernate = false; _dirty |= D_LIST; } break; }
      _settings.flash(_gfx, top, bottom, (uint8_t)id, part);
      applySetting((uint8_t)id, part);
      if (!_fullRedraw && _screen == Screen::SETTINGS) _settings.flash(_gfx, top, bottom, (uint8_t)id, PART_NONE);
      break;
    }
    case Screen::STATS: break;
#ifdef SPECTRUM_ANALYZER
    case Screen::SPECTRUM: {
      if (y < S(L::STATUS_H)) { setScreen(Screen::SETTINGS); break; }
      for (int i = 0; i < SPEC_BINS; i++) _specMax[i] = SPEC_MIN_DBM;
      _specPeakDbm = -200; _specPeakHz = 0;
      _fullRedraw = true;
      break;
    }
#endif
    case Screen::CHAT: {
      if (y < S(L::STATUS_H)) {
        if (x < S(60)) setScreen(_convos[_chatConvo].isChannel ? Screen::CHANNELS : Screen::CONTACTS);
        break;
      }
      int fieldTop = _kbVisible ? (GH - S(L::KB_H + L::FIELD_H + 2)) : (GH - S(L::FIELD_H + 2));
      if (_kbVisible && y >= (GH - S(L::KB_H))) {
        int k = keyAt(x, y);
        if (k >= 0) {
          _kbFlashKey = k; _kbFlashUntil = millis() + 100;
          drawKey(k, true);
          keyPress(k);   // mode/shift changes set D_KEYBOARD themselves
        }
      } else if (y >= fieldTop) {
        if (!_kbVisible) { _kbVisible = true; _fullRedraw = true; }
      } else {
        // bubble area: toggle keyboard to see more history
        _kbVisible = !_kbVisible; _fullRedraw = true;
      }
      break;
    }
    default: break;
  }
}

// ---------------------------------------------------------------- main loop

void UITask::loop() {
  if (!_display || !_gfx) return;
  unsigned long now = millis();

#ifdef SPECTRUM_ANALYZER
  // sampling owns the radio outright (mesh task skips the radio while ownsRadio()), so it
  // runs without the lock and the mesh task keeps pumping BLE / sensors meanwhile
  if (_screen == Screen::SPECTRUM && _specOwn && !_fullRedraw && _display->isOn()) spectrumStep();
#endif

  MeshLock lock;   // everything below may touch MyMesh / radio_driver / sensors

  if (_wakePending) { _wakePending = false; wakeDisplay(); }
  if (_alertPending) { _alertPending = false; if (_display->isOn()) drawAlert(); }

#ifdef PIN_USER_BTN
  int ev = user_btn.check();
  if (ev == BUTTON_EVENT_CLICK) {
    if (!_display->isOn()) wakeDisplay();
    else if (_screen == Screen::CHAT) setScreen(Screen::CONTACTS);
    else setScreen(Screen::HOME);
  } else if (ev == BUTTON_EVENT_LONG_PRESS) {
    onLongPress(0, 0);
  }
#endif

  pollTouch();

  if (_screen == Screen::SPLASH && now >= _splash_until) setScreen(Screen::HOME);

  if (_kbFlashKey >= 0 && now >= _kbFlashUntil) {
    int k = _kbFlashKey; _kbFlashKey = -1;
    if (_screen == Screen::CHAT && _kbVisible && !(_dirty & D_KEYBOARD)) drawKey(k, false);
  }

  if (now >= _next_tick) {
    _next_tick = now + TICK_MILLIS;
    if (_display->isOn() && !_fullRedraw && _screen != Screen::SPLASH && _screen != Screen::SPECTRUM && !_alert_expiry) drawStatusBar(false);
    if (_screen == Screen::HOME && (now / TICK_MILLIS) % 2 == 0) _dirty |= D_TILES | D_LASTHEARD;
    if ((_screen == Screen::SETTINGS || _screen == Screen::STATS) && (now / TICK_MILLIS) % 3 == 0) _dirty |= D_LIST;
#ifdef UI_HEAP_LOG
    static unsigned long next_heap_log = 0;
    if (now > next_heap_log) {
      Serial.printf("heap free=%u largest=%u min=%u ui_stack_free=%u", ESP.getFreeHeap(),
        heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), ESP.getMinFreeHeap(),
        (unsigned)uxTaskGetStackHighWaterMark(NULL));
#if ENV_INCLUDE_GPS
      {
        LocationProvider* lp = _sensors ? _sensors->getLocationProvider() : NULL;
        char gd[128] = "none";
        if (lp) lp->formatDiagnostics(gd, sizeof(gd));
        Serial.printf("  gps=%s [%s]", getGPSState() ? "on" : "off", gd);
      }
#endif
      Serial.println();
      next_heap_log = now + 10000;
    }
#endif
  }

  if (_alert_expiry && now >= _alert_expiry) {
    _alert_expiry = 0;
    if (_alertShown && _display->isOn()) {
      _alertShown = false;
      if (_screen == Screen::SPECTRUM) _fullRedraw = true;   // custom header; rare
      else if (_screen != Screen::SPLASH) drawStatusBar(true);
    }
  }

  if (_display->isOn() && (_fullRedraw || _dirty)) {
    bool full = _fullRedraw;
    _fullRedraw = false;
    switch (_screen) {
      case Screen::SPLASH:   if (full) drawSplash(); _dirty = 0; break;
      case Screen::HOME:     drawHome(full); break;
      case Screen::CONTACTS: drawContacts(full); break;
      case Screen::CHANNELS: drawChannels(full); break;
      case Screen::CHAT:     drawChat(full); break;
      case Screen::SETTINGS: drawSettings(full); break;
      case Screen::STATS:    drawStats(full); break;
#ifdef SPECTRUM_ANALYZER
      case Screen::SPECTRUM: drawSpectrum(full); _dirty = 0; break;
#endif
      default: _dirty = 0; break;
    }
    if (_alert_expiry && now < _alert_expiry && full) drawAlert();
  }

#if AUTO_OFF_MILLIS > 0
  if (_display->isOn() && now > _auto_off && !hasConnection() && _screen != Screen::SPECTRUM) _display->turnOff();
#endif
}
