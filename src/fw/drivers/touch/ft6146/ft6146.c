/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "board/board.h"
#include <pbl/drivers/gpio.h>
#include <pbl/drivers/i2c.h>
#include <pbl/drivers/touch/touch_sensor.h>
#include "kernel/util/delay.h"
#include "kernel/util/sleep.h"
#include "pbl/services/new_timer/new_timer.h"
#include "pbl/services/touch/touch.h"
#include <pbl/logging/logging.h>
#include "system/passert.h"

PBL_LOG_MODULE_DEFINE(driver_touch_ft6146, CONFIG_DRIVER_TOUCH_LOG_LEVEL);

#define FT6146_TOUCH_REG   0x01
#define FT6146_ID_H_REG    0xA3
#define FT6146_ID_L_REG    0x9F
#define FT6146_G_MODE_REG  0xA4
#define FT6146_POINT_LEN   (2 + 6 * 2)

#define FT6146_PANEL_WIDTH  390
#define FT6146_PANEL_HEIGHT 450
#define FT6146_X_OFFSET     ((FT6146_PANEL_WIDTH - PBL_DISPLAY_WIDTH) / 2)
#define FT6146_Y_OFFSET     ((FT6146_PANEL_HEIGHT - PBL_DISPLAY_HEIGHT) / 2)
#define FT6146_POLL_MS      30

static TimerID s_poll_timer = TIMER_INVALID_ID;
static bool s_touching;
static int16_t s_last_x;
static int16_t s_last_y;
static uint8_t s_error_count;
static bool s_poll_alive_logged;
static uint16_t s_poll_count;

static bool prv_read_regs(uint8_t reg, uint8_t *buffer, uint32_t length) {
  i2c_use(I2C_FT6146);
  const bool result = i2c_read_register_block(I2C_FT6146, reg, length, buffer);
  i2c_release(I2C_FT6146);
  return result;
}

static bool prv_write_reg(uint8_t reg, uint8_t value) {
  i2c_use(I2C_FT6146);
  const bool result = i2c_write_register(I2C_FT6146, reg, value);
  i2c_release(I2C_FT6146);
  return result;
}

static int16_t prv_clamp(int16_t value, int16_t min, int16_t max) {
  if (value < min) {
    return min;
  }
  if (value > max) {
    return max;
  }
  return value;
}

static void prv_report_up(void) {
  if (s_touching) {
    PBL_LOG_INFO("FT6146 up x=%d y=%d", s_last_x, s_last_y);
    touch_handle_update(TouchState_FingerUp, s_last_x, s_last_y);
    s_touching = false;
  }
}

static void prv_set_power_pin(uint32_t gpio_pin, bool level) {
  OutputConfig config = {
    .gpio = hwp_gpio1,
    .gpio_pin = gpio_pin,
    .active_high = true,
  };
  HAL_PIN_Set(PAD_PA00 + gpio_pin, GPIO_A0 + gpio_pin, PIN_NOPULL, 1);
  gpio_output_init(&config, GPIO_OType_PP);
  gpio_output_set(&config, level);
}

static void prv_poll_timer_cb(void *unused) {
  if (!s_poll_alive_logged) {
    PBL_LOG_INFO("FT6146 polling alive");
    s_poll_alive_logged = true;
  }

  uint8_t point_data[FT6146_POINT_LEN] = {0};
  if (!prv_read_regs(FT6146_TOUCH_REG, point_data, sizeof(point_data))) {
    prv_report_up();
    if (++s_error_count >= 5) {
      PBL_LOG_ERR("FT6146 polling stopped after repeated I2C errors");
      new_timer_stop(s_poll_timer);
    }
    return;
  }
  s_error_count = 0;

  const uint8_t touch_count = point_data[1] & 0x0F;
  if ((++s_poll_count % 33U) == 0U) {
    PBL_LOG_INFO("FT6146 heartbeat count=%u raw=%02X %02X %02X %02X", touch_count,
                 point_data[0], point_data[1], point_data[2], point_data[3]);
  }
  if (touch_count == 0) {
    prv_report_up();
    return;
  }

  // FT6146 reports physical panel coordinates in the same orientation as the
  // CO5300 panel. The display driver scales the 200x228 logical framebuffer to
  // the full 390x450 panel, so map touch coordinates with the same scaling.
  const int16_t physical_x = ((int16_t)(point_data[2] & 0x0F) << 8) | point_data[3];
  const int16_t physical_y = ((int16_t)(point_data[4] & 0x0F) << 8) | point_data[5];
  int16_t x = (int16_t)(((uint32_t)physical_x * PBL_DISPLAY_WIDTH) / FT6146_PANEL_WIDTH);
  int16_t y = (int16_t)(((uint32_t)physical_y * PBL_DISPLAY_HEIGHT) / FT6146_PANEL_HEIGHT);

  x = prv_clamp(x, 0, PBL_DISPLAY_WIDTH - 1);
  y = prv_clamp(y, 0, PBL_DISPLAY_HEIGHT - 1);

  if (!s_touching || x != s_last_x || y != s_last_y) {
    PBL_LOG_INFO("FT6146 down x=%d y=%d", x, y);
    touch_handle_update(TouchState_FingerDown, x, y);
    s_touching = true;
    s_last_x = x;
    s_last_y = y;
  }
}

static void prv_reset_touch(void) {
  // The touch panel and LCD share the ULP power rails. PebbleOS calls the
  // touch driver before display_init(), so bring the rails up here as well.
  prv_set_power_pin(1, true);
  prv_set_power_pin(26, true);
  prv_set_power_pin(38, true);
  prv_set_power_pin(42, true);
  psleep(2);

  OutputConfig reset = {
    .gpio = hwp_gpio1,
    .gpio_pin = 9,
    .active_high = true,
  };
  HAL_PIN_Set(PAD_PA09, GPIO_A9, PIN_NOPULL, 1);
  gpio_output_init(&reset, GPIO_OType_PP);
  gpio_output_set(&reset, false);
  psleep(5);
  gpio_output_set(&reset, true);
  psleep(80);
}

void touch_sensor_init(void) {
  prv_reset_touch();

  uint8_t id_h = 0;
  uint8_t id_l = 0;
  if (!prv_read_regs(FT6146_ID_H_REG, &id_h, 1) ||
      !prv_read_regs(FT6146_ID_L_REG, &id_l, 1)) {
    PBL_LOG_ERR("FT6146 ID read failed");
  } else {
    PBL_LOG_INFO("FT6146 ID: 0x%02X%02X", id_h, id_l);
  }

  // Polling mode for the bring-up driver. The FT6x36 family defaults can
  // leave the controller gated in interrupt mode; selecting polling mode
  // makes the status register update without requiring the INT line.
  if (!prv_write_reg(FT6146_G_MODE_REG, 0)) {
    PBL_LOG_ERR("FT6146 G_MODE write failed");
  }

  if (s_poll_timer == TIMER_INVALID_ID) {
    s_poll_timer = new_timer_create();
    PBL_ASSERTN(s_poll_timer != TIMER_INVALID_ID);
  }
  new_timer_start(s_poll_timer, FT6146_POLL_MS, prv_poll_timer_cb, NULL,
                  TIMER_START_FLAG_REPEATING);
}

void touch_sensor_set_enabled(bool enabled) {
  if (s_poll_timer == TIMER_INVALID_ID) {
    return;
  }
  if (enabled) {
    new_timer_start(s_poll_timer, FT6146_POLL_MS, prv_poll_timer_cb, NULL,
                    TIMER_START_FLAG_REPEATING);
  } else {
    // Keep polling during bring-up so physical touch tests remain observable
    // even before the shell has installed a touch subscriber.
    PBL_LOG_INFO("FT6146 disable ignored for bring-up");
  }
}
