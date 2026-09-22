# SiFli SF32LB52-LCHSPI-ULP bring-up

This document records the PebbleOS port to the SiFli
`sf32lb52-lchspi-ulp` board. The first milestone is a minimal system boot, not
full watch feature parity.

## Scope

Stage 1 intentionally keeps the board-specific surface small:

- Cortex-M33/SF32LB52X startup and clock configuration
- Debug UART on `USART1` (`PA18`/`PA19`)
- 16 MiB QSPI NOR on `FLASH2` (`XT25F128F`, JEDEC ID `0x18400b`)
- RTC, GPIO, EXTI and RNG
- Human-readable text logging on the SiFli 1 Mbaud boot console
- LCPU/NimBLE Bluetooth controller and host initialization
- BLE advertising and GATT connection verified with a host-side scanner
- CO5300 390x450 QSPI AMOLED with full-screen scaling and PSRAM framebuffer
- FT6146 touch controller with scaled logical coordinates
- No-op battery, accelerometer, ambient light, backlight and vibration drivers
- MPI1 8 MiB PSRAM initialization for the panel framebuffer
- Stored apps disabled for the first firmware link (`CONFIG_BRINGUP_NO_STORED_APPS`).

Hardware bring-up notes and the exact validated flashing/reset procedure are in
{doc}`boards/sf32lb52_ulp/bringup`.

The following hardware is deliberately not enabled yet:

- LSM6DS3TR-C accelerometer
- LTR-303ALS ambient light sensor
- MMC5603NJ magnetometer
- Microphone, speaker and real vibration motor
- Battery charger and real fuel-gauge reporting

Heart-rate monitoring is explicitly out of scope for this ULP target.

## Hardware mapping

| Function | ULP pin/interface | PebbleOS configuration |
|---|---|---|
| SoC | SF32LB525UC6 | `CONFIG_SOC_SF32LB52` |
| Debug UART | `USART1`, RX PA18, TX PA19 | `DBG_UART` |
| External NOR | `FLASH2`, 16 MiB XT25F128F | `CONFIG_FLASH_XT25F128F` |
| KEY1 | PA34 | `BUTTON_ID_BACK` |
| KEY2 | PA43 | `BUTTON_ID_SELECT` |
| Up/Down | not populated | absent-button entries |
| Display | CO5300 QSPI, 390x450 | validated, 200x228 logical framebuffer scaled to full panel |
| Touch | FT6146 over I2C1 | validated, scaled to logical coordinates |

The ULP display is a 390x450 AMOLED panel. The logical Pebble framebuffer is
kept at 200x228, matching the existing Emery geometry, and is scaled in the
CO5300 driver to fill the full panel. The scaled RGB565 frame is assembled in
the 8 MiB onboard PSRAM and sent as one TE-synchronized LCDC transfer. FT6146
touch coordinates are scaled back to the same logical coordinate system.

See {doc}`boards/sf32lb52_ulp/bringup` for the display/touch bring-up details.

## Boot strategy

The board now defaults to PBLBOOT, using a repository-owned bootloader patch
and the validated dual-slot/recovery layout:

- `CONFIG_PBLBOOT=y`
- `CONFIG_FLASH_OFFSET=0x20000`
- `CONFIG_FIRMWARE_OFFSET=4096`
- `CONFIG_FLASH_SIZE=0x1000000`

The boot chain used for the validated bring-up is:

- flash table at `0x12000000`
- PBLBOOT at `0x12010000`
- firmware slot 0 at `0x12020000`
- firmware slot 1 at `0x12320000`
- system resources at `0x12620000`
- PRF at `0x12A20000`

The ULP PBLBOOT board patch is stored in
`boards/sf32lb52_ulp/pblboot/sf32lb52-ulp.patch`; see that directory for the
tested upstream revision and build instructions. Hardware validation confirmed
both normal slot-0 boot and automatic fallback to PRF when slot 0 is
invalidated. Flash-table provisioning and OTA/rollback are still separate
productization tasks.

## Requirements breakdown

### P0: buildable board target

- Add `BOARD_SF32LB52_ULP`
- Add the Emery platform selection
- Add a 16 MiB flash layout and linker region
- Add Kconfig and CMake integration

### P1: minimal boot

- Configure SF32LB52X clocks, PMU and LCPU startup
- Bring up the debug UART
- Initialize `FLASH2` and verify the NOR device ID
- Initialize RTC and GPIO/EXTI/RNG
- Reach the PebbleOS kernel and main task
- Start the LCPU controller, sync NimBLE, and advertise

### P2: display path

- Port the CO5300 QSPI initialization sequence
- Add the QSPI 4-data LCDC path
- Center or scale the 200x228 logical framebuffer on 390x450
- Add panel brightness control

### P3: input and power path

- Port the FT6146 touch driver
- Map KEY1/KEY2 and touch gestures to Pebble navigation
- Implement battery/USB/charger reporting for the ULP board revision

### P4: watch peripherals

- LSM6DS3TR-C accelerometer
- LTR-303ALS ambient light sensor
- MMC5603NJ magnetometer
- Analog microphone and speaker path
- PA20 PWM vibration motor

### P5: productization

- Low-power and suspend/resume validation
- Flash update and recovery flow
- Manufacturing data and calibration
- Final UI geometry and panel-specific tuning

## Build

```shell
pbl configure --board sf32lb52_ulp
pbl build
```

Flash the resulting image through the ULP USB-to-UART adapter:

```shell
pbl flash --tty COMx
```

The target declares the `sftool` runner. A ROM flash table is still required
as the first-stage handoff. The validated second-stage bootloader is PBLBOOT;
build it using the patch in `boards/sf32lb52_ulp/pblboot/` and flash it at
`0x12010000` before flashing the PBLBOOT-formatted firmware.

### Validated host build

The target was successfully linked on Windows with:

- Arm GNU Toolchain 14.2.Rel1 (`arm-none-eabi-gcc`)
- newlib-nano for the local validation build
- `CONFIG_LIBC_PICOLIBC=n`
- `CONFIG_LIBC_NEWLIB_NANO=y`

The normal developer path remains the PebbleOS SDK. The newlib-nano overrides
are only a fallback for hosts that do not yet have the PebbleOS SDK installed.

The successful validation build produced:

- `pebbleos.elf`
- `pebbleos.hex`
- `pebbleos.bin`
- `system_resources.pbpack`

For the validated bring-up build with NimBLE enabled, Flash usage was
971,915 bytes (about 0.93 MiB) and RAM usage was 276,960 bytes (about 270 KiB).
The Intel HEX image starts at `0x12020000` as expected.

The Windows build also required portability fixes in the build scripts:

- preserve Windows `PATH` entries when spawning Python helpers
- normalize backslashes before writing generated CMake files
- parse SDK headers with an ARM target and short-enums ABI
- use UTF-8 when reading and writing source, resource and SDK files
- provide a no-microphone voice-window stub

## Verification checklist

Validated on 2026-09-21 with the ULP board connected as `COM16`:

1. The host sees `USB-SERIAL CH340` on `COM16`.
2. The SiFli boot ROM prints `SFBL`.
3. `PBULP_ENTER` and `PBULP_INIT_OK` confirm entry to `Reset_Handler` and
   completion of `SystemInit()`.
4. QSPI detects `XT25F128F` (JEDEC ID `0x18400b`).
5. The kernel/main task reaches the final startup log
   `Ready for communication.`
6. A 20-second run produced no second `SFBL`, no core dump, and no reset loop.
7. The PBLBOOT build selected and booted valid slot 0.
8. Erasing the slot 0 header caused PBLBOOT to load PRF automatically; slot 0
   was restored after the test.

Still to validate after enabling the real hardware backends:

- Full phone feature interoperability: notifications, timeline, weather,
  watchface settings and app message delivery
- battery, sensors and audio

## Known assumptions and risks

- The validated board reports `XT25F128F` with JEDEC ID `0x18400b`. Compatible
  16 MiB parts still need to be checked individually.
- The validated boot chain maps slot 0 at `0x12020000`, slot 1 at
  `0x12320000`, and PRF at `0x12A20000`. The ROM flash table remains an
  SDK-generated first-stage dependency and must be made repository-owned before
  production.
- The ULP board has multiple power/charger revisions. The source tree contains
  AW32001 references while public board documentation also mentions SY6103.
  Confirm the physical board revision before implementing battery management.
- Display and touch are hardware-validated. Full-screen scaling uses the EPIC
  GPU with PSRAM strip staging; the previous 16-row black-line artifact is
  fixed.
- Stored applications are disabled in stage 1. The resource map temporarily
  reuses the Obelix map so that the firmware/resource pipeline can link; it must
  be replaced with a ULP-specific map before shipping.
- The first bring-up, BLE advertising, GATT, Android pairing/bonding, automatic
  reconnect and an App Store watchface install are hardware-validated. PBLBOOT
  slot-0 boot and PRF fallback are also hardware-validated. The current firmware
  is still not a shippable product image: OTA/rollback, phone feature parity and
  all real peripherals remain to be validated.
  See {doc}`boards/sf32lb52_ulp/bringup` for the remaining work.
- A persisted airplane-mode flag in `gap_bonding_db` silently prevents the BT
  driver from starting. The bring-up notes document how to distinguish this
  from an LCPU/NimBLE failure before changing controller code.
- The hardware watchdog is enabled and the 75-second reconnection smoke test
  completed without a reset, but two transient `KernelBG` stalls recovered
  during weather/BLE synchronization. These should be eliminated before a
  release build is considered stable.
- PBLBOOT slot-0 boot and automatic PRF fallback are hardware-validated. The
  mobile-app `Ignore Missing PRF` workaround should no longer be necessary,
  but it still needs an App retest. OTA, slot switching and rollback remain to
  be validated.
- `LOG_DOMAIN_BT_STACK` must be nonzero for HCI/NimBLE transport diagnostics to
  be emitted; otherwise transport errors are silently dropped by the logger.

## Code locations

- `boards/sf32lb52_ulp/defconfig`
- `boards/sf32lb52_ulp/Kconfig`
- `src/fw/board/boards/board_sf32lb52_ulp.c`
- `src/fw/board/boards/board_sf32lb52_ulp.h`
- `src/fw/board/displays/display_sf32lb52_ulp.h`
- `src/fw/drivers/flash/xt25f128f.c`
- `src/fw/flash_region/flash_region_py25q128ha.h`
