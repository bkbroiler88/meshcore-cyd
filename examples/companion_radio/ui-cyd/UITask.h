#pragma once

// Colour touch UI for the LCDWIKI CYD boards (variants/lcdwiki_cyd). Draws straight on
// the LovyanGFX device at native resolution; every screen repaints only the regions
// whose state changed. Same public surface as ui-new so main.cpp / MyMesh are untouched.

#include <MeshCore.h>
#include <helpers/ui/DisplayDriver.h>
#include <helpers/SensorManager.h>
#include <helpers/MultiSerialInterface.h>
#include <Arduino.h>

#include "../AbstractUITask.h"
#include "../NodePrefs.h"
#include "CYDDisplay.h"
#include "ui_widgets.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

extern SemaphoreHandle_t ui_mesh_mutex;   // defined in main.cpp (dual-core build)
struct MeshLock {
  MeshLock()  { if (ui_mesh_mutex) xSemaphoreTakeRecursive(ui_mesh_mutex, portMAX_DELAY); }
  ~MeshLock() { if (ui_mesh_mutex) xSemaphoreGiveRecursive(ui_mesh_mutex); }
};

#ifndef UI_MAX_CONVOS
  #define UI_MAX_CONVOS 8
#endif
#ifndef UI_MSGS_PER_CONVO
  #define UI_MSGS_PER_CONVO 6
#endif
#ifndef UI_MSG_TEXT
  #define UI_MSG_TEXT 80
#endif

struct UiMsg {
  uint32_t ts;
  uint32_t ack;        // expected ack crc for outgoing DMs (0 = none)
  uint8_t  flags;      // bit0 outgoing, bit1 acked, bit2 failed
  char     text[UI_MSG_TEXT];
};

struct UiConvo {
  bool     used;
  bool     isChannel;
  uint8_t  key[8];     // contact pubkey prefix, or channel idx in key[0]
  char     name[32];
  uint32_t lastActivity;
  uint8_t  unread;
  uint8_t  head, count;
  UiMsg    msgs[UI_MSGS_PER_CONVO];

  const UiMsg* at(int i) const;   // 0 = oldest
};

enum class Screen : uint8_t { SPLASH, HOME, CONTACTS, CHANNELS, CHAT, STATS, SETTINGS, SPECTRUM };

class UITask : public AbstractUITask {
  CYDDisplay* _display;
  SensorManager* _sensors;
  NodePrefs* _prefs;
  LGFX_CYD* _gfx;

  // conversations
  UiConvo _convos[UI_MAX_CONVOS];
  int _chatConvo = -1;          // index into _convos while on CHAT
  int _msgcount = 0;            // phone offline queue size

  // screen state
  Screen _screen = Screen::SPLASH;
  bool _fullRedraw = true;
  volatile uint32_t _dirty = 0; // screen-specific region bits (set from both cores, under the mutex)
  unsigned long _next_tick = 0, _auto_off = 0, _splash_until = 0, _ui_started_at = 0;
  unsigned long _alert_expiry = 0;
  char _alert[48];
  char _statusCache[48];
  int _heapCacheKb = -1;
  char _tileCache[3][16];
  char _moreCache[160];
  char _moreRowCache[16][48];
  int _battEma = -1;   // smoothed battery mV
  bool _alertShown = false;
  volatile bool _alertPending = false, _wakePending = false;

  // lists
  int _listScroll = 0;
  uint16_t _contactIdx[64];     // sorted contact slot indexes (most recent first)
  int _contactCount = 0;
  uint8_t _channelIdx[16];
  int _channelCount = 0;

  // chat compose
  char _compose[UI_MSG_TEXT];
  int _composeLen = 0;
  bool _kbVisible = true;
  uint8_t _kbMode = 0;          // 0 lower, 1 upper, 2 symbols
  int _kbFlashKey = -1;
  unsigned long _kbFlashUntil = 0;

  // more screen
  bool _confirmHibernate = false;
  uint8_t _themeIdx = 0;

