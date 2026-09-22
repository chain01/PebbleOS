/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "board/board.h"
#include "pbl/drivers/imu/lsm6ds3tr_c/lsm6ds3tr_c.h"
#include <pbl/drivers/i2c.h>
#include "kernel/util/delay.h"
#include "pbl/drivers/rtc.h"
#include "pbl/services/imu/units.h"
#include "pbl/util/math.h"
#include "pbl/util/size.h"
#include <pbl/logging/logging.h>

PBL_LOG_MODULE_DEFINE(driver_accel_lsm6ds3tr_c, CONFIG_DRIVER_IMU_LOG_LEVEL);

#define LSM6DS3TR_C_REG_FIFO_CTRL1   0x06U
#define LSM6DS3TR_C_REG_FIFO_CTRL2   0x07U
#define LSM6DS3TR_C_REG_FIFO_CTRL3   0x08U
#define LSM6DS3TR_C_REG_FIFO_CTRL5   0x0AU
#define LSM6DS3TR_C_REG_WHO_AM_I     0x0FU
#define LSM6DS3TR_C_REG_CTRL1_XL     0x10U
#define LSM6DS3TR_C_REG_CTRL3_C      0x12U
#define LSM6DS3TR_C_REG_CTRL6_C      0x15U
#define LSM6DS3TR_C_REG_STATUS_REG   0x1EU
#define LSM6DS3TR_C_REG_OUTX_L_XL    0x28U

#define LSM6DS3TR_C_WHO_AM_I_VAL     0x6AU

#define LSM6DS3TR_C_CTRL1_XL_ODR_OFF   (0x0U << 4U)
#define LSM6DS3TR_C_CTRL1_XL_ODR_12HZ5 (0x1U << 4U)
#define LSM6DS3TR_C_CTRL1_XL_ODR_26HZ  (0x2U << 4U)
#define LSM6DS3TR_C_CTRL1_XL_ODR_52HZ  (0x3U << 4U)
#define LSM6DS3TR_C_CTRL1_XL_ODR_104HZ (0x4U << 4U)
#define LSM6DS3TR_C_CTRL1_XL_ODR_208HZ (0x5U << 4U)

#define LSM6DS3TR_C_CTRL1_XL_FS_2G  (0x0U << 2U)
#define LSM6DS3TR_C_CTRL1_XL_FS_4G  (0x2U << 2U)
#define LSM6DS3TR_C_CTRL1_XL_FS_8G  (0x3U << 2U)
#define LSM6DS3TR_C_CTRL1_XL_FS_16G (0x1U << 2U)

#define LSM6DS3TR_C_CTRL3_C_SW_RESET (1U << 0U)
#define LSM6DS3TR_C_CTRL3_C_IF_INC   (1U << 2U)
#define LSM6DS3TR_C_CTRL3_C_BDU      (1U << 6U)

#define LSM6DS3TR_C_S16_SCALE_RANGE (1U << 15U)

#define LSM6DS3TR_C_SHAKE_INTERVAL_US 20000U
#define LSM6DS3TR_C_SHAKE_COOLDOWN_MS 300U
#define LSM6DS3TR_C_SHAKE_MIN_MG 200U
#define LSM6DS3TR_C_SHAKE_MAX_MG 1800U

static const uint32_t s_odr_intervals_us[] = {
  80000U, 38462U, 19231U, 9615U, 4808U,
};

static const uint8_t s_odr_bits[] = {
  LSM6DS3TR_C_CTRL1_XL_ODR_12HZ5,
  LSM6DS3TR_C_CTRL1_XL_ODR_26HZ,
  LSM6DS3TR_C_CTRL1_XL_ODR_52HZ,
  LSM6DS3TR_C_CTRL1_XL_ODR_104HZ,
  LSM6DS3TR_C_CTRL1_XL_ODR_208HZ,
};

