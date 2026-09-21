/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <pbl/drivers/display/display.h>
#include <pbl/drivers/gpio.h>
#include <pbl/logging/logging.h>
#include "kernel/util/delay.h"
#include "pbl/kernel/types.h"
#include "pbl/mcu/cache.h"
#include "system/passert.h"

#include "bf0_hal.h"
#include "bf0_hal_epic.h"
#include "bf0_hal_ezip.h"
#include "bf0_hal_lcdc.h"
#include "bf0_hal_i2c.h"

PBL_LOG_MODULE_DEFINE(display_co5300, CONFIG_DRIVER_DISPLAY_LOG_LEVEL);

#define CO5300_REG_LCD_ID             0x04
#define CO5300_REG_SLEEP_OUT          0x11
#define CO5300_REG_DISPLAY_OFF        0x28
#define CO5300_REG_DISPLAY_ON         0x29
#define CO5300_REG_WRITE_RAM          0x2C
#define CO5300_REG_CASET              0x2A
#define CO5300_REG_RASET              0x2B
#define CO5300_REG_TEARING_EFFECT_ON  0x35
#define CO5300_REG_COLOR_MODE         0x3A
#define CO5300_REG_WBRIGHT            0x51
#define CO5300_REG_WRITE_CTRL_DISPLAY 0x53
#define CO5300_REG_WRHBMDISBV         0x63
#define CO5300_REG_SET_SPI_MODE       0xC4
#define CO5300_REG_PASSWD1            0xF4
#define CO5300_REG_PASSWD2            0xF5
#define CO5300_REG_CMD_PAGE_SWITCH    0xFE

#define CO5300_PANEL_WIDTH  390
#define CO5300_PANEL_HEIGHT 450
#define CO5300_PANEL_FRAMEBUFFER_BYTES (CO5300_PANEL_WIDTH * CO5300_PANEL_HEIGHT * 2U)
#define CO5300_PANEL_FRAMEBUFFER_ADDR (QSPI1_MEM_BASE + 0x100000U)

/* EPIC scaling is performed in 16-source-row strips. EPIC cannot reliably
 * access the PSRAM framebuffer directly on this SDK revision, so each scaled
 * strip is copied from internal SRAM into the PSRAM full-frame buffer. */
#define CO5300_SOURCE_STRIP_HEIGHT 16U
#define CO5300_OUTPUT_STRIP_MAX_HEIGHT 32U

static LCDC_HandleTypeDef s_hlcdc = {
  .Instance = LCDC1,
};

static volatile bool s_layer_transfer_done;
static bool s_initialized;
static bool s_enabled = true;
static bool s_rotated;
static volatile bool s_updating;

static uint16_t s_source_framebuffer[PBL_DISPLAY_WIDTH * PBL_DISPLAY_HEIGHT];
static uint16_t s_scaled_strip[CO5300_PANEL_WIDTH * CO5300_OUTPUT_STRIP_MAX_HEIGHT];
static uint16_t *const s_panel_framebuffer = (uint16_t *)CO5300_PANEL_FRAMEBUFFER_ADDR;
static uint16_t s_color_palette[256];
static uint16_t s_xmap[CO5300_PANEL_WIDTH];

static EPIC_TypeDef s_epic_ram;
static EZIP_HandleTypeDef s_ezip = {
  .Instance = EZIP,
};
static EPIC_HandleTypeDef s_epic = {
  .Instance = EPIC,
  .RamInstance = &s_epic_ram,
  .hezip = &s_ezip,
};
static bool s_gpu_ready;

static void prv_charger_power_up(void);
extern bool board_psram_init(void);

void HAL_LCDC_SendLayerDataCpltCbk(LCDC_HandleTypeDef *lcdc) {
  (void)lcdc;
  s_layer_transfer_done = true;
}

void display_co5300_irq_handler(void *ctx) {
  (void)ctx;
  HAL_LCDC_IRQHandler(&s_hlcdc);
}

static void prv_send_layer_data(uint32_t command) {
  s_layer_transfer_done = false;
  const HAL_StatusTypeDef status = HAL_LCDC_SendLayerData2Reg_IT(&s_hlcdc, command, 4);
  if (status != HAL_OK) {
    PBL_LOG_ERR("CO5300 layer transfer start failed: %d", (int)status);
    return;
  }

  const uint32_t start = HAL_GetTick();
  while (!s_layer_transfer_done && (HAL_GetTick() - start) < 100U) {
    __NOP();
  }
  if (!s_layer_transfer_done) {
    PBL_LOG_ERR("CO5300 layer transfer timed out");
  }
}

