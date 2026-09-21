/* SPDX-FileCopyrightText: 2025 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "bf0_hal.h"
#include "bf0_hal_efuse.h"

#define NVDS_BUFF_START 0x2040FE00

static const uint8_t s_ble_slp_default[] = {
// Control pre-wakeup time for the sleep of BT subsytem in LCPU.
// See SiFli-SDK EXT_WAKEUP_TIME_LXT32K/EXT_WAKEUP_TIME_RC10K for details.
// RC10K -> 4500us (0x1194)
// LXT32K -> 3500us (0x0DAC)
#ifdef LXT_DISABLE
  0x0D,
  0x02,
  0x19,
  0x11,
#else
  0x0D,
  0x02,
  0xAC,
  0x0D,
#endif
  // Control maximum sleep duration of BT subsystem.
  // The last 0x01 means 10s in BLE only and 30s in dual mode. 0 means 500ms
  0x12,
  0x01,
  0x01,
  // Control the log in controller
  // Changed to 0x20, 0x00, 0x09, 0x00 will enable HCI logs by default
  0x2F,
  0x04,
  0x20,
  0x00,
  0x00,
  0x00,
  // Internal usage, for scheduling
  0x15,
  0x01,
  0x01,
};

static int prv_bt_mac_addr_generate(uint8_t mac_addr[6]) {
  uint8_t uid[16] = {0};
  int32_t ret;
  uint8_t chksum;
  uint8_t i;

  ret = HAL_EFUSE_Read(0, uid, sizeof(uid));
  if (ret != (int32_t)sizeof(uid)) {
    return false;
  }

  for (i = 0U; i < sizeof(uid); i++) {
    if (uid[i] != 0U) {
      break;
    }
  }

  if (i >= sizeof(uid)) {
    return false;
  }

  // Some SF32LB52 production lots use the newer v2 UID layout. Keep the
  // native-checksum path for those parts.
  if (uid[7] == 0xA5) {
    chksum = uid[0] + uid[1] + uid[2] + uid[3] + uid[4] + uid[5];
    bool nonzero = false;
    for (i = 0U; i < 6U; i++) {
      nonzero |= uid[i] != 0U;
    }
    if (chksum == uid[6] && nonzero) {
      memcpy(mac_addr, uid, 6);
      return true;
    }
  }

  // The ULP sample has a non-v2 UID. Use the same v1 UID-to-MAC conversion
  // as the SiFli SDK so the BLE controller still gets a stable address.
  uint32_t lower_part = uid[8] | (uid[9] << 8) | ((uid[10] & 0x07) << 16);
  uint32_t lot_part = ((uid[10] & 0xF8) >> 3) | ((uid[11] & 0x7F) << 5) |
                      (uid[12] << 12) | (uid[13] << 20) | ((uid[14] & 0x03) << 28);
  uint32_t higher_part = 0;
  uint32_t base = 36;

  for (i = 0; i < 5; i++) {
    if (i == 0) {
      higher_part += (lot_part & 0x3F) % 36;
    } else {
      higher_part += ((lot_part & 0x3F) % 36) * base;
      base *= 36;
    }
    lot_part >>= 6;
  }

  uint64_t mac = (uint64_t)(lower_part & 0xFFFFF) |
                 (((uint64_t)higher_part & 0xFFFFFFF) << 20);
  if (mac != 0U) {
    memcpy(mac_addr, &mac, 6);
    return true;
  }

  // ULP boards may expose a UID that is neither v2-formatted nor suitable for
  // the v1 field split above. Derive a stable locally-administered address
  // from the complete UID instead of leaving the controller with all zeros.
  uint32_t hash = 2166136261U;
  for (i = 0U; i < sizeof(uid); i++) {
    hash = (hash ^ uid[i]) * 16777619U;
  }

  for (i = 0U; i < 6U; i++) {
    mac_addr[i] = (uint8_t)(hash >> ((i & 3U) * 8U));
  }
  mac_addr[0] = (mac_addr[0] & 0xFEU) | 0x02U;

  return true;
}

void lcpu_custom_nvds_config(void) {
  uint8_t *nvds_addr = (uint8_t *)NVDS_BUFF_START;
  uint8_t mac_addr[6];
  bool res;

  // The NVDS lives in LPSYS RAM. Hold the LCPU wake request while HCPU writes
  // it, matching the vendor stack's bt_stack_nvds_init() sequence.
  HAL_HPAON_WakeCore(CORE_ID_LCPU);

  res = prv_bt_mac_addr_generate(mac_addr);
  assert(res);

  *(uint32_t *)nvds_addr = 0x4E564453;
  *(uint16_t *)(nvds_addr + 4) = sizeof(s_ble_slp_default) + 8U;
  *(uint16_t *)(nvds_addr + 6) = 0;

  *(uint8_t *)(nvds_addr + 8) = 0x01;
  *(uint8_t *)(nvds_addr + 9) = 0x06;
  memcpy(nvds_addr + 10, mac_addr, 6);

  memcpy(nvds_addr + 16, s_ble_slp_default, sizeof(s_ble_slp_default));

  __DSB();
  assert(*(volatile uint32_t *)nvds_addr == 0x4E564453U);
  HAL_HPAON_CANCEL_LP_ACTIVE_REQUEST();
}
