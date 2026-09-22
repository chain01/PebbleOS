/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include <stdbool.h>
#include <stdint.h>

#include "board/board.h"
#include "bf0_hal.h"
#include "kernel/events.h"
#include "kernel/util/sleep.h"
#include <pbl/drivers/battery.h>
#include <pbl/drivers/exti.h>
#include <pbl/drivers/gpio.h>
#include <pbl/drivers/i2c.h>
#include "pbl/kernel/mutex.h"
#include "pbl/services/new_timer/new_timer.h"
#include "pbl/services/system_task.h"
#include <pbl/logging/logging.h>

PBL_LOG_MODULE_DEFINE(driver_battery_aw32001, CONFIG_DRIVER_BATTERY_LOG_LEVEL);

#define AW32001_REG_POWERON_CONF        0x01U
#define AW32001_REG_CHARGE_VOLTAGE      0x04U
#define AW32001_REG_TIMER_CTRL          0x05U
#define AW32001_REG_SYS_STATUS          0x08U

#define AW32001_CEB_DISABLE       (1U << 3U)
#define AW32001_WDT_MASK          (3U << 5U)
#define AW32001_STATUS_MASK       (3U << 3U)
#define AW32001_TARGET_VOLTAGE_MV 4215U
#define AW32001_CHARGER_DEBOUNCE_MS 100U

#define BATTERY_ADC_CHANNEL       7U
#define BATTERY_ADC_SAMPLE_COUNT  22U
#define BATTERY_ADC_DELAY_MS      10U
#define BATTERY_ADC_FALLBACK_MV   4000

static TimerID s_charger_debounce_timer = TIMER_INVALID_ID;
static uint32_t s_battery_mv = BATTERY_ADC_FALLBACK_MV;

static ADC_HandleTypeDef s_adc = {
  .Instance = hwp_gpadc1,
};
static HAL_ADC_CalibContextTypeDef s_adc_calib;
static bool s_adc_ready;
static PBL_MUTEX_DEFINE(s_adc_mutex);

static bool prv_read_register(uint8_t reg, uint8_t *value) {
  i2c_use(I2C_AW32001);
  const bool result = i2c_read_register(I2C_AW32001, reg, value);
  i2c_release(I2C_AW32001);
  return result;
}

static bool prv_write_register(uint8_t reg, uint8_t value) {
  i2c_use(I2C_AW32001);
  const bool result = i2c_write_register(I2C_AW32001, reg, value);
  i2c_release(I2C_AW32001);
  return result;
}

static bool prv_update_register(uint8_t reg, uint8_t mask, uint8_t value) {
  uint8_t current;
  if (!prv_read_register(reg, &current)) {
    return false;
  }
  current = (current & ~mask) | (value & mask);
  return prv_write_register(reg, current);
}

static bool prv_usb_connected_raw(void) {
  const InputConfig usb_detect = {
    .gpio = BOARD_CONFIG_POWER.pmic_int.peripheral,
    .gpio_pin = BOARD_CONFIG_POWER.pmic_int.gpio_pin,
  };
  // The ULP VBUS_DET line is active high.
  return gpio_input_read(&usb_detect);
}

static void prv_charger_debounce_cb(void *unused) {
  const bool connected = prv_usb_connected_raw();
  PebbleEvent event = {
    .type = PEBBLE_BATTERY_CONNECTION_EVENT,
    .battery_connection = {
      .is_connected = connected,
    },
  };
  event_put(&event);
}

static void prv_schedule_charger_update(void *unused) {
  if (s_charger_debounce_timer != TIMER_INVALID_ID) {
    new_timer_start(s_charger_debounce_timer, AW32001_CHARGER_DEBOUNCE_MS,
                    prv_charger_debounce_cb, NULL, 0 /* flags */);
  } else {
    prv_charger_debounce_cb(NULL);
  }
}

static void prv_charger_interrupt_handler(bool *should_context_switch) {
  system_task_add_callback_from_isr(prv_schedule_charger_update, NULL, should_context_switch);
}

