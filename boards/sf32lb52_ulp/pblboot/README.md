# PBLBOOT board support for SF32LB52-ULP

This directory contains the board patch used to build the PebbleOS bootloader
for the SiFli `sf32lb52-lchspi-ulp` target.

The patch was developed and hardware-validated against upstream `pblboot`
commit `aa776870527a56422556601d7494582425b96e53`.

## Apply

```shell
git clone https://github.com/coredevices/pblboot.git
cd pblboot
git checkout aa776870527a56422556601d7494582425b96e53
git am /path/to/sf32lb52-ulp.patch
```

Build with the Zephyr/west environment required by `pblboot`:

```shell
west build -b sf32lb52_ulp boot
```

The resulting bootloader is `build/zephyr/zephyr.bin` and is flashed at
`0x12010000`.

## ULP configuration

- SoC: `sf32lb525uc6`
- External flash: 16 MiB `XT25F128F`, JEDEC ID `0x18400b`
- Console: `USART1`, PA18/PA19, 1 Mbaud
- Buttons: PA34 Back and PA43 Select
- Recovery combo: Back + Select
- Boot partition: 64 KiB at `0x12010000`
- Slot 0: 3 MiB at `0x12020000`
- Slot 1: 3 MiB at `0x12320000`
- PRF: 512 KiB at `0x12A20000`
- Bootloader watchdog is disabled on ULP so PebbleOS owns the watchdog after
  handoff

The slot and PRF headers use the standard PBLBOOT 28-byte firmware header.
PebbleOS produces those headers when configured with `CONFIG_PBLBOOT=y`.

## Validated path

The hardware test used:

- PBLBOOT at `0x12010000`
- normal PebbleOS in slot 0 with `CONFIG_PBLBOOT=y`
- PRF in `SAFE_FIRMWARE` with `CONFIG_PBLBOOT=y`

The bootloader selected slot 0 normally. After erasing the slot 0 header, it
reported `No valid firmware image`, loaded PRF, and PRF reached
`Ready for communication.`