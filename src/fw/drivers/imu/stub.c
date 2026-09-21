/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include <pbl/drivers/accel.h>

static uint32_t s_sampling_interval_us = 100000;
static bool s_shake_enabled;
static bool s_double_tap_enabled;

void accel_init(void) {
}

void accel_set_rotated(bool rotated) {
  (void)rotated;
}

uint32_t accel_set_sampling_interval(uint32_t interval_us) {
  s_sampling_interval_us = interval_us;
  return s_sampling_interval_us;
}

uint32_t accel_get_sampling_interval(void) {
  return s_sampling_interval_us;
}

void accel_set_num_samples(uint32_t num_samples) {
  (void)num_samples;
}

uint32_t accel_get_max_num_samples(void) {
  return 1;
}

int accel_peek(AccelDriverSample *data) {
  (void)data;
  return -1;
}

void accel_enable_shake_detection(bool on) {
  s_shake_enabled = on;
}

bool accel_get_shake_detection_enabled(void) {
  return s_shake_enabled;
}

void accel_enable_double_tap_detection(bool on) {
  s_double_tap_enabled = on;
}

bool accel_get_double_tap_detection_enabled(void) {
  return s_double_tap_enabled;
}

void accel_set_shake_sensitivity_high(bool sensitivity_high) {
  (void)sensitivity_high;
}

void accel_set_shake_sensitivity_percent(uint8_t percent) {
  (void)percent;
}