  // spectrum analyser
#ifdef SPECTRUM_ANALYZER
  #define SPEC_BINS 104
  #define SPEC_WF_ROWS 10
  bool _specOwn = false;
  uint32_t _specHz0 = 902000000UL, _specStepHz = 125000UL;
  int _specPos = 0;
  int16_t _specMax[SPEC_BINS];
  uint8_t _specH[SPEC_BINS];      // drawn bar height (px) per bin
  uint8_t _specMaxH[SPEC_BINS];   // drawn max-hold height
  int16_t _specPeakDbm = -200; uint32_t _specPeakHz = 0;
  int16_t _specCur[SPEC_BINS];                 // latest sweep
  uint8_t _specWf[SPEC_WF_ROWS][SPEC_BINS];    // waterfall ring (0..255 level)
  uint8_t _specWfHead = 0;
  uint16_t _sweeps = 0, _sweepRate = 0; unsigned long _sweepRateAt = 0;
  void drawWaterfall();
  void enterSpectrum();
  void exitSpectrum();
  void spectrumStep();
  void drawSpectrum(bool full);
#endif
  void loadTheme();
  void saveTheme();

  // touch gesture state
  bool _tDown = false;
  unsigned long _tNextPoll = 0;
  int _tx0, _ty0, _txLast, _tyLast;
  unsigned long _tDownAt = 0;
  bool _tLongFired = false, _tDragging = false;

  // --- helpers
  void setScreen(Screen s);
  void pollTouch();
  void onTap(int x, int y);
  void onLongPress(int x, int y);
  void onDrag(int dy);
  void wakeDisplay();

  void drawStatusBar(bool force);
  void drawTabBar();
  void drawSplash();
  void drawHome(bool full);
  void drawContacts(bool full);
  void drawChannels(bool full);
  void drawChat(bool full);
  void drawSettings(bool full);
  void drawStats(bool full);
  void fillSettings(bool rebuild);
  void fillStats();
  void applySetting(uint8_t id, SetPart part);
  SettingsList _settings;
  StatsList _stats;
  bool _listRelayout = false;
  void drawAlert();
  void drawKeyboard();
  void drawKey(int k, bool pressed);
  void drawField();
  void drawBubbles();

  int  keyAt(int x, int y) const;         // -1 if none
  const char* keyLabel(int k, char* buf) const;
  void keyPress(int k);
  void sendCompose();

  int  findConvo(const uint8_t* key, bool isChannel);
  int  openConvo(const uint8_t* key, bool isChannel, const char* name);
  void addMsg(int ci, const char* text, bool outgoing, uint32_t ack);
  int  totalUnread() const;
  void rebuildContactList();
  void rebuildChannelList();
  void openChatForContact(int listRow);
  void openChatForChannel(int listRow);

  void fmtAgo(uint32_t ts, char* out, size_t n) const;
  int  battPercent() const;

public:
  UITask(mesh::MainBoard* board, MultiSerialInterface* serial)
    : AbstractUITask(board, serial), _display(NULL), _sensors(NULL), _prefs(NULL), _gfx(NULL) {
    memset(_convos, 0, sizeof(_convos));
    _alert[0] = 0; _statusCache[0] = 0; _compose[0] = 0;
  }
  void begin(DisplayDriver* display, SensorManager* sensors, NodePrefs* node_prefs);

  void gotoHomeScreen() { setScreen(Screen::HOME); }
  void showAlert(const char* text, int duration_millis);
  int  getMsgCount() const { return _msgcount; }
  bool hasDisplay() const { return _display != NULL; }
  bool isButtonPressed() const;
#ifdef SPECTRUM_ANALYZER
  bool ownsRadio() const { return _specOwn; }
#else
  bool ownsRadio() const { return false; }
#endif
  bool isBuzzerQuiet() { return true; }
  void toggleBuzzer() {}
  bool getGPSState();
  void toggleGPS();

  // MyMesh::Listener
  void onMessageRecv(mesh::Packet *pkt, const ContactInfo &from, uint8_t txt_type, uint32_t sender_timestamp, const char* text) override;
  void onChannelMessageRecv(mesh::Packet *pkt, ChannelDetails& channel_details, const char* text) override;
  void onQueueSizeChanged(int offline_queue_size) override;
  void onDiscoveredContact(ContactInfo &contact, bool is_new, uint8_t path_len, const uint8_t* path) override;
  void onACKRecv(uint32_t ack_crc) override;

  // AbstractUITask
  void notify(UIEventType t = UIEventType::none) override;
  void loop() override;

  void shutdown(bool restart = false);
};
