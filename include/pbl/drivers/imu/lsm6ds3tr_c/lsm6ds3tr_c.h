/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <pbl/drivers/accel.h>
#include <pbl/services/new_timer/new_timer.h>

typedef struct LSM6DS3TR_CState {
  bool initialized;
  bool sampling_enabled;
  bool shake_detection_enabled;
  bool rotated;
  uint8_t shake_sensitivity_percent;
  uint32_t sampling_interval_us;
  uint8_t odr_bits;
  AccelDriverSample last_sample;
  TimerID sample_timer;
} LSM6DS3TR_CState;

typedef struct LSM6DS3TR_CConfig {
  I2CSlavePort *i2c;
  LSM6DS3TR_CState *state;
  uint8_t axis_map[3];
  int8_t axis_dir[3];
} LSM6DS3TR_CConfig;