static bool prv_read(uint8_t reg, uint8_t *data, uint16_t len) {
  i2c_use(LSM6DS3TR_C->i2c);
  const bool result = i2c_read_register_block(LSM6DS3TR_C->i2c, reg, len, data);
  i2c_release(LSM6DS3TR_C->i2c);
  return result;
}

static bool prv_write(uint8_t reg, uint8_t value) {
  i2c_use(LSM6DS3TR_C->i2c);
  const bool result = i2c_write_register(LSM6DS3TR_C->i2c, reg, value);
  i2c_release(LSM6DS3TR_C->i2c);
  return result;
}

static uint8_t prv_scale_bits(void) {
  switch (CONFIG_ACCEL_LSM6DS3TR_C_SCALE_MG) {
    case 2000U:
      return LSM6DS3TR_C_CTRL1_XL_FS_2G;
    case 4000U:
      return LSM6DS3TR_C_CTRL1_XL_FS_4G;
    case 8000U:
      return LSM6DS3TR_C_CTRL1_XL_FS_8G;
    case 16000U:
      return LSM6DS3TR_C_CTRL1_XL_FS_16G;
    default:
      return LSM6DS3TR_C_CTRL1_XL_FS_4G;
  }
}

static int16_t prv_raw_to_s16(const uint8_t *raw) {
  return (int16_t)((uint16_t)raw[0] | ((uint16_t)raw[1] << 8U));
}

static int16_t prv_axis_mg(IMUCoordinateAxis axis, const uint8_t *raw) {
  const uint8_t offset = LSM6DS3TR_C->axis_map[axis];
  int16_t value = (int16_t)(((int32_t)prv_raw_to_s16(&raw[offset * 2U]) *
                             (int32_t)CONFIG_ACCEL_LSM6DS3TR_C_SCALE_MG) /
                            (int32_t)LSM6DS3TR_C_S16_SCALE_RANGE);
  value *= LSM6DS3TR_C->axis_dir[axis];
  if (LSM6DS3TR_C->state->rotated && (axis == AXIS_X || axis == AXIS_Y)) {
    value = -value;
  }
  return value;
}

static uint64_t prv_get_time_us(void) {
  time_t time_s;
  uint16_t time_ms;
  rtc_get_time_ms(&time_s, &time_ms);
  return (((uint64_t)time_s) * 1000U + time_ms) * 1000U;
}

static bool prv_read_sample(AccelDriverSample *sample) {
  uint8_t raw[6];
  if (!prv_read(LSM6DS3TR_C_REG_OUTX_L_XL, raw, sizeof(raw))) {
    return false;
  }

  sample->timestamp_us = prv_get_time_us();
  sample->x = prv_axis_mg(AXIS_X, raw);
  sample->y = prv_axis_mg(AXIS_Y, raw);
  sample->z = prv_axis_mg(AXIS_Z, raw);
  return true;
}

static void prv_maybe_report_shake(const AccelDriverSample *sample) {
  LSM6DS3TR_CState *state = LSM6DS3TR_C->state;
  if (!state->shake_detection_enabled || state->last_sample.timestamp_us == 0U) {
    return;
  }

  const int32_t dx = sample->x - state->last_sample.x;
  const int32_t dy = sample->y - state->last_sample.y;
  const int32_t dz = sample->z - state->last_sample.z;
  const int32_t delta = abs(dx) + abs(dy) + abs(dz);
  const uint32_t threshold =
      LSM6DS3TR_C_SHAKE_MAX_MG -
      ((LSM6DS3TR_C_SHAKE_MAX_MG - LSM6DS3TR_C_SHAKE_MIN_MG) *
       state->shake_sensitivity_percent) / 100U;

  if (delta < (int32_t)threshold ||
      (state->last_shake_us != 0U &&
       (sample->timestamp_us - state->last_shake_us) <
           (LSM6DS3TR_C_SHAKE_COOLDOWN_MS * 1000U))) {
    return;
  }

  IMUCoordinateAxis axis = AXIS_X;
  int32_t direction = dx;
  if (abs(dy) > abs(direction)) {
    axis = AXIS_Y;
    direction = dy;
  }
  if (abs(dz) > abs(direction)) {
    axis = AXIS_Z;
    direction = dz;
  }
  state->last_shake_us = sample->timestamp_us;
  accel_cb_shake_detected(axis, direction);
}

static void prv_sample_timer_cb(void *unused) {
  AccelDriverSample sample;
  if (!prv_read_sample(&sample)) {
    PBL_LOG_WRN("LSM6DS3TR-C sample read failed");
    return;
  }

  prv_maybe_report_shake(&sample);
  LSM6DS3TR_C->state->last_sample = sample;
  if (LSM6DS3TR_C->state->sampling_enabled) {
    accel_cb_new_sample(&sample);
  }
}

static void prv_update_timer(void) {
  LSM6DS3TR_CState *state = LSM6DS3TR_C->state;
  if (state->sample_timer == TIMER_INVALID_ID) {
    return;
  }

  const bool needs_timer = state->sampling_enabled || state->shake_detection_enabled;
  if (!needs_timer) {
    new_timer_stop(state->sample_timer);
    return;
  }

  uint32_t interval_us = state->sampling_interval_us;
  if (!state->sampling_enabled || interval_us == 0U) {
    interval_us = LSM6DS3TR_C_SHAKE_INTERVAL_US;
  }
  new_timer_start(state->sample_timer, interval_us / 1000U, prv_sample_timer_cb, NULL,
                  TIMER_START_FLAG_REPEATING);
}

static bool prv_configure_odr(uint32_t interval_us) {
  LSM6DS3TR_CState *state = LSM6DS3TR_C->state;
  uint8_t odr_bits = LSM6DS3TR_C_CTRL1_XL_ODR_OFF;
  uint32_t actual_interval_us = 0U;

  if (interval_us != 0U) {
    for (uint32_t i = 0; i < ARRAY_LENGTH(s_odr_bits); i++) {
      if (s_odr_intervals_us[i] <= interval_us) {
        odr_bits = s_odr_bits[i];
        actual_interval_us = s_odr_intervals_us[i];
        break;
      }
    }
    if (odr_bits == LSM6DS3TR_C_CTRL1_XL_ODR_OFF) {
      odr_bits = s_odr_bits[ARRAY_LENGTH(s_odr_bits) - 1U];
      actual_interval_us = s_odr_intervals_us[ARRAY_LENGTH(s_odr_intervals_us) - 1U];
    }
  } else if (state->shake_detection_enabled) {
    odr_bits = LSM6DS3TR_C_CTRL1_XL_ODR_52HZ;
    actual_interval_us = 19231U;
  }

  if (!prv_write(LSM6DS3TR_C_REG_CTRL1_XL, odr_bits | prv_scale_bits())) {
    return false;
  }

  state->odr_bits = odr_bits;
  state->sampling_interval_us = actual_interval_us;
  return true;
}