static void prv_write_cmd(uint16_t reg, const uint8_t *parameters, uint32_t count) {
  const uint32_t cmd = (0x02U << 24) | ((uint32_t)reg << 8);
  HAL_LCDC_WriteU32Reg(&s_hlcdc, cmd, (uint8_t *)parameters, count);
}

static void prv_write_region_physical(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
  uint8_t data[4];

  data[0] = x0 >> 8;
  data[1] = x0 & 0xFF;
  data[2] = x1 >> 8;
  data[3] = x1 & 0xFF;
  prv_write_cmd(CO5300_REG_CASET, data, sizeof(data));

  data[0] = y0 >> 8;
  data[1] = y0 & 0xFF;
  data[2] = y1 >> 8;
  data[3] = y1 & 0xFF;
  prv_write_cmd(CO5300_REG_RASET, data, sizeof(data));
}

static void prv_send_full_frame(void) {
  uintptr_t flush_addr = (uintptr_t)s_panel_framebuffer;
  size_t flush_size = CO5300_PANEL_FRAMEBUFFER_BYTES;
  dcache_align(&flush_addr, &flush_size);
  dcache_flush((const void *)flush_addr, flush_size);

  HAL_LCDC_SetROIArea(&s_hlcdc, 0, 0, CO5300_PANEL_WIDTH - 1U, CO5300_PANEL_HEIGHT - 1U);
  prv_write_region_physical(0, 0, CO5300_PANEL_WIDTH - 1U, CO5300_PANEL_HEIGHT - 1U);
  HAL_LCDC_LayerSetData(&s_hlcdc, HAL_LCDC_LAYER_DEFAULT, (uint8_t *)s_panel_framebuffer, 0, 0,
                        CO5300_PANEL_WIDTH - 1U, CO5300_PANEL_HEIGHT - 1U);
  prv_send_layer_data(((0x32U << 24) | (CO5300_REG_WRITE_RAM << 8)));
}

static uint16_t prv_rgb222_to_rgb565(uint8_t pixel) {
  const uint8_t r = (pixel >> 4) & 0x03U;
  const uint8_t g = (pixel >> 2) & 0x03U;
  const uint8_t b = pixel & 0x03U;
  const uint8_t r5 = (uint8_t)((r << 3) | (r << 1) | (r >> 1));
  const uint8_t g6 = (uint8_t)((g << 4) | (g << 2) | g);
  const uint8_t b5 = (uint8_t)((b << 3) | (b << 1) | (b >> 1));
  return (uint16_t)((r5 << 11) | (g6 << 5) | b5);
}

static void prv_init_scaling_tables(void) {
  for (uint32_t pixel = 0; pixel < 256U; pixel++) {
    s_color_palette[pixel] = prv_rgb222_to_rgb565((uint8_t)pixel);
  }
  for (uint16_t x = 0; x < CO5300_PANEL_WIDTH; x++) {
    uint32_t src_x = ((uint32_t)x * PBL_DISPLAY_WIDTH) / CO5300_PANEL_WIDTH;
    if (src_x >= PBL_DISPLAY_WIDTH) {
      src_x = PBL_DISPLAY_WIDTH - 1U;
    }
    s_xmap[x] = (uint16_t)src_x;
  }
}

static void prv_update_source_row(uint16_t y, const uint8_t *source) {
  uint16_t *dst = &s_source_framebuffer[(uint32_t)y * PBL_DISPLAY_WIDTH];
  if (s_rotated) {
    for (uint16_t x = 0; x < PBL_DISPLAY_WIDTH; x++) {
      dst[PBL_DISPLAY_WIDTH - 1U - x] = s_color_palette[source[x]];
    }
  } else {
    for (uint16_t x = 0; x < PBL_DISPLAY_WIDTH; x++) {
      dst[x] = s_color_palette[source[x]];
    }
  }
}

typedef struct {
  int16_t x1;
  int16_t y1;
} EpicTransBounds;

static int16_t prv_epic_hpath(int16_t x, EPIC_LayerConfigTypeDef *layer, void *user_data) {
  (void)x;
  (void)layer;
  return ((EpicTransBounds *)user_data)->x1;
}

