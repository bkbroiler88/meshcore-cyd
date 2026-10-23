#include <Arduino.h>
#include "target.h"
#include <driver/gpio.h>
#include <soc/gpio_sig_map.h>
#include <soc/gpio_pins.h>

ESP32Board board;

// VSPI explicitly: SPIClass() defaults to HSPI on the classic ESP32, and HSPI (14/13/12)
// is the LCD + touch bus on these boards. The E22 and the SD slot share VSPI (18/19/23).
static SPIClass spi(VSPI);
RADIO_CLASS radio = new Module(P_LORA_NSS, P_LORA_DIO_1, P_LORA_RESET, P_LORA_BUSY, spi);

WRAPPER_CLASS radio_driver(radio, board);

ESP32RTCClock fallback_clock;
AutoDiscoverRTCClock rtc_clock(fallback_clock);
#if ENV_INCLUDE_GPS
  // GPS module TX -> GPIO3 (PIN_GPS_TX), same wiring as the Meshtastic port. UART1 needs a
  // TX pad or the Arduino core silently defaults it to GPIO10 (SPI flash): PIN_GPS_RX=16
  // is a free pad on both boards and nothing listens to it.
  MicroNMEALocationProvider nmea(Serial1, &rtc_clock);
  EnvironmentSensorManager sensors(nmea);
#else
  SensorManager sensors;
#endif

#ifdef DISPLAY_CLASS
  DISPLAY_CLASS display;
  MomentaryButton user_btn(PIN_USER_BTN, 1000, true);   // BOOT button, active low
  TouchButton touch_btn(&display);
#endif

bool radio_init() {
  fallback_clock.begin();
  rtc_clock.begin(Wire);

#ifdef SDCARD_CS
  // The SD slot sits on the same VSPI bus as the SX1262. Park its CS high before the
  // radio's first transaction so the card can never answer on the bus.
  pinMode(SDCARD_CS, OUTPUT);
  digitalWrite(SDCARD_CS, HIGH);
#endif

  Serial.printf("CYD: heap before radio free=%u largest=%u\n",
    ESP.getFreeHeap(), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

  return radio.std_init(&spi);
}

mesh::LocalIdentity radio_new_identity() {
  RadioNoiseListener rng(radio);
  return mesh::LocalIdentity(&rng);
}
