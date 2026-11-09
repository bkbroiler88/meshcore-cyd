#ifdef XIAO_NRF52

#include <Arduino.h>
#include <Wire.h>

#include "XiaoNrf52Board.h"

#ifdef NRF52_POWER_MANAGEMENT
// Static configuration for power management
// Values set in variant.h defines
const PowerMgtConfig power_config = {
  .lpcomp_ain_channel = PWRMGT_LPCOMP_AIN,
  .lpcomp_refsel = PWRMGT_LPCOMP_REFSEL,
  .voltage_bootlock = PWRMGT_VOLTAGE_BOOTLOCK
};

void XiaoNrf52Board::initiateShutdown(uint8_t reason) {
  bool enable_lpcomp = (reason == SHUTDOWN_REASON_LOW_VOLTAGE ||
                        reason == SHUTDOWN_REASON_BOOT_PROTECT);

  pinMode(VBAT_ENABLE, OUTPUT);
  digitalWrite(VBAT_ENABLE, enable_lpcomp ? LOW : HIGH);

  if (enable_lpcomp) {
    configureVoltageWake(power_config.lpcomp_ain_channel, power_config.lpcomp_refsel);
  }

  enterSystemOff(reason);
}
#endif // NRF52_POWER_MANAGEMENT

void XiaoNrf52Board::begin() {
  NRF52BoardDCDC::begin();

  // Configure battery voltage ADC
  pinMode(PIN_VBAT, INPUT);
  pinMode(VBAT_ENABLE, OUTPUT);
  digitalWrite(VBAT_ENABLE, LOW);  // Enable VBAT divider for reading
  analogReadResolution(12);
  analogReference(AR_INTERNAL_3_0);
  delay(50);  // Allow ADC to settle

#ifdef PIN_USER_BTN
  pinMode(PIN_USER_BTN, INPUT_PULLUP);
#endif

#if defined(PIN_WIRE_SDA) && defined(PIN_WIRE_SCL)
  Wire.setPins(PIN_WIRE_SDA, PIN_WIRE_SCL);
#endif

  Wire.begin();

#ifdef P_LORA_TX_LED
  pinMode(P_LORA_TX_LED, OUTPUT);
  digitalWrite(P_LORA_TX_LED, HIGH);
#endif

#ifdef NRF52_POWER_MANAGEMENT
  // Boot voltage protection check (may not return if voltage too low)
  checkBootVoltage(&power_config);
#endif

  delay(10);  // Give sx1262 some time to power up
}

#ifdef BATT_MAX17048
// MAX17048 fuel gauge (fixed I2C 0x36) on the main Wire bus. Registers are
// big-endian; VCELL (0x02) is 78.125 uV/LSB. Boards that feed the XIAO through
// its 3V3 pin (BKBroiler Mesh V2.1) leave the on-module VBAT divider unpowered,
// so the gauge is the only real battery reading there.
static bool max17048_read(uint8_t reg, uint16_t& v) {
  Wire.beginTransmission(0x36);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint8_t)0x36, (uint8_t)2) != 2) return false;
  v = (uint16_t)Wire.read() << 8;
  v |= (uint16_t)Wire.read();
  return true;
}
#endif

uint16_t XiaoNrf52Board::getBattMilliVolts() {
#ifdef BATT_MAX17048
  uint16_t vcell;
  if (max17048_read(0x02, vcell)) {
    return (uint32_t)vcell * 5 / 64;   // 78.125 uV/LSB == 5/64 mV
  }
  // gauge absent / no ACK: fall through to the XIAO's own divider
#endif
  // https://wiki.seeedstudio.com/XIAO_BLE#q3-what-are-the-considerations-when-using-xiao-nrf52840-sense-for-battery-charging
  // VBAT_ENABLE must be LOW to read battery voltage
  digitalWrite(VBAT_ENABLE, LOW);
  int adcvalue = analogRead(PIN_VBAT);
  return (adcvalue * ADC_MULTIPLIER * AREF_VOLTAGE) / 4.096;
}

#endif