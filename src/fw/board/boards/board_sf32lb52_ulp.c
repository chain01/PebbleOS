/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "board/board.h"
#include <pbl/drivers/i2c.h>
#include <pbl/logging/logging.h>
#include "bf0_hal_mpi.h"
#include "bf0_hal_mpi_ex.h"
#include "bf0_hal_pmu.h"


#include <pbl/drivers/sf32lb52/debounced_button_definitions.h>

static UARTDeviceState s_dbg_uart_state = {
  .huart =
      {
        .Instance = USART1,
        .Init =
            {
              .BaudRate = 1000000,
              .WordLength = UART_WORDLENGTH_8B,
              .StopBits = UART_STOPBITS_1,
              .Parity = UART_PARITY_NONE,
              .HwFlowCtl = UART_HWCONTROL_NONE,
              .OverSampling = UART_OVERSAMPLING_16,
            },
      },
  .hdma = {
    .Instance = DMA1_Channel1,
    .Init = {
      .Request = DMA_REQUEST_5,
      .IrqPrio = 5,
    },
  },
};

static UARTDevice DBG_UART_DEVICE = {
  .state = &s_dbg_uart_state,
  .tx =
      {
        .pad = PAD_PA19,
        .func = USART1_TXD,
        .flags = PIN_NOPULL,
      },
  .rx =
      {
        .pad = PAD_PA18,
        .func = USART1_RXD,
        .flags = PIN_PULLUP,
      },
  .irqn = USART1_IRQn,
  .irq_priority = 5,
  .dma_irqn = DMAC1_CH1_IRQn,
  .dma_irq_priority = 5,
};

UARTDevice *const DBG_UART = &DBG_UART_DEVICE;

IRQ_MAP(USART1, uart_irq_handler, DBG_UART);
IRQ_MAP(DMAC1_CH1, uart_dma_irq_handler, DBG_UART);

static QSPIPortState s_qspi_port_state = {
  .cfg =
      {
        .Instance = FLASH2,
        .line = HAL_FLASH_QMODE,
        .base = FLASH2_BASE_ADDR,
        .msize = 16,
        .SpiMode = SPI_MODE_NOR,
      },
  .dma =
      {
        .Instance = DMA1_Channel2,
        .dma_irq = DMAC1_CH2_IRQn,
        .request = DMA_REQUEST_1,
      },
  .t_enter_deep_us = 3,
  .t_exit_deep_us = 20,
};

static QSPIPort QSPI_PORT = {
  .state = &s_qspi_port_state,
  .clk_div = 4U,
};

QSPIPort *const QSPI = &QSPI_PORT;

static QSPIFlashState s_qspi_flash_state;
static QSPIFlash QSPI_FLASH_DEVICE = {
  .state = &s_qspi_flash_state,
  .qspi = &QSPI_PORT,
};

QSPIFlash *const QSPI_FLASH = &QSPI_FLASH_DEVICE;

static I2CBusHalState s_i2c_bus_hal_state_1 = {
  .hdl =
      {
        .Instance = I2C1,
        .Init =
            {
              .AddressingMode = I2C_ADDRESSINGMODE_7BIT,
              .ClockSpeed = 400000,
              .GeneralCallMode = I2C_GENERALCALL_DISABLE,
            },
        .Mode = HAL_I2C_MODE_MASTER,
        .core = CORE_ID_HCPU,
      },
};

static I2CBusHal s_i2c_bus_hal_1 = {
  .state = &s_i2c_bus_hal_state_1,
  .scl =
      {
        .pad = PAD_PA37,
        .func = I2C1_SCL,
        .flags = PIN_NOPULL,
      },
  .sda =
      {
        .pad = PAD_PA33,
        .func = I2C1_SDA,
        .flags = PIN_NOPULL,
      },
  .module = RCC_MOD_I2C1,
  .irqn = I2C1_IRQn,
  .irq_priority = 5,
};

static I2CBusState s_i2c_bus_state_1;

static I2CBus s_i2c_bus_1 = {
  .hal = &s_i2c_bus_hal_1,
  .state = &s_i2c_bus_state_1,
  .name = "i2c1",
};

I2CBus *const I2C1_BUS = &s_i2c_bus_1;
IRQ_MAP(I2C1, i2c_irq_handler, I2C1_BUS);

static const I2CSlavePort s_i2c_ft6146 = {
  .bus = &s_i2c_bus_1,
  .address = 0x38,
};

I2CSlavePort *const I2C_FT6146 = &s_i2c_ft6146;

const BoardConfig BOARD_CONFIG = {
  .backlight_on_percent = 50,
  .ambient_light_dark_threshold = 1,
  .ambient_k_delta_threshold = 1,
  .ambient_light_lux_dark_offset = 0,
  .ambient_light_lux_num = 0,
  .ambient_light_lux_den = 0,
};

