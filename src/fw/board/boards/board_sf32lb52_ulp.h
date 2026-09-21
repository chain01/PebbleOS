/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "pbl/services/imu/units.h"

#define BT_VENDOR_ID   0x0EEA
#define BT_VENDOR_NAME "Core Devices LLC"

extern UARTDevice *const DBG_UART;
extern QSPIPort *const QSPI;
extern I2CBus *const I2C1_BUS;
extern I2CSlavePort *const I2C_FT6146;
extern QSPIFlash *const QSPI_FLASH;
extern const BoardConfig BOARD_CONFIG;
extern const BoardConfigButton BOARD_CONFIG_BUTTON;
extern const BoardConfigPower BOARD_CONFIG_POWER;

static const BoardConfigAccel BOARD_CONFIG_ACCEL = {
  .default_motion_sensitivity = 55U,
};

static const BoardConfigMag BOARD_CONFIG_MAG = {
  .mag_config = {
    .axes_offsets[AXIS_X] = 1,
    .axes_offsets[AXIS_Y] = 0,
    .axes_offsets[AXIS_Z] = 2,
    .axes_inverts[AXIS_X] = false,
    .axes_inverts[AXIS_Y] = true,
    .axes_inverts[AXIS_Z] = false,
  },
};
