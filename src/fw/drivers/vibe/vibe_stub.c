/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include <pbl/drivers/vibe.h>

#include "console/prompt.h"

#include <stdlib.h>

void vibe_init(void) {
}

void vibe_set_strength(int8_t strength) {
  (void)strength;
}

void vibe_ctl(bool on) {
  (void)on;
}

void vibe_force_off(void) {
}

int8_t vibe_get_braking_strength(void) {
  return VIBE_STRENGTH_OFF;
}

status_t vibe_calibrate(void) {
  return E_INVALID_OPERATION;
}

uint8_t vibe_get_calibration(void) {
  return 0xFF;
}

void vibe_apply_calibration(uint8_t cali) {
  (void)cali;
}

void command_vibe_ctl(const char *arg) {
  const int value = atoi(arg);
  if (value < 0 || value > VIBE_STRENGTH_MAX || (value == 0 && arg[0] != '0')) {
    prompt_send_response("Invalid argument");
    return;
  }

  vibe_set_strength(value);
  vibe_ctl(value != 0);
  prompt_send_response("OK");
}
