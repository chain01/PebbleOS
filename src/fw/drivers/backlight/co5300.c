/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include <pbl/drivers/backlight.h>
#include <pbl/drivers/display/display.h>

// CO5300 brightness 0 turns the AMOLED completely off. Keep a low but visible
// image in the Pebble light-service idle state instead.
#define CO5300_STANDBY_BRIGHTNESS_PERCENT 3U

static uint8_t s_brightness = 100U;

static uint8_t prv_effective_brightness(uint8_t brightness) {
  if (brightness > 100U) {
    brightness = 100U;
  }
  if (brightness < CO5300_STANDBY_BRIGHTNESS_PERCENT) {
    brightness = CO5300_STANDBY_BRIGHTNESS_PERCENT;
  }
  return brightness;
}

void backlight_init(void) {
  display_set_brightness(s_brightness);
}

void backlight_set_brightness(uint8_t brightness) {
  const uint8_t effective = prv_effective_brightness(brightness);
  if (effective == s_brightness) {
    return;
  }
  s_brightness = effective;
  display_set_brightness(s_brightness);
}

uint8_t backlight_get_level(uint8_t brightness) {
  return prv_effective_brightness(brightness);
}

void backlight_refresh(void) {
  display_set_brightness(s_brightness);
}
