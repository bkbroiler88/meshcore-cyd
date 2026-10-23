#pragma once

#define RADIOLIB_STATIC_ONLY 1
#include <RadioLib.h>
#include <helpers/radiolib/RadioLibWrappers.h>
#include <helpers/ESP32Board.h>
#include <helpers/radiolib/CustomSX1262Wrapper.h>
#include <helpers/AutoDiscoverRTCClock.h>
#include <helpers/SensorManager.h>
#if ENV_INCLUDE_GPS
  #include <helpers/sensors/EnvironmentSensorManager.h>
  #include <helpers/sensors/MicroNMEALocationProvider.h>
#endif

#ifdef DISPLAY_CLASS
  #include "CYDDisplay.h"
  #include "TouchButton.h"
  #include <helpers/ui/MomentaryButton.h>
#endif

extern ESP32Board board;
extern WRAPPER_CLASS radio_driver;
extern AutoDiscoverRTCClock rtc_clock;
#if ENV_INCLUDE_GPS
  extern EnvironmentSensorManager sensors;
#else
  extern SensorManager sensors;
#endif

#ifdef DISPLAY_CLASS
  extern DISPLAY_CLASS display;
  extern MomentaryButton user_btn;
  extern TouchButton touch_btn;
#endif

bool radio_init();
mesh::LocalIdentity radio_new_identity();