const BoardConfigPower BOARD_CONFIG_POWER = {
  .pmic_int =
      {
        .peripheral = GPIO_Port_NULL,
        .gpio_pin = GPIO_Pin_NULL,
        .pull = GPIO_PuPd_NOPULL,
      },
  .low_power_threshold = 1,
  .battery_capacity_hours = 48,
};

// The ULP board exposes KEY1 on PA34 and KEY2 on PA43. Up/Down are reserved
// for a later input-mapping pass and intentionally describe absent buttons.
const BoardConfigButton BOARD_CONFIG_BUTTON = {
  .buttons =
      {
        [BUTTON_ID_BACK] = {"Back", hwp_gpio1, 34, GPIO_PuPd_NOPULL, true},
        [BUTTON_ID_UP] = {"Up", GPIO_Port_NULL, GPIO_Pin_NULL, GPIO_PuPd_NOPULL, false},
        [BUTTON_ID_SELECT] = {"Select", hwp_gpio1, 43, GPIO_PuPd_NOPULL, true},
        [BUTTON_ID_DOWN] =
            {"Down", GPIO_Port_NULL, GPIO_Pin_NULL, GPIO_PuPd_NOPULL, false},
      },
  .timer = GPTIM2,
  .timer_irqn = GPTIM2_IRQn,
};

void display_co5300_irq_handler(void *ctx);
IRQ_MAP(LCDC1, display_co5300_irq_handler, NULL);

IRQ_MAP(GPTIM2, debounced_button_irq_handler, GPTIM2);

uint32_t BSP_GetOtpBase(void) {
  return MPI2_MEM_BASE;
}

static void prv_psram_pins(uint32_t pid) {
  switch (pid) {
    case 5:  // APS 16-pin QSPI PSRAM
      HAL_PIN_Set(PAD_SA09, MPI1_CLK, PIN_NOPULL, 1);
      HAL_PIN_Set(PAD_SA08, MPI1_CS, PIN_NOPULL, 1);
      HAL_PIN_Set(PAD_SA05, MPI1_DIO0, PIN_PULLDOWN, 1);
      HAL_PIN_Set(PAD_SA07, MPI1_DIO1, PIN_PULLDOWN, 1);
      HAL_PIN_Set(PAD_SA06, MPI1_DIO2, PIN_PULLUP, 1);
      HAL_PIN_Set(PAD_SA10, MPI1_DIO3, PIN_PULLUP, 1);
      HAL_PIN_Set_Analog(PAD_SA00, 1);
      HAL_PIN_Set_Analog(PAD_SA01, 1);
      HAL_PIN_Set_Analog(PAD_SA02, 1);
      HAL_PIN_Set_Analog(PAD_SA03, 1);
      HAL_PIN_Set_Analog(PAD_SA04, 1);
      HAL_PIN_Set_Analog(PAD_SA11, 1);
      HAL_PIN_Set_Analog(PAD_SA12, 1);
      break;
    case 2:  // APS 128-pin XCELLA PSRAM
      HAL_PIN_Set(PAD_SA01, MPI1_DIO0, PIN_PULLDOWN, 1);
      HAL_PIN_Set(PAD_SA02, MPI1_DIO1, PIN_PULLDOWN, 1);
      HAL_PIN_Set(PAD_SA03, MPI1_DIO2, PIN_PULLDOWN, 1);
      HAL_PIN_Set(PAD_SA04, MPI1_DIO3, PIN_PULLDOWN, 1);
      HAL_PIN_Set(PAD_SA05, MPI1_DIO4, PIN_PULLDOWN, 1);
      HAL_PIN_Set(PAD_SA06, MPI1_DIO5, PIN_PULLDOWN, 1);
      HAL_PIN_Set(PAD_SA07, MPI1_DIO6, PIN_PULLDOWN, 1);
      HAL_PIN_Set(PAD_SA08, MPI1_DIO7, PIN_PULLDOWN, 1);
      HAL_PIN_Set(PAD_SA09, MPI1_DQSDM, PIN_PULLDOWN, 1);
      HAL_PIN_Set(PAD_SA10, MPI1_CLK, PIN_NOPULL, 1);
      HAL_PIN_Set(PAD_SA11, MPI1_CS, PIN_NOPULL, 1);
      HAL_PIN_Set_Analog(PAD_SA00, 1);
      HAL_PIN_Set_Analog(PAD_SA12, 1);
      break;
    case 3:  // APS 64-pin XCELLA PSRAM
    case 4:  // APS 32-pin legacy PSRAM
    case 6:  // Winbond HyperBus PSRAM
    default:
      HAL_PIN_Set(PAD_SA01, MPI1_DIO0, PIN_PULLDOWN, 1);
      HAL_PIN_Set(PAD_SA02, MPI1_DIO1, PIN_PULLDOWN, 1);
      HAL_PIN_Set(PAD_SA03, MPI1_DIO2, PIN_PULLDOWN, 1);
      HAL_PIN_Set(PAD_SA04, MPI1_DIO3, PIN_PULLDOWN, 1);
      HAL_PIN_Set(PAD_SA08, MPI1_DIO4, PIN_PULLDOWN, 1);
      HAL_PIN_Set(PAD_SA09, MPI1_DIO5, PIN_PULLDOWN, 1);
      HAL_PIN_Set(PAD_SA10, MPI1_DIO6, PIN_PULLDOWN, 1);
      HAL_PIN_Set(PAD_SA11, MPI1_DIO7, PIN_PULLDOWN, 1);
      HAL_PIN_Set(PAD_SA07, MPI1_CLK, PIN_NOPULL, 1);
      HAL_PIN_Set(PAD_SA05, MPI1_CS, PIN_NOPULL, 1);
      if (pid == 4) {
        HAL_PIN_Set(PAD_SA00, MPI1_DM, PIN_PULLDOWN, 1);
        HAL_PIN_Set(PAD_SA12, MPI1_DQS, PIN_PULLDOWN, 1);
        HAL_PIN_Set(PAD_SA06, MPI1_CLKB, PIN_NOPULL, 1);
      } else if (pid == 6) {
        HAL_PIN_Set(PAD_SA12, MPI1_DQSDM, PIN_NOPULL, 1);
        HAL_PIN_Set_Analog(PAD_SA00, 1);
        HAL_PIN_Set_Analog(PAD_SA06, 1);
      } else {
        HAL_PIN_Set(PAD_SA12, MPI1_DQSDM, PIN_PULLDOWN, 1);
        HAL_PIN_Set_Analog(PAD_SA00, 1);
        HAL_PIN_Set_Analog(PAD_SA06, 1);
      }
      break;
  }
}