static int16_t prv_epic_vpath(int16_t y, EPIC_LayerConfigTypeDef *layer, void *user_data) {
  (void)y;
  (void)layer;
  return ((EpicTransBounds *)user_data)->y1;
}

static bool prv_gpu_scale_strip(uint16_t src_y0, uint16_t src_rows, uint16_t out_y0,
                                uint16_t out_rows) {
  EPIC_LayerConfigTypeDef input;
  EPIC_LayerConfigTypeDef output;
  EpicTransBounds bounds = {
      .x1 = CO5300_PANEL_WIDTH - 1,
      .y1 = out_rows - 1,
  };

  HAL_EPIC_LayerConfigInit(&input);
  input.data = (uint8_t *)&s_source_framebuffer[(uint32_t)src_y0 * PBL_DISPLAY_WIDTH];
  input.color_mode = EPIC_COLOR_RGB565;
  input.width = PBL_DISPLAY_WIDTH;
  input.height = src_rows;
  input.total_width = PBL_DISPLAY_WIDTH;
  input.transform_cfg.scale_x =
      ((uint32_t)PBL_DISPLAY_WIDTH * EPIC_INPUT_SCALE_NONE + CO5300_PANEL_WIDTH / 2U) /
      CO5300_PANEL_WIDTH;
  input.transform_cfg.scale_y =
      ((uint32_t)src_rows * EPIC_INPUT_SCALE_NONE + out_rows / 2U) / out_rows;

  HAL_EPIC_LayerConfigInit(&output);
  output.data = (uint8_t *)s_scaled_strip;
  output.color_mode = EPIC_COLOR_RGB565;
  output.width = CO5300_PANEL_WIDTH;
  output.height = out_rows;
  output.total_width = CO5300_PANEL_WIDTH;

  const HAL_StatusTypeDef status =
      HAL_EPIC_TransStart(&s_epic, &input, 1, &output, prv_epic_hpath, prv_epic_vpath, &bounds);
  if (status != HAL_OK) {
    return false;
  }

  // EPIC pitch scaling may leave the first/last sample row untouched when the
  // strip height is not an integer multiple. Replicate the adjacent row so a
  // stale black line does not appear at each 16-source-row boundary.
  if (out_rows >= 2U) {
    memcpy(&s_scaled_strip[0], &s_scaled_strip[CO5300_PANEL_WIDTH],
           CO5300_PANEL_WIDTH * sizeof(uint16_t));
    memcpy(&s_scaled_strip[(uint32_t)(out_rows - 1U) * CO5300_PANEL_WIDTH],
           &s_scaled_strip[(uint32_t)(out_rows - 2U) * CO5300_PANEL_WIDTH],
           CO5300_PANEL_WIDTH * sizeof(uint16_t));
  }

  memcpy(&s_panel_framebuffer[(uint32_t)out_y0 * CO5300_PANEL_WIDTH], s_scaled_strip,
         (uint32_t)out_rows * CO5300_PANEL_WIDTH * sizeof(uint16_t));
  return true;
}

static bool prv_gpu_scale(void) {
  if (!s_gpu_ready) {
    return false;
  }

  for (uint16_t src_y0 = 0; src_y0 < PBL_DISPLAY_HEIGHT; src_y0 += CO5300_SOURCE_STRIP_HEIGHT) {
    const uint16_t remaining = (uint16_t)(PBL_DISPLAY_HEIGHT - src_y0);
    const uint16_t src_rows =
        remaining < CO5300_SOURCE_STRIP_HEIGHT ? remaining : CO5300_SOURCE_STRIP_HEIGHT;
    const uint16_t out_y0 =
        (uint16_t)(((uint32_t)src_y0 * CO5300_PANEL_HEIGHT + PBL_DISPLAY_HEIGHT - 1U) /
                   PBL_DISPLAY_HEIGHT);
    const uint16_t out_y1 =
        (uint16_t)(((uint32_t)(src_y0 + src_rows) * CO5300_PANEL_HEIGHT + PBL_DISPLAY_HEIGHT - 1U) /
                   PBL_DISPLAY_HEIGHT);
    const uint16_t out_rows = out_y1 - out_y0;

    if (!prv_gpu_scale_strip(src_y0, src_rows, out_y0, out_rows)) {
      return false;
    }
  }
  return true;
}

