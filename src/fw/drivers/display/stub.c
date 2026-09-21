/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include <pbl/drivers/display/display.h>

void display_init(void) {
}

void display_clear(void) {
}

void display_set_enabled(bool enabled) {
  (void)enabled;
}

void display_set_rotated(bool rotated) {
  (void)rotated;
}

void display_update(NextRowCallback nrcb, UpdateCompleteCallback uccb) {
  (void)nrcb;
  if (uccb) {
    uccb();
  }
}

bool display_update_in_progress(void) {
  return false;
}

void display_update_boot_frame(uint8_t *framebuffer) {
  (void)framebuffer;
}
