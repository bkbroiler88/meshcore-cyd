#pragma once

// Pin tables for the two LCDWIKI CYD boards this variant covers. Selected by the env:
//   -D LCDWIKI_28_ST7789   2.8" ESP32-2432S028R, ST7789 240x320, touch on dedicated pins
//   -D LCDWIKI_35          3.5" ESP32-32E, ST7796 320x480, touch on the shared LCD bus
// LoRa / button / battery / SD pins come from platformio.ini (P_LORA_*, SX126X_RXEN, ...).
// Values are the hardware-verified ones from the Meshtastic port of the same boards
// (meshtastic-fw/variants/esp32/lcdwiki_2_8_e22 and lcdwiki_3_5_e22).

#if defined(LCDWIKI_35)
  #define CYD_PANEL_ST7796 1
  #define CYD_NATIVE_W   320
  #define CYD_NATIVE_H   480
  #define CYD_TFT_SCLK   14
  #define CYD_TFT_MOSI   13
  #define CYD_TFT_MISO   12
  #define CYD_TFT_CS     15
  #define CYD_TFT_DC     2
  #define CYD_TFT_RST    -1     // panel reset tied to EN
  #define CYD_TFT_BL     27
  // XPT2046 shares the LCD HSPI bus; only CS is its own.
  #define CYD_TOUCH_SHARED_BUS 1
  #define CYD_TOUCH_SCLK 14
  #define CYD_TOUCH_MOSI 13
  #define CYD_TOUCH_MISO 12
  #define CYD_TOUCH_CS   33
#elif defined(LCDWIKI_28_ST7789)
  #define CYD_PANEL_ST7796 0
  #define CYD_NATIVE_W   240
  #define CYD_NATIVE_H   320
  #define CYD_TFT_SCLK   14
  #define CYD_TFT_MOSI   13
  #define CYD_TFT_MISO   12
  #define CYD_TFT_CS     15
  #define CYD_TFT_DC     2
  #define CYD_TFT_RST    -1
  #define CYD_TFT_BL     21
  // XPT2046 on its OWN pins (not the LCD bus): bit-banged by LovyanGFX (spi_host = -1).
  #define CYD_TOUCH_SHARED_BUS 0
  #define CYD_TOUCH_SCLK 25
  #define CYD_TOUCH_MOSI 32
  #define CYD_TOUCH_MISO 39
  #define CYD_TOUCH_CS   33
#else
  #error "Define LCDWIKI_28_ST7789 or LCDWIKI_35"
#endif

// Landscape geometry after setRotation(1).
#define CYD_W CYD_NATIVE_H
#define CYD_H CYD_NATIVE_W

#ifndef CYD_PANEL_INVERT
  #define CYD_PANEL_INVERT 1
#endif