static void prv_render_output_row_cpu(uint16_t panel_y) {
  uint16_t *dst = &s_panel_framebuffer[(uint32_t)panel_y * CO5300_PANEL_WIDTH];
  uint32_t src_y = ((uint32_t)panel_y * PBL_DISPLAY_HEIGHT) / CO5300_PANEL_HEIGHT;
  if (src_y >= PBL_DISPLAY_HEIGHT) {
    src_y = PBL_DISPLAY_HEIGHT - 1U;
  }
  const uint16_t *src_row = &s_source_framebuffer[src_y * PBL_DISPLAY_WIDTH];
  for (uint16_t x = 0; x < CO5300_PANEL_WIDTH; x++) {
    dst[x] = src_row[s_xmap[x]];
  }
}

static void prv_render_full_frame(void) {
  uintptr_t flush_addr = (uintptr_t)s_panel_framebuffer;
  size_t flush_size = CO5300_PANEL_FRAMEBUFFER_BYTES;
  dcache_align(&flush_addr, &flush_size);
  dcache_flush((const void *)s_source_framebuffer, sizeof(s_source_framebuffer));
  dcache_flush_invalidate((const void *)flush_addr, flush_size);

  if (!s_gpu_ready || !prv_gpu_scale()) {
    if (s_gpu_ready) {
      PBL_LOG_ERR("ULP GPU scale failed; falling back to CPU");
      s_gpu_ready = false;
      HAL_RCC_ResetModule(RCC_MOD_EPIC);
    }
    for (uint16_t y = 0; y < CO5300_PANEL_HEIGHT; y++) {
      prv_render_output_row_cpu(y);
    }
  }

  prv_send_full_frame();
}

static void prv_set_power_pin(uint32_t gpio_pin, bool level) {
  OutputConfig config = {
    .gpio = hwp_gpio1,
    .gpio_pin = gpio_pin,
    .active_high = true,
  };
  gpio_output_init(&config, GPIO_OType_PP);
  gpio_output_set(&config, level);
}

static void prv_pins_init(void) {
  HAL_Delay_us(500);

  HAL_PIN_Set(PAD_PA03, LCDC1_SPI_CS, PIN_NOPULL, 1);
  HAL_PIN_Set(PAD_PA04, LCDC1_SPI_CLK, PIN_NOPULL, 1);
  HAL_PIN_Set(PAD_PA05, LCDC1_SPI_DIO0, PIN_NOPULL, 1);
  HAL_PIN_Set(PAD_PA06, LCDC1_SPI_DIO1, PIN_NOPULL, 1);
  HAL_PIN_Set(PAD_PA07, LCDC1_SPI_DIO2, PIN_NOPULL, 1);
  HAL_PIN_Set(PAD_PA08, LCDC1_SPI_DIO3, PIN_NOPULL, 1);
  HAL_PIN_Set(PAD_PA02, LCDC1_SPI_TE, PIN_NOPULL, 1);

  HAL_PIN_Set(PAD_PA00, GPIO_A0, PIN_NOPULL, 1);
  HAL_PIN_Set(PAD_PA01, GPIO_A1, PIN_NOPULL, 1);
  HAL_PIN_Set(PAD_PA26, GPIO_A26, PIN_NOPULL, 1);
  HAL_PIN_Set(PAD_PA38, GPIO_A38, PIN_NOPULL, 1);
  HAL_PIN_Set(PAD_PA42, GPIO_A42, PIN_PULLUP, 1);
  HAL_PIN_Set(PAD_PA10, I2C2_SCL, PIN_PULLUP, 1);
  HAL_PIN_Set(PAD_PA11, I2C2_SDA, PIN_PULLUP, 1);

  prv_set_power_pin(0, true);
  prv_set_power_pin(1, true);
  prv_set_power_pin(26, true);
  prv_set_power_pin(38, true);
  prv_set_power_pin(42, true);
  HAL_Delay_us(500);
  prv_charger_power_up();
}