bool board_psram_init(void) {
  static FLASH_HandleTypeDef psram_handle;
  const uint32_t pid = (hwp_hpsys_cfg->IDR & HPSYS_CFG_IDR_PID_Msk) >> HPSYS_CFG_IDR_PID_Pos;
  const uint32_t psram_pid = pid & 7U;

  prv_psram_pins(psram_pid);
  (void)HAL_RCC_HCPU_EnableDLL2(240000000);
  HAL_RCC_HCPU_ClockSelect(RCC_CLK_MOD_FLASH1, RCC_CLK_FLASH_DLL2);
  (void)HAL_PMU_ConfigPeriLdo(PMU_PERI_LDO_1V8, true, true);

  qspi_configure_t qspi_cfg = {
      .Instance = hwp_qspi1,
      .SpiMode = SPI_MODE_OPSRAM,
      .msize = 8,
      .base = QSPI1_MEM_BASE,
  };
  if (psram_pid == 5) {
    qspi_cfg.SpiMode = SPI_MODE_PSRAM;
  } else if (psram_pid == 4) {
    qspi_cfg.SpiMode = SPI_MODE_LEGPSRAM;
  } else if (psram_pid == 6) {
    qspi_cfg.SpiMode = SPI_MODE_HBPSRAM;
  }

  psram_handle.wakeup = 0;
  HAL_StatusTypeDef status = HAL_MPI_PSRAM_Init(&psram_handle, &qspi_cfg, 2);
  if (status != HAL_OK) {
    return false;
  }

  volatile uint32_t *probe = (volatile uint32_t *)(PSRAM_BASE + 0x100000U);
  probe[0] = 0x5A17C0DEU;
  return probe[0] == 0x5A17C0DEU;
}

void board_early_init(void) {
}

void board_init(void) {
  i2c_init(I2C1_BUS);

  // The ULP board wires its external NOR flash to MPI2. The boot ROM /
  // bootloader set these up before jumping, but the application must not
  // rely on that state after soc_early_init().
  HAL_PIN_Set(PAD_PA16, MPI2_CLK, PIN_NOPULL, 1);
  HAL_PIN_Set(PAD_PA12, MPI2_CS, PIN_NOPULL, 1);
  HAL_PIN_Set(PAD_PA15, MPI2_DIO0, PIN_PULLDOWN, 1);
  HAL_PIN_Set(PAD_PA13, MPI2_DIO1, PIN_PULLDOWN, 1);
  HAL_PIN_Set(PAD_PA14, MPI2_DIO2, PIN_PULLUP, 1);
  HAL_PIN_Set(PAD_PA17, MPI2_DIO3, PIN_PULLUP, 1);

}
