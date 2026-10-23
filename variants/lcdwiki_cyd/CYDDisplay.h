#pragma once

#ifdef UI_CYD
  // direct-draw colour UI: logical == native, no sprite scaling
  #undef UI_ZOOM
  #define UI_ZOOM 1
#endif
#include <helpers/ui/LGFXDisplay.h>
#include "board_pins.h"

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

// LovyanGFX device for the LCDWIKI CYD panels. Mirrors the ST7789 / ST7796 blocks of
// meshtastic-fw/src/graphics/TFTDisplay.cpp, which are verified on these exact boards.
class LGFX_CYD : public lgfx::LGFX_Device {
#if CYD_PANEL_ST7796
  lgfx::Panel_ST7796 _panel_instance;
#else
  lgfx::Panel_ST7789 _panel_instance;
#endif
  lgfx::Bus_SPI _bus_instance;
  lgfx::Light_PWM _light_instance;
  lgfx::Touch_XPT2046 _touch_instance;

public:
  LGFX_CYD(void) {
    {
      auto cfg = _bus_instance.config();
      cfg.spi_host = HSPI_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 40000000;
      cfg.freq_read = 16000000;
#if CYD_PANEL_ST7796
      cfg.spi_3wire = false;
#else
      cfg.spi_3wire = true;
#endif
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = CYD_TFT_SCLK;
      cfg.pin_mosi = CYD_TFT_MOSI;
      cfg.pin_miso = CYD_TFT_MISO;
      cfg.pin_dc = CYD_TFT_DC;
      _bus_instance.config(cfg);
      _panel_instance.setBus(&_bus_instance);
    }
    {
      auto cfg = _panel_instance.config();
      cfg.pin_cs = CYD_TFT_CS;
      cfg.pin_rst = CYD_TFT_RST;
      cfg.pin_busy = -1;
      cfg.memory_width = CYD_NATIVE_W;
      cfg.memory_height = CYD_NATIVE_H;
      cfg.panel_width = CYD_NATIVE_W;
      cfg.panel_height = CYD_NATIVE_H;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.offset_rotation = 0;
#if CYD_PANEL_ST7796
      cfg.dummy_read_pixel = 8;
#else
      cfg.dummy_read_pixel = 9;
#endif
      cfg.dummy_read_bits = 1;
      cfg.readable = true;
      cfg.invert = (CYD_PANEL_INVERT != 0);
      cfg.rgb_order = false;
      cfg.dlen_16bit = false;
      cfg.bus_shared = (CYD_TOUCH_SHARED_BUS != 0);
      _panel_instance.config(cfg);
    }
    {
      auto cfg = _light_instance.config();
      cfg.pin_bl = CYD_TFT_BL;
      cfg.invert = false;
      cfg.freq = 44100;
      cfg.pwm_channel = 7;
      _light_instance.config(cfg);
      _panel_instance.setLight(&_light_instance);
    }
    {
      auto cfg = _touch_instance.config();
#if CYD_TOUCH_SHARED_BUS
      cfg.spi_host = HSPI_HOST;      // ride the panel SPI host, arbitrated via bus_shared
      cfg.bus_shared = true;
#else
      cfg.spi_host = -1;             // bit-bang on the dedicated touch pins
      cfg.bus_shared = false;
#endif
      cfg.pin_sclk = CYD_TOUCH_SCLK;
      cfg.pin_mosi = CYD_TOUCH_MOSI;
      cfg.pin_miso = CYD_TOUCH_MISO;
      cfg.pin_cs = CYD_TOUCH_CS;
      // Must stay -1: the driver skips the SPI read whenever this pin reads high, and
      // GPIO36 has no internal pull-up, so binding it silently kills touch.
      cfg.pin_int = -1;
      cfg.freq = 2500000;
      // Raw ADC span at the panel edges; the NVS 4-point calibration refines this.
      cfg.x_min = 300;
      cfg.x_max = 3900;
      cfg.y_min = 400;
      cfg.y_max = 3900;
      cfg.offset_rotation = 0;
      _touch_instance.config(cfg);
      _panel_instance.setTouch(&_touch_instance);
    }
    setPanel(&_panel_instance);
  }
};

class CYDDisplay : public LGFXDisplay {
  LGFX_CYD disp;
  bool _calibrated = false;
  bool _inverted = (CYD_PANEL_INVERT != 0);

  bool loadCalibration(uint16_t cal[8]);
  void saveCalibration(const uint16_t cal[8]);
  bool recalibrationRequested();

public:
  CYDDisplay() : LGFXDisplay(CYD_W, CYD_H, disp) {}

  bool begin();

#ifdef UI_CYD
  // Direct-draw mode for the colour UI (examples/companion_radio/ui-cyd): no sprite, the
  // DisplayDriver primitives paint straight onto the panel at native resolution. Only
  // main.cpp's "Loading..." banner and shutdown() still go through them.
  void turnOn() override;
  void turnOff() override;
  void clear() override;
  void startFrame(ColorVal bkg = UIColor::window_bkg) override;
  void setTextSize(int sz) override;
  void setColor(ColorVal c) override;
  void setCursor(int x, int y) override;
  void print(const char* str) override;
  void fillRect(int x, int y, int w, int h) override;
  void drawRect(int x, int y, int w, int h) override;
  void drawXbm(int x, int y, const uint8_t* bits, int w, int h) override;
  uint16_t getTextWidth(const char* str) override;
  void endFrame() override {}
#endif

  // One-time 4-point XPT2046 calibration persisted in NVS ("touchcal"/"cal_ls").
  // force=true re-runs it regardless of stored data.
  void touchCalibrate(bool force);

  // Panel colour inversion, persisted in NVS ("uicyd"/"inv"); default CYD_PANEL_INVERT.
  bool inverted() const { return _inverted; }
  void setInverted(bool on, bool save = true);
  void loadInversion();

  // Calibrated touch in LOGICAL (UI_ZOOM-divided) coordinates. False when not touched.
  bool touched(int* x, int* y);
  // Calibrated touch in NATIVE panel coordinates.
  bool touchedNative(int* x, int* y);

  LGFX_CYD& device() { return disp; }

  // Stream the panel contents over a serial port as RGB565 (header + raw rows), for
  // README screenshots. Blocks for the duration (~13 s at 115200 on the 2.8").
  void dumpScreenshot(Stream& out);
};