static void prv_charger_power_up(void) {
  I2C_HandleTypeDef i2c = {0};
  i2c.Instance = I2C2;
  i2c.Mode = HAL_I2C_MODE_MASTER;
  i2c.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
  i2c.Init.ClockSpeed = 400000;
  i2c.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
  i2c.Init.OwnAddress1 = 0;

  HAL_RCC_EnableModule(RCC_MOD_I2C2);
  if (HAL_I2C_Init(&i2c) != HAL_OK) {
    PBL_LOG_ERR("CO5300 charger I2C init failed");
    return;
  }
  __HAL_I2C_ENABLE(&i2c);

  uint8_t data = 0;
  if (HAL_I2C_Mem_Read(&i2c, 0x49, 0x01, I2C_MEMADD_SIZE_8BIT, &data, 1, 1000) == HAL_OK) {
    data &= ~(1U << 3U);
    (void)HAL_I2C_Mem_Write(&i2c, 0x49, 0x01, I2C_MEMADD_SIZE_8BIT, &data, 1, 1000);
  }
  if (HAL_I2C_Mem_Read(&i2c, 0x49, 0x05, I2C_MEMADD_SIZE_8BIT, &data, 1, 1000) == HAL_OK) {
    data &= ~(3U << 5U);
    (void)HAL_I2C_Mem_Write(&i2c, 0x49, 0x05, I2C_MEMADD_SIZE_8BIT, &data, 1, 1000);
  }
  __HAL_I2C_DISABLE(&i2c);
}

static void prv_reset_panel(void) {
  OutputConfig reset = {
    .gpio = hwp_gpio1,
    .gpio_pin = 0,
    .active_high = true,
  };
  gpio_output_init(&reset, GPIO_OType_PP);

  gpio_output_set(&reset, true);
  HAL_Delay_us(10000);
  gpio_output_set(&reset, false);
  HAL_Delay_us(10000);
  gpio_output_set(&reset, true);
  HAL_Delay_us(50000);
}

static uint32_t prv_read_id(void) {
  uint32_t id = 0;
  HAL_LCDC_SetFreq(&s_hlcdc, 2000000);
  HAL_LCDC_ReadU32Reg(&s_hlcdc, ((0x03U << 24) | (CO5300_REG_LCD_ID << 8)), (uint8_t *)&id, 3);
  HAL_LCDC_SetFreq(&s_hlcdc, 50000000);
  return id & 0xFFFFFFU;
}

static void prv_send_init_sequence(void) {
  uint8_t parameter[4];

  parameter[0] = 0x20;
  prv_write_cmd(CO5300_REG_CMD_PAGE_SWITCH, parameter, 1);
  parameter[0] = 0x5A;
  prv_write_cmd(CO5300_REG_PASSWD1, parameter, 1);
  parameter[0] = 0x59;
  prv_write_cmd(CO5300_REG_PASSWD2, parameter, 1);

  parameter[0] = 0x20;
  prv_write_cmd(CO5300_REG_CMD_PAGE_SWITCH, parameter, 1);
  parameter[0] = 0xA5;
  prv_write_cmd(CO5300_REG_PASSWD1, parameter, 1);
  parameter[0] = 0xA5;
  prv_write_cmd(CO5300_REG_PASSWD2, parameter, 1);

  parameter[0] = 0x00;
  prv_write_cmd(CO5300_REG_CMD_PAGE_SWITCH, parameter, 1);
  parameter[0] = 0x80;
  prv_write_cmd(CO5300_REG_SET_SPI_MODE, parameter, 1);
  parameter[0] = 0x55;
  prv_write_cmd(CO5300_REG_COLOR_MODE, parameter, 1);
  parameter[0] = 0x00;
  prv_write_cmd(CO5300_REG_TEARING_EFFECT_ON, parameter, 1);
  parameter[0] = 0x20;
  prv_write_cmd(CO5300_REG_WRITE_CTRL_DISPLAY, parameter, 1);
  parameter[0] = 0xFF;
  prv_write_cmd(CO5300_REG_WRHBMDISBV, parameter, 1);

  parameter[0] = 0;
  parameter[1] = 0;
  parameter[2] = (CO5300_PANEL_WIDTH - 1U) >> 8;
  parameter[3] = (CO5300_PANEL_WIDTH - 1U) & 0xFF;
  prv_write_cmd(CO5300_REG_CASET, parameter, 4);

  parameter[0] = 0;
  parameter[1] = 0;
  parameter[2] = (CO5300_PANEL_HEIGHT - 1U) >> 8;
  parameter[3] = (CO5300_PANEL_HEIGHT - 1U) & 0xFF;
  prv_write_cmd(CO5300_REG_RASET, parameter, 4);

  parameter[0] = 0xD5;
  prv_write_cmd(CO5300_REG_COLOR_MODE, parameter, 1);

  prv_write_cmd(CO5300_REG_SLEEP_OUT, NULL, 0);
  HAL_Delay_us(120000);
  prv_write_cmd(CO5300_REG_DISPLAY_ON, NULL, 0);
  HAL_Delay_us(70000);

  parameter[0] = 0xFF;
  prv_write_cmd(CO5300_REG_WBRIGHT, parameter, 1);
}