static bool prv_adc_init(void) {
  s_adc.Init.data_samp_delay = 2;
  s_adc.Init.conv_width = 75;
  s_adc.Init.sample_width = 71;
  s_adc.Init.adc_se = 1;
  s_adc.Init.adc_force_on = 0;
  s_adc.Init.atten3 = 0;
  s_adc.Init.dma_en = 0;
  s_adc.Init.en_slot = 0;
  s_adc.Init.op_mode = 0;

  HAL_RCC_EnableModule(RCC_MOD_GPADC);
  if (HAL_ADC_Init(&s_adc) != HAL_OK || HAL_ADC_SetFreq(&s_adc, 240000U) == 0U) {
    return false;
  }

  HAL_ADC_CalibInit(&s_adc_calib);
  if (HAL_ADC_CalibLoad(&s_adc, &s_adc_calib, HAL_ADC_CALIB_SOURCE_BSP,
                        HAL_ADC_CALIB_F_INIT | HAL_ADC_CALIB_F_APPLY) != HAL_OK) {
    PBL_LOG_WRN("AW32001 ADC calibration unavailable; using defaults");
    HAL_ADC_CalibInit(&s_adc_calib);
  }

  ADC_ChannelConfTypeDef channel = {
    .Channel = BATTERY_ADC_CHANNEL,
    .pchnl_sel = BATTERY_ADC_CHANNEL,
    .slot_en = 1,
    .acc_num = 0,
  };
  if (HAL_ADC_ConfigChannel(&s_adc, &channel) != HAL_OK) {
    return false;
  }

  psleep(300);
  return true;
}

static bool prv_adc_read_mv(uint32_t *result_mv) {
  uint32_t samples[BATTERY_ADC_SAMPLE_COUNT] = {0};

  if (HAL_ADC_Start(&s_adc) != HAL_OK) {
    return false;
  }

  uint32_t total = 0;
  for (uint32_t i = 0; i < BATTERY_ADC_SAMPLE_COUNT; i++) {
    if (i != 0U) {
      ADC_SET_UNMUTE(&s_adc);
      HAL_Delay_us(200);
      __HAL_ADC_START_CONV(&s_adc);
    }

    if (HAL_ADC_PollForConversion(&s_adc, 100) != HAL_OK) {
      HAL_ADC_Stop(&s_adc);
      return false;
    }

    samples[i] = HAL_ADC_GetValue(&s_adc, BATTERY_ADC_CHANNEL);
    total += samples[i];
    ADC_SET_MUTE(&s_adc);
    psleep(BATTERY_ADC_DELAY_MS);
  }

  HAL_ADC_Stop(&s_adc);

  for (uint32_t i = 0; i < BATTERY_ADC_SAMPLE_COUNT - 1U; i++) {
    for (uint32_t j = 0; j < BATTERY_ADC_SAMPLE_COUNT - 1U - i; j++) {
      if (samples[j] > samples[j + 1U]) {
        const uint32_t swap = samples[j];
        samples[j] = samples[j + 1U];
        samples[j + 1U] = swap;
      }
    }
  }

  total -= samples[0];
  total -= samples[BATTERY_ADC_SAMPLE_COUNT - 1U];
  const float average = (float)total / (BATTERY_ADC_SAMPLE_COUNT - 2U);
  float voltage = HAL_ADC_RegToVoltageFloat(average, &s_adc_calib);
  if (BATTERY_ADC_CHANNEL == 7U) {
    voltage *= s_adc_calib.vbat_factor;
  }
  *result_mv = (uint32_t)(voltage + 0.5f);
  PBL_LOG_DBG("AW32001 ADC raw=%u voltage=%u mV", (unsigned)average,
              (unsigned)*result_mv);
  return true;
}

static void prv_read_battery_voltage(void) {
  if (!s_adc_ready) {
    return;
  }

  uint32_t voltage_mv;
  pbl_mutex_lock(&s_adc_mutex, PBL_FOREVER);
  const bool success = prv_adc_read_mv(&voltage_mv);
  pbl_mutex_unlock(&s_adc_mutex);

  if (!success) {
    PBL_LOG_ERR("AW32001 battery ADC read failed");
  } else if (voltage_mv < 2500U || voltage_mv > 5000U) {
    PBL_LOG_WRN("AW32001 battery ADC out of range: %u mV", (unsigned)voltage_mv);
  } else {
    s_battery_mv = voltage_mv;
  }
}

