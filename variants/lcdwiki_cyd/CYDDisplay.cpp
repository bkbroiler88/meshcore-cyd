#include "CYDDisplay.h"
#include <Preferences.h>

#ifndef PIN_USER_BTN
  #define PIN_USER_BTN 0
#endif

static const char* kCalNamespace = "touchcal";
static const char* kCalKey = "cal_ls";   // landscape; a portrait option would use "cal_pt"

void CYDDisplay::loadInversion() {
  Preferences p;
  if (p.begin("uicyd", true)) {
    _inverted = p.getBool("inv", (CYD_PANEL_INVERT != 0));
    p.end();
  }
  disp.invertDisplay(_inverted);
}

void CYDDisplay::setInverted(bool on, bool save) {
  _inverted = on;
  disp.invertDisplay(on);
  if (save) {
    Preferences p;
    if (p.begin("uicyd", false)) { p.putBool("inv", on); p.end(); }
  }
}

#ifdef UI_CYD
bool CYDDisplay::begin() {
  disp.init();
  disp.setRotation(1);
  disp.setColorDepth(16);
  disp.setBrightness(200);
  loadInversion();
  _isOn = true;
  touchCalibrate(recalibrationRequested());
  return true;
}
void CYDDisplay::turnOn() {
  if (!_isOn) { disp.wakeup(); disp.setBrightness(200); }
  _isOn = true;
}
void CYDDisplay::turnOff() {
  if (_isOn) { disp.setBrightness(0); disp.sleep(); }
  _isOn = false;
}
void CYDDisplay::clear() { disp.fillScreen(TFT_BLACK); }
void CYDDisplay::startFrame(ColorVal bkg) {
  disp.fillScreen(bkg);
  disp.setTextColor(_color = UIColor::primary_txt);
  disp.setFont(&fonts::Font2);
  disp.setTextSize(1);
  disp.setTextDatum(textdatum_t::top_left);
}
void CYDDisplay::setTextSize(int sz) { disp.setTextSize(sz); }
void CYDDisplay::setColor(ColorVal c) { disp.setTextColor(_color = c); }
void CYDDisplay::setCursor(int x, int y) { disp.setCursor(x, y); }
void CYDDisplay::print(const char* str) { disp.print(str); }
void CYDDisplay::fillRect(int x, int y, int w, int h) { disp.fillRect(x, y, w, h, _color); }
void CYDDisplay::drawRect(int x, int y, int w, int h) { disp.drawRect(x, y, w, h, _color); }
void CYDDisplay::drawXbm(int x, int y, const uint8_t* bits, int w, int h) { disp.drawXBitmap(x, y, bits, w, h, _color); }
uint16_t CYDDisplay::getTextWidth(const char* str) { return disp.textWidth(str); }
#else
bool CYDDisplay::begin() {
  if (!LGFXDisplay::begin()) return false;
  disp.setBrightness(200);   // base class sets 64, too dim for these panels
  touchCalibrate(recalibrationRequested());
  return true;
}
#endif

bool CYDDisplay::loadCalibration(uint16_t cal[8]) {
  Preferences p;
  if (!p.begin(kCalNamespace, true)) return false;
  size_t got = p.getBytes(kCalKey, cal, sizeof(uint16_t) * 8);
  p.end();
  return got == sizeof(uint16_t) * 8;
}

void CYDDisplay::saveCalibration(const uint16_t cal[8]) {
  Preferences p;
  if (p.begin(kCalNamespace, false)) {
    p.putBytes(kCalKey, cal, sizeof(uint16_t) * 8);
    p.end();
  }
}

// Two escape hatches, same as the Meshtastic port (BOOT held at boot) and the
// bulb/dongle controllers (screen pressed continuously for 1.5 s at boot).
bool CYDDisplay::recalibrationRequested() {
  pinMode(PIN_USER_BTN, INPUT_PULLUP);
  if (digitalRead(PIN_USER_BTN) == LOW) {
    Serial.println("CYD: button held at boot - forcing touch recalibration");
    return true;
  }
  unsigned long start = millis();
  while (millis() - start < 1500) {
    lgfx::touch_point_t tp;
    if (disp.getTouchRaw(&tp, 1) == 0) return false;
    delay(20);
  }
  Serial.println("CYD: screen held at boot - forcing touch recalibration");
  return true;
}

void CYDDisplay::touchCalibrate(bool force) {
  uint16_t cal[8] = {0};
  if (!force && loadCalibration(cal)) {
    disp.setTouchCalibrate(cal);
    _calibrated = true;
    Serial.println("CYD: touch calibration restored from NVS");
    return;
  }
  Serial.println("CYD: running touch calibration");
  disp.fillScreen(TFT_BLACK);
  disp.setTextColor(TFT_WHITE, TFT_BLACK);
  disp.setTextSize(2);
  disp.setCursor(10, 10);
  disp.print("Touch calibration");
  disp.setTextSize(1);
  disp.setCursor(10, 40);
  disp.print("Tap each corner marker");
  delay(1500);
  disp.calibrateTouch(cal, TFT_WHITE, TFT_BLACK, 20);
  Serial.printf("CYD: touch cal %u %u %u %u %u %u %u %u\n",
    cal[0], cal[1], cal[2], cal[3], cal[4], cal[5], cal[6], cal[7]);
  saveCalibration(cal);
  _calibrated = true;
  disp.fillScreen(TFT_BLACK);
}

bool CYDDisplay::touchedNative(int* x, int* y) {
  lgfx::touch_point_t tp;
  if (disp.getTouch(&tp, 1) == 0) return false;
  *x = tp.x;
  *y = tp.y;
  return true;
}

bool CYDDisplay::touched(int* x, int* y) {
  int nx, ny;
  if (!touchedNative(&nx, &ny)) return false;
  *x = (int)(nx / UI_ZOOM);
  *y = (int)(ny / UI_ZOOM);
  return true;
}

void CYDDisplay::dumpScreenshot(Stream& out) {
  static uint16_t line[480];
  int w = disp.width(), h = disp.height();
  out.print("\nSCREENSHOT "); out.print(w); out.print(' '); out.print(h); out.print('\n');
  out.flush();
  for (int y = 0; y < h; y++) {
    disp.readRect(0, y, w, 1, line);
    out.write((const uint8_t*)line, w * 2);
    out.flush();
  }
  out.print("\nSCREENSHOT_END\n");
  out.flush();
}