void display_init(void) {
  if (s_initialized) {
    return;
  }

  const bool psram_ok = board_psram_init();
  PBL_LOG_INFO("ULP PSRAM ready=%d", (int)psram_ok);
  PBL_ASSERTN(psram_ok);

  s_gpu_ready = HAL_EZIP_Init(&s_ezip) == HAL_OK && HAL_EPIC_Init(&s_epic) == HAL_OK;
  PBL_LOG_INFO("ULP GPU ready=%d", (int)s_gpu_ready);

  prv_pins_init();

  LCDC_InitTypeDef config = {
    .lcd_itf = LCDC_INTF_SPI_DCX_4DATA,
    .color_mode = LCDC_PIXEL_FORMAT_RGB565,
    .freq = 50000000,
    .cfg.spi = {
      .dummy_clock = 0,
      .syn_mode = HAL_LCDC_SYNC_VER,
      .vsyn_polarity = 1,
      .vsyn_delay_us = 0,
      .hsyn_num = 0,
    },
  };
  s_hlcdc.Init = config;

  HAL_LCDC_Init(&s_hlcdc);
  HAL_NVIC_SetPriority(LCDC1_IRQn, 6, 0);
  HAL_NVIC_EnableIRQ(LCDC1_IRQn);
  HAL_LCDC_LayerReset(&s_hlcdc, HAL_LCDC_LAYER_DEFAULT);
  HAL_LCDC_LayerSetFormat(&s_hlcdc, HAL_LCDC_LAYER_DEFAULT, LCDC_PIXEL_FORMAT_RGB565);
  HAL_LCDC_LayerEnable(&s_hlcdc, HAL_LCDC_LAYER_DEFAULT);

  prv_reset_panel();
  const uint32_t lcd_id = prv_read_id();
  prv_send_init_sequence();
  prv_init_scaling_tables();
  memset(s_source_framebuffer, 0, sizeof(s_source_framebuffer));
  prv_render_full_frame();

  PBL_LOG_INFO("CO5300 ready: id=0x%06lx", (unsigned long)lcd_id);
  s_initialized = true;
  s_enabled = true;
}

void display_set_enabled(bool enabled) {
  if (!s_initialized || s_enabled == enabled) {
    return;
  }
  prv_write_cmd(enabled ? CO5300_REG_DISPLAY_ON : CO5300_REG_DISPLAY_OFF, NULL, 0);
  s_enabled = enabled;
}

void display_set_rotated(bool rotated) {
  s_rotated = rotated;
}

bool display_update_in_progress(void) {
  return s_updating;
}

void display_update(NextRowCallback nrcb, UpdateCompleteCallback uccb) {
  s_updating = true;
  DisplayRow row;
  bool updated = false;

  while (nrcb(&row)) {
    prv_update_source_row(row.address, row.data);
    updated = true;
  }

  if (updated) {
    prv_render_full_frame();
  }

  s_updating = false;
  if (uccb) {
    uccb();
  }
}

void display_update_boot_frame(uint8_t *framebuffer) {
  s_updating = true;
  if (s_rotated) {
    for (uint16_t y = 0; y < PBL_DISPLAY_HEIGHT; y++) {
      prv_update_source_row(y, &framebuffer[(uint32_t)y * PBL_DISPLAY_WIDTH]);
    }
  } else {
    for (uint16_t y = 0; y < PBL_DISPLAY_HEIGHT; y++) {
      prv_update_source_row(y, &framebuffer[(uint32_t)y * PBL_DISPLAY_WIDTH]);
    }
  }
  prv_render_full_frame();
  s_updating = false;
}

void display_clear(void) {
  if (s_initialized) {
    s_updating = true;
    memset(s_source_framebuffer, 0, sizeof(s_source_framebuffer));
    prv_render_full_frame();
    s_updating = false;
  }
}