void battery_init(void) {
  uint8_t poweron = 0;
  if (prv_read_register(AW32001_REG_POWERON_CONF, &poweron)) {
    PBL_LOG_DBG("AW32001 detected");
  } else {
    PBL_LOG_ERR("AW32001 not detected");
  }

  if (!prv_update_register(AW32001_REG_TIMER_CTRL, AW32001_WDT_MASK, 0)) {
    PBL_LOG_ERR("AW32001 watchdog disable failed");
  }
  if (!prv_update_register(AW32001_REG_POWERON_CONF, AW32001_CEB_DISABLE, 0)) {
    PBL_LOG_ERR("AW32001 charge enable failed");
  }

  uint8_t charge_voltage;
  if (prv_read_register(AW32001_REG_CHARGE_VOLTAGE, &charge_voltage)) {
    const uint8_t level = (uint8_t)((AW32001_TARGET_VOLTAGE_MV - 3600U) / 15U);
    charge_voltage = (uint8_t)((level << 2U) | (charge_voltage & 0x03U));
    if (!prv_write_register(AW32001_REG_CHARGE_VOLTAGE, charge_voltage)) {
      PBL_LOG_ERR("AW32001 charge voltage write failed");
    }
  }

  s_charger_debounce_timer = new_timer_create();
  if (s_charger_debounce_timer == TIMER_INVALID_ID) {
    PBL_LOG_ERR("AW32001 charger debounce timer unavailable");
  }
  exti_configure_pin(BOARD_CONFIG_POWER.pmic_int, ExtiTrigger_RisingFalling,
                     prv_charger_interrupt_handler);
  exti_enable(BOARD_CONFIG_POWER.pmic_int);

  s_adc_ready = prv_adc_init();
  if (!s_adc_ready) {
    PBL_LOG_ERR("AW32001 battery ADC init failed");
  }
  prv_read_battery_voltage();
  uint8_t status = 0;
  (void)prv_read_register(AW32001_REG_SYS_STATUS, &status);
  const InputConfig usb_detect = {
    .gpio = BOARD_CONFIG_POWER.pmic_int.peripheral,
    .gpio_pin = BOARD_CONFIG_POWER.pmic_int.gpio_pin,
  };
  PBL_LOG_INFO("AW32001 ready: %u mV, status=0x%02x, vbus_det=%d, plugged=%d",
               (unsigned)s_battery_mv, status, (int)gpio_input_read(&usb_detect),
               (int)battery_is_usb_connected_impl());
}

int battery_get_millivolts(void) {
  prv_read_battery_voltage();
  return (int)s_battery_mv;
}

int battery_get_constants(BatteryConstants *constants) {
  *constants = (BatteryConstants) {
    .v_mv = (int32_t)s_battery_mv,
    .i_ua = 0,
    .t_mc = 25000,
  };
  return 0;
}

bool battery_charge_controller_thinks_we_are_charging_impl(void) {
  uint8_t status;
  if (!prv_read_register(AW32001_REG_SYS_STATUS, &status)) {
    return false;
  }
  return (status & AW32001_STATUS_MASK) != 0U;
}

bool battery_is_usb_connected_impl(void) {
  return prv_usb_connected_raw();
}

void battery_set_charge_enable(bool charging_enabled) {
  const uint8_t value = charging_enabled ? 0U : AW32001_CEB_DISABLE;
  if (!prv_update_register(AW32001_REG_POWERON_CONF, AW32001_CEB_DISABLE, value)) {
    PBL_LOG_ERR("AW32001 charge state write failed");
  }
}

void battery_set_fast_charge(bool fast_charge_enabled) {
  // The charger current is set by the board's hardware straps. Keep the
  // vendor default until a validated battery/charge-current profile exists.
  (void)fast_charge_enabled;
}

ADCVoltageMonitorReading battery_read_voltage_monitor(void) {
  return (ADCVoltageMonitorReading) {
    .vref_total = 0,
    .vmon_total = 0,
  };
}

uint32_t battery_convert_reading_to_millivolts(ADCVoltageMonitorReading reading,
                                               uint32_t numerator,
                                               uint32_t denominator) {
  (void)reading;
  (void)numerator;
  (void)denominator;
  return s_battery_mv;
}

int battery_charge_status_get(BatteryChargeStatus *status) {
  uint8_t charger_status;
  if (!prv_read_register(AW32001_REG_SYS_STATUS, &charger_status)) {
    *status = BatteryChargeStatusUnknown;
    return -1;
  }

  switch ((charger_status & AW32001_STATUS_MASK) >> 3U) {
    case 1:
      *status = BatteryChargeStatusTrickle;
      break;
    case 2:
      *status = BatteryChargeStatusCC;
      break;
    case 3:
      *status = BatteryChargeStatusComplete;
      break;
    default:
      *status = BatteryChargeStatusUnknown;
      break;
  }
  return 0;
}
