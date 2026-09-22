/* SPDX-FileCopyrightText: 2024 Google LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "board/display.h"
#include "applib/graphics/gtypes.h"

#include <stdint.h>
#include <stdbool.h>

typedef struct {
  uint16_t address;
  uint8_t *data;
} DisplayRow;

typedef bool (*NextRowCallback)(DisplayRow *row);
typedef void (*UpdateCompleteCallback)(void);

//! Update the display with a boot animation frame.
//! This is a simple interface for the boot animation service to send frames
//! to the display before the compositor is initialized.
//! @param framebuffer Pointer to the framebuffer data (DISPLAY_FRAMEBUFFER_BYTES)
void display_update_boot_frame(uint8_t *framebuffer);

void display_init(void);

void display_clear(void);

void display_set_enabled(bool enabled);

//! Set the panel brightness in percent. A value of zero is allowed at the
//! driver layer; board backlight policy decides whether it maps to a minimum
//! visible brightness for AMOLED panels.
void display_set_brightness(uint8_t brightness);

void display_set_rotated(bool rotated);

void display_update(NextRowCallback nrcb, UpdateCompleteCallback uccb);

bool display_update_in_progress(void);