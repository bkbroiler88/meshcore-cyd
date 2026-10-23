#pragma once

// Colour touch UI for a MeshCore REPEATER on the LCDWIKI CYD boards (variants/lcdwiki_cyd).
// Status dashboard, neighbours, spectrum analyser and a settings page. Same drawing rules
// as the companion ui-cyd: direct draw on the LovyanGFX device, repaint only what changed.
// Adjustable rows are locked unless the BOOT button is held while tapping.

#include <Arduino.h>
#include <helpers/ui/DisplayDriver.h>
#include <helpers/CommonCLI.h>
#include "CYDDisplay.h"
#include "ui_theme.h"
#include "ui_widgets.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

extern SemaphoreHandle_t ui_mesh_mutex;   // defined in main.cpp (dual-core build)
struct MeshLock {
  MeshLock()  { if (ui_mesh_mutex) xSemaphoreTakeRecursive(ui_mesh_mutex, portMAX_DELAY); }
  ~MeshLock() { if (ui_mesh_mutex) xSemaphoreGiveRecursive(ui_mesh_mutex); }
};

enum class Screen : uint8_t { SPLASH, STATUS, NEIGHBOURS, STATS, SETTINGS, SPECTRUM };

class UITask {
  mesh::MainBoard* _board;
  CYDDisplay* _display;
  LGFX_CYD* _gfx;
  NodePrefs* _prefs;
  char _version[16];
  char _build[24];

  Screen _screen = Screen::SPLASH;
  bool _fullRedraw = true;
  volatile uint32_t _dirty = 0;
  unsigned long _next_tick = 0, _splash_until = 0, _ui_started_at = 0, _alert_expiry = 0;
  char _alert[48];
  bool _alertShown = false;
  char _statusCache[48];
  int _heapCacheKb = -1;
  char _tileCache[3][16];
  char _lineCache[4][64];
  char _moreRowCache[24][48];
  int _listScroll = 0;
  int _battEma = -1;
  uint8_t _themeIdx = 0;
  bool _confirmReboot = false;

  // touch
  bool _tDown = false; unsigned long _tNextPoll = 0;
  int _tx0, _ty0, _txLast, _tyLast; unsigned long _tDownAt = 0;
  bool _tLongFired = false, _tDragging = false;

#ifdef SPECTRUM_ANALYZER
  #define SPEC_BINS 104
  #define SPEC_WF_ROWS 10
  bool _specOwn = false;
  uint32_t _specHz0 = 902000000UL, _specStepHz = 250000UL;
  int _specPos = 0;
  int16_t _specMax[SPEC_BINS];
  uint8_t _specH[SPEC_BINS];
  uint8_t _specMaxH[SPEC_BINS];
  int16_t _specPeakDbm = -200; uint32_t _specPeakHz = 0;
  int16_t _specCur[SPEC_BINS];
  uint8_t _specWf[SPEC_WF_ROWS][SPEC_BINS];
  uint8_t _specWfHead = 0;
  uint16_t _sweeps = 0, _sweepRate = 0; unsigned long _sweepRateAt = 0;
  void enterSpectrum();
  void exitSpectrum();
  void spectrumStep();
  void drawSpectrum(bool full);
  void drawWaterfall();
#endif

  void setScreen(Screen s);
  void pollTouch();
  void onTap(int x, int y);
  void onLongPress(int x, int y);
  void onDrag(int dy);
  bool unlocked();
  void runCli(const char* cmd);

  void drawStatusBar(bool force);
  void drawTabBar();
  void drawSplash();
  void drawStatus(bool full);
  void drawNeighbours(bool full);
  void drawSettings(bool full);
  void drawStats(bool full);
  void fillSettings(bool rebuild);
  void fillStats();
  void applySetting(uint8_t id, SetPart part);
  SettingsList _settings;
  StatsList _stats;
  bool _listRelayout = false;
  void drawAlert();
  void showAlert(const char* text, int ms);

  int battPercent();
  void fmtAgo(uint32_t ts, char* out, size_t n);
  void loadTheme();
  void saveTheme();

public:
  UITask(mesh::MainBoard& board, DisplayDriver& display)
    : _board(&board), _display((CYDDisplay*)&display), _gfx(NULL), _prefs(NULL) {
    _alert[0] = 0; _statusCache[0] = 0; memset(_tileCache, 0, sizeof(_tileCache));
    memset(_lineCache, 0, sizeof(_lineCache)); memset(_moreRowCache, 0, sizeof(_moreRowCache));
  }
  void begin(NodePrefs* node_prefs, const char* build_date, const char* firmware_version);
  void loop();
#ifdef SPECTRUM_ANALYZER
  bool ownsRadio() const { return _specOwn; }
#else
  bool ownsRadio() const { return false; }
#endif
};