void accel_init(void) {
  LSM6DS3TR_CState *state = LSM6DS3TR_C->state;
  *state = (LSM6DS3TR_CState){};
  state->sample_timer = new_timer_create();
  state->shake_sensitivity_percent = 50U;

  uint8_t value = 0;
  if (!prv_read(LSM6DS3TR_C_REG_WHO_AM_I, &value, 1)) {
    PBL_LOG_ERR("LSM6DS3TR-C WHO_AM_I read failed");
    return;
  }
  if (value != LSM6DS3TR_C_WHO_AM_I_VAL) {
    PBL_LOG_ERR("LSM6DS3TR-C unexpected WHO_AM_I 0x%02x", value);
    return;
  }
  const uint8_t who_am_i = value;

  if (!prv_write(LSM6DS3TR_C_REG_CTRL3_C, LSM6DS3TR_C_CTRL3_C_SW_RESET)) {
    PBL_LOG_ERR("LSM6DS3TR-C reset failed");
    return;
  }
  delay_us(10000U);
  do {
    if (!prv_read(LSM6DS3TR_C_REG_CTRL3_C, &value, 1)) {
      PBL_LOG_ERR("LSM6DS3TR-C reset poll failed");
      return;
    }
  } while (value & LSM6DS3TR_C_CTRL3_C_SW_RESET);

  if (!prv_write(LSM6DS3TR_C_REG_CTRL3_C,
                 LSM6DS3TR_C_CTRL3_C_BDU | LSM6DS3TR_C_CTRL3_C_IF_INC)) {
    PBL_LOG_ERR("LSM6DS3TR-C CTRL3_C write failed");
    return;
  }

  if (!prv_write(LSM6DS3TR_C_REG_CTRL6_C, 0U) ||
      !prv_configure_odr(0U)) {
    PBL_LOG_ERR("LSM6DS3TR-C configuration failed");
    return;
  }

  state->initialized = true;
  PBL_LOG_INFO("LSM6DS3TR-C ready: id=0x%02x", who_am_i);
}

void accel_set_rotated(bool rotated) {
  LSM6DS3TR_C->state->rotated = rotated;
}

uint32_t accel_set_sampling_interval(uint32_t interval_us) {
  LSM6DS3TR_CState *state = LSM6DS3TR_C->state;
  if (!state->initialized) {
    state->sampling_interval_us = interval_us;
    return interval_us;
  }

  if (!prv_configure_odr(interval_us)) {
    PBL_LOG_ERR("LSM6DS3TR-C ODR configuration failed");
    return state->sampling_interval_us;
  }
  prv_update_timer();
  return state->sampling_interval_us;
}

uint32_t accel_get_sampling_interval(void) {
  return LSM6DS3TR_C->state->sampling_interval_us;
}

void accel_set_num_samples(uint32_t num_samples) {
  LSM6DS3TR_CState *state = LSM6DS3TR_C->state;
  state->sampling_enabled = (num_samples > 0U);
  if (state->initialized) {
    const uint32_t interval_us = state->sampling_enabled ? MAX(state->sampling_interval_us, 10000U) : 0U;
    (void)prv_configure_odr(interval_us);
  }
  prv_update_timer();
}

uint32_t accel_get_max_num_samples(void) {
  return 1U;
}

int accel_peek(AccelDriverSample *data) {
  LSM6DS3TR_CState *state = LSM6DS3TR_C->state;
  if (!state->initialized) {
    return -1;
  }
  if (state->sampling_enabled && state->last_sample.timestamp_us != 0U) {
    *data = state->last_sample;
    return 0;
  }
  return prv_read_sample(data) ? 0 : -1;
}

void accel_enable_shake_detection(bool on) {
  LSM6DS3TR_CState *state = LSM6DS3TR_C->state;
  if (!state->initialized || state->shake_detection_enabled == on) {
    return;
  }
  state->shake_detection_enabled = on;
  state->last_shake_us = 0U;
  state->last_sample.timestamp_us = 0U;
  if (!state->sampling_enabled) {
    (void)prv_configure_odr(on ? LSM6DS3TR_C_SHAKE_INTERVAL_US : 0U);
  }
  prv_update_timer();
}

bool accel_get_shake_detection_enabled(void) {
  return LSM6DS3TR_C->state->shake_detection_enabled;
}

void accel_enable_double_tap_detection(bool on) {
  (void)on;
}

bool accel_get_double_tap_detection_enabled(void) {
  return false;
}

void accel_set_shake_sensitivity_high(bool sensitivity_high) {
  accel_set_shake_sensitivity_percent(sensitivity_high ? 0U : 50U);
}

void accel_set_shake_sensitivity_percent(uint8_t percent) {
  LSM6DS3TR_C->state->shake_sensitivity_percent = MIN(percent, 100U);
}
