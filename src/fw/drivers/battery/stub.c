/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include <pbl/drivers/battery.h>

#define BATTERY_STUB_MV 4000

void battery_init(void) {
}

int battery_get_millivolts(void) {
  return BATTERY_STUB_MV;
}

int battery_get_constants(BatteryConstants *constants) {
  *constants = (BatteryConstants) {
    .v_mv = BATTERY_STUB_MV,
    .i_ua = 0,
    .t_mc = 25000,
  };
  return 0;
}

bool battery_charge_controller_thinks_we_are_charging_impl(void) {
  return false;
}

bool battery_is_usb_connected_impl(void) {
  return false;
}

void battery_set_charge_enable(bool charging_enabled) {
  (void)charging_enabled;
}

void battery_set_fast_charge(bool fast_charge_enabled) {
  (void)fast_charge_enabled;
}

ADCVoltageMonitorReading battery_read_voltage_monitor(void) {
  return (ADCVoltageMonitorReading) {
    .vref_total = 0,
    .vmon_total = 0,
  };
}

uint32_t battery_convert_reading_to_millivolts(ADCVoltageMonitorReading reading,
                                               uint32_t numerator,
                                               uint32_t denominator) {
  (void)reading;
  (void)numerator;
  (void)denominator;
  return BATTERY_STUB_MV;
}

int battery_charge_status_get(BatteryChargeStatus *status) {
  *status = BatteryChargeStatusUnknown;
  return 0;
}
