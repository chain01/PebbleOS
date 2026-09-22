# SF32LB52-LCHSPI-ULP 最小系统启动记录

本文记录 `sf32lb52_ulp` 板级目标在真实硬件上的最小系统启动过程。
验证日期：2026-09-21。

## 目标与需求分解

本阶段的验收目标是“最小系统启动”，不做 HRM、加速度计、环境光、屏幕、
触摸、电池计、音频等外设功能。需求按优先级拆分如下：

### P0 构建基线

- 新增 `sf32lb52_ulp` 板级 target。
- 选定 Emery 逻辑平台。
- 提供 16 MiB NOR 的 flash region 和 linker layout。
- 在 Windows 和 Arm GNU Toolchain 环境下可重复构建。

### P1 最小启动

- SF32LB52X 启动、时钟、PMU、GPIO 初始化。
- `USART1` 调试串口可用，波特率与 boot ROM 一致。
- `FLASH2` QSPI NOR 初始化并识别器件。
- RTC、定时器和资源子系统初始化。
- 进入 PebbleOS `main()`、FreeRTOS 调度和 `KernelMain` 任务。
- 启动 LCPU 控制器，NimBLE host 同步并进入 BLE 广告。
- 连续运行不出现二次复位或 core dump。

### P2 显示与输入

- CO5300 QSPI AMOLED。
- FT6146 触摸。
- KEY1/KEY2 导航映射。

### P3 电源与传感器

- 电池、充电器、USB 检测。
- LSM6DS3TR-C、LTR-303ALS、MMC5603NJ。
- 麦克风、扬声器、振动马达。

### P4 系统级完善

- 低功耗和 suspend/resume。
- watchdog、DFU、恢复流程。
- 最终 UI 几何和量产校准。

## 已验证的启动链

当前最小启动使用 SiFli SDK 同板 ULP 工程的二级 bootloader，以及为 PebbleOS
固件长度重新生成的 flash table：

| 区域 | 地址 | 说明 |
|---|---:|---|
| flash table | `0x12000000` | `FCES` 表头，大小 `0x2000` 对齐 |
| bootloader | `0x12010000` | SiFli ULP bootloader，大小 58400 字节 |
| PebbleOS | `0x12020000` | 当前单槽 bring-up 镜像 |
| system resources | `0x12620000` | `system_resources.pbpack` |
| HCPU image header | `0x12001400` | `running_imgs[2]` 指向它 |

flash table 中关键字段：

- `ftab[3].base = 0x12010000`
- `ftab[3].xip_base = 0x20020000`
- `ftab[4].base = 0x12020000`
- `ftab[4].xip_base = 0x12020000`
- `imgs[1].length = bootloader.bin` 大小
- `imgs[2].length = pebbleos.bin` 大小
- `running_imgs[1] = 0x12001200`
- `running_imgs[2] = 0x12001400`

注意：`0x12010000` 使用的是 SDK ULP 工程已经构建出的 bootloader，属于
临时 bring-up 方案。正式产品化时应把 bootloader 源码和构建流程纳入仓库，
不要依赖开发机上的 SDK 构建产物。

## 硬件连接与复位时序

- 调试串口：`COM16`，`USB-SERIAL CH340`
- 串口参数：`1,000,000 8N1`
- `USART1`：TX `PA19`，RX `PA18`
- QSPI NOR：`FLASH2/MPI2`
  - CLK `PA16`
  - CS `PA12`
  - DIO0 `PA15`
  - DIO1 `PA13`
  - DIO2 `PA14`
  - DIO3 `PA17`
  - QSPI 分频：`4`
  - 识别结果：`XT25F128F`，JEDEC ID `0x18400b`

复位时序是本次 bring-up 最容易误判的问题。验证可用的顺序是：

1. DTR 和 RTS 同时置高。
2. 保持约 300 ms。
3. DTR 和 RTS 同时置低，保持低电平并开始抓 log。

如果复位后把 RTS 重新拉高，ROM/下载态会继续保持，串口只会看到 `SFBL\n`，不会启动二级 bootloader。此前所有“改完 ftab 仍然只有 SFBL”的现象均由该
时序误判放大。

## 本次修复的问题

### 1. UART 日志不可读

`CONFIG_PULSE_EVERYWHERE` 默认启用，PebbleOS 日志走二进制 PULSE 协议。
bring-up 阶段先关闭该选项，改为可读文本日志：

```text
# CONFIG_PULSE_EVERYWHERE is not set
```

同时，关闭 PULSE 后 `dbgserial.c` 原先默认切到 `115200`，而 SiFli boot ROM
和 bootloader 使用 `1,000,000`。ULP 板因此固定使用 1 Mbaud：

```c
#if defined(CONFIG_BOARD_SF32LB52_ULP)
#define DEFAULT_SERIAL_BAUD_RATE 1000000
#elif defined(CONFIG_PULSE_EVERYWHERE)
#define DEFAULT_SERIAL_BAUD_RATE 1000000
#else
#define DEFAULT_SERIAL_BAUD_RATE 115200
#endif
```

### 2. `FLASH2` 初始化断言

第一次进入 `init_drivers()` 时停在 `flash_init()`。根因有两点：

- 板级 `board_init()` 没有配置 MPI2 引脚复用。
- QSPI 分频使用 `0`，而 SDK 同板使用 `4`。

修复后 `HAL_FLASH_Init()` 返回 `HAL_OK`，并成功识别 `XT25F128F`。

### 3. 启动约 12 秒后复位与 watchdog 恢复

SF32LB52X 的 WDT 超时为 10 秒。最初最小系统尚未到达任务级喂狗路径，因此
在 `Ready for communication.` 前会被 WDT 复位。首次 bring-up 临时使用
`CONFIG_NO_WATCHDOG=y`，该选项会让 `soc_early_init()` 跳过
`watchdog_init()/watchdog_start()`。

在 FreeRTOS、SysTick 和 task-watchdog 路径打通后，ULP 板已移除
`CONFIG_NO_WATCHDOG`，恢复 SF32LB52 硬件看门狗：

- 10 秒硬件超时，任务级 watchdog 正常喂狗。
- 启动后先暂停 task-watchdog 30 秒，之后由 NewTimers 和 KernelMain/
  KernelBackground 的正常调度路径喂狗。
- 2026-09-22 实机烧录并连续抓取 75 秒日志：`SFBL`、`PBULP_ENTER` 和
  `Ready for communication.` 均只出现一次，没有 core dump 或复位。
- 手机重连后执行天气/BLE 同步时，出现过两次约 5 秒的 `KernelBG` 瞬时
  滞后，但都在约 0.5 秒后恢复，没有触发 6.5 秒的任务失败重启路径。

因此硬件 watchdog 和基础喂狗链路已经恢复；下一轮需要减少 ULP 调试日志、
定位天气/BLE 并发时的短时 `KernelBG` 滞后，并在 release 构建下复测。

### 4. Bluetooth/LCPU：BLE MAC 生成失败

启用 NimBLE 后，`ble_transport_ll_reinit()` 会先调用
`lcpu_custom_nvds_config()`。该函数原先只接受 `UID[7] == 0xA5` 的 v2
格式；ULP 模块的 efuse UID 不是这种格式，且回退算法可能得到全零地址，
最终触发断言或让控制器拿到全零 MAC。

修复位于 `third_party/hal_sifli/sf32lb52/lcpu_config.c`：

- v2 格式合法时继续使用原有 UID 校验和路径。
- 否则使用 SiFli SDK 的 v1 UID→MAC 转换。
- v1 结果为零或不可用时，再对整个 UID 做稳定哈希，生成非零的本地管理
  单播地址。

本板最终可以正常广播，但广播名称/地址会随蓝牙持久化数据状态变化；不要把
某一个地址硬编码进测试。最近一次实测为 `Pebble 1D34` /
`05:EE:D0:41:1D:34`，RSSI 约 `-43` 到 `-57 dBm`。保持 DTR/RTS 高电平
（复位态）时广播消失，释放复位后重新出现，说明广播来自本板而不是附近
设备。启动日志中的 `Pinned address: 00:00:00:00:00:00` 表示没有需要地址
绑定的 bonding，并不等于控制器实际广告地址。

### 5. Bluetooth/NimBLE bring-up 调试

显示/触摸迁移后，最初在 PC 端扫描不到任何 Pebble BLE 广播。排查链路和
最终根因如下：

1. 本板实际 `REVID=3`（低于 `HAL_CHIP_REV_ID_A4`），因此走 A3 兼容路径：
   `lcpu_img_install()` 安装 `lcpu_52x.c` 镜像。Rev B ROM patch 路径本轮未
   在这块板上执行；换用 A4/B 芯片时仍需单独验证。
2. NimBLE host task 能启动，但看不到 HCI 日志。根因是
   `LOG_DOMAIN_BT_STACK` 默认定义为 `0`，而 PebbleOS 日志宏会丢弃
   `domain == 0` 的日志。已在 `third_party/nimble/CMakeLists.txt` 中加入：

   ```cmake
   pbl_compile_definitions(LOG_DOMAIN_BT_STACK=1)
   ```

   否则 HCI 收发、IPC 错误和 “CMD complete” 日志全部被静默。
3. 即使 HCI 日志恢复，仍没有 HCI packet。进一步确认 `gap_bonding_db` 中
   持久化 `AIRPLANE_MODE=true`，所以 `bt_ctl_set_enabled(true)` 后
   `bt_ctl_is_bluetooth_active()` 仍返回 false，驱动根本没有启动。
   串口的 `bt airplane mode exit` 只清除运行时 override，不会修改飞行模式
   持久化位。bring-up 时可从 Pebble console 仅删除蓝牙持久化数据库：

   ```text
   pfs rm gap_bonding_db
   ```

   然后复位。该操作会同时清除 bonding/root key 等 BLE 持久化数据，开发板
   可用，量产镜像不应自动执行。
4. NVDS 位于 LPSYS RAM。现在写入前先 `HAL_HPAON_WakeCore(CORE_ID_LCPU)`，
   写完后 `HAL_HPAON_CANCEL_LP_ACTIVE_REQUEST()`，与 SiFli SDK 的
   `bt_stack_nvds_init()` 顺序一致，避免低功耗域未唤醒时写入不可靠。
5. 完整 HCI trace 只用于 bring-up。验证完成后
   `boards/sf32lb52_ulp/defconfig` 恢复：

   ```text
   CONFIG_NIMBLE_HCI_SF32LB52_TRACE_NONE=y
   ```

   需要再次抓取 HCI 时切换为 `CONFIG_NIMBLE_HCI_SF32LB52_TRACE_LOG=y`。

实测 HCI 顺序包含 `Reset` -> 读取版本/命令/feature -> LE event mask、
buffer、white list、PHY、address/parameter 配置 -> Set Advertising Data ->
Set Scan Response Data -> Set Advertising Enable；控制器对所有命令均返回
Command Complete。广告数据中包含 `Pebble 1D34` 和 `FED9` service UUID，
scan response 中包含 Core Devices `0x0EEA` 厂商数据。Windows/Bleak 的
`manufacturer_data` 有时不会暴露扫描响应中的厂商字段，因此验证不能只依赖
该字段，还应同时按设备名、service UUID 和可连接性判断。

连接级验证使用 Bleak：

- 能建立 BLE GATT 连接。
- 枚举到 6 个 service：GAP、GATT、DIS、`FED9`、BAS 和 Pebble 自定义
  service `40000000-328e-0fbb-c642-1aa6699bdada`。
- 主机主动断开后设备恢复广告。
- 8 秒扫描收到 95 次同一设备报告，连接和断开过程中无 LCPU 崩溃或复位。


### 6. Pebble App 手机端验证

手机端使用 Xiaomi 17 Pro（Android 17 / SDK 37）和 Pebble App
`coredevices.coreapp` 1.13.0.2 验证。Android BLE 连接、bonding、重连和
App Store 表盘安装均已实际跑通。

配对流程中，Android 会先弹出 Companion Device 授权，再显示 6 位 Numeric
Comparison 配对码。手机和手表两端都必须在约 30 秒内确认；只确认一端会出现
`SMP_NUMERIC_COMPAR_FAIL`，随后 bond 回退到 `BT_BOND_STATE_NONE`。两端及时
确认后，Android 记录 `BT_BOND_STATE_BONDED`，重启 App 会自动重连。

手机首次扫描到本板时曾显示 `Unknown platform / Unknown Watch!`。核对
`coredevices/mobileapp` 源码后确认，它读取 WatchVersion 中
`running.hardware_platform`，并进行以下映射：

```text
16 = obelix_evt  -> EMERY/Pebble Time 2
17 = obelix_dvt  -> EMERY/Pebble Time 2
18 = obelix_pvt  -> EMERY/Pebble Time 2
0  = unknown     -> Unknown platform
```

ULP 板原先没有在 `pebbleos/firmware_metadata.h` 中映射平台，所以发送 `0`。
现在 `CONFIG_BOARD_SF32LB52_ULP` 映射为 `PebbleObelixPVT`（18）。同时 ULP
默认表壳颜色从 `UNKNOWN` 改为 `COREDEVICES_PT2_BLACK_GREY`，App 现在显示：

平台和序列号正确后，连接状态曾显示为 `Connected (Factory)`。它对应移动 App 的
`ConnectedInPrf` 状态，不是单纯的显示标签：

```kotlin
val ignoreMissingPrfOnThisDevice =
    watchConfig.value.ignoreMissingPrf || !identifier.canHaveRecoveryFirmware()
val recoveryMode = when {
    watchInfo.runningFwVersion.isRecovery -> true
    !ignoreMissingPrfOnThisDevice && watchInfo.recoveryFwVersion == null -> true
    watchInfo.runningFwVersion < FW_3_0_0 -> true
    else -> false
}
```

当前单槽 raw-XIP bring-up 没有 PRF/recovery 镜像，因此
`recoveryFwVersion == null`，App 会把连接降级为 PRF 模式，只初始化固件更新、
日志、core dump 等恢复服务；表盘安装、语言包、音乐和通知服务都不会注册。

开发阶段需要在 App 中打开：

```text
Settings -> Phone -> Debug -> Show debug options
Settings -> Phone -> Connectivity -> Ignore Missing PRF
```

开启后重新连接，App 状态变为普通：

```text
Pebble 1D34
Connected
Pebble Time 2 - Black/Gray
Battery 77%
```

这一步是 Pebble App 官方给开发板提供的绕过选项。长期方案仍是补齐仓库可构建的
PRF/recovery 镜像和 PBLBOOT 双槽流程。

序列号原先回退为 `XXXXXXXXXXXX`，App 会将其视为未烧录并隐藏。ULP 没有正式
OTP serial；现在未写 OTP 时使用 MCU UID 生成稳定的 12 位开发序列号，不写入
OTP，也不影响以后写正式序列号。当前实测：

```text
Serial: ULP02A28F89C
```

开启 `Ignore Missing PRF` 后完成了以下实测：

- App Store 安装 `Chroma`，状态变为 `Running On Watch Pebble 1D34`。
- 选择并安装 `简体通知 (SimplifiedChinese) v2`，App 日志出现
  `LanguagePackInstaller: installLanguagePack finished/done`。
- 手表端持续处理 `PpPutBytes`，表明 app/resource/language pack 已经进入正常
  Pebble 协议通道。
- App 状态日志从 `ConnectedPebbleDeviceInRecovery` 变为普通
  `ConnectedPebbleDevice`，并注册 AppMessages、Music、Language、Notification
  等完整服务。
- App 的通知监听服务已绑定，测试通知能在 Android NotificationManager 中正常
  创建；QQ 音乐 MediaSession 处于 active/PLAYING，重新连接后可由 Music
  服务接收。
- 语言包安装后重新连接，WatchVersion 返回
  `language=en_CN, languageVersion=2`，确认语言包已经写入手表。

### 7. 资源 map 暂用 Obelix

`resources/normal/sf32lb52_ulp/resource_map.json` 目前只是让资源和链接
流程先跑通的临时 Obelix map。它不决定屏幕物理尺寸，但量产前必须替换为
ULP 专用资源映射。

## 构建命令

本机验证使用 Arm GNU Toolchain 14.2 和 newlib-nano：

```powershell
cmake -S . -B build-ulp-arm10 -GNinja `
  -DBOARD=sf32lb52_ulp `
  -DCONFIG_LIBC_PICOLIBC=n `
  -DCONFIG_LIBC_NEWLIB_NANO=y
cmake --build build-ulp-arm10 -j 16
```

Windows 下资源转换代理需要位于 `PATH`，例如本次使用的临时目录：

```powershell
$env:PATH = "$env:TEMP\pebble-build-tools;" + $env:PATH
```

最终产物：

- `build-ulp-arm10/pebbleos.elf`
- `build-ulp-arm10/pebbleos.hex`
- `build-ulp-arm10/pebbleos.bin`
- `build-ulp-arm10/system_resources.pbpack`

## 烧录命令

```powershell
sftool -c SF32LB52 -p COM16 -b 1000000 --after no_reset write_flash --verify `
  "$SIFLI_SDK\example\multimedia\audio\local_music\project\build_sf32lb52-lchspi-ulp_hcpu\bootloader\bootloader.bin@0x12010000" `
  "build-ulp-arm10\pebbleos.bin@0x12020000" `
  "$env:TEMP\pebble-ulp-ftab.bin@0x12000000" `
  "build-ulp-arm10\system_resources.pbpack@0x12620000"
```

`pebble-ulp-ftab.bin` 由 SDK ULP 工程的 `ftab.bin` 修改而来，必须写入
当前 `pebbleos.bin` 的实际长度，并保持 `running_imgs` 指向 `0x12001200`
和 `0x12001400`。

## 复位并抓 2 秒以上日志

```python
import serial, time

with serial.Serial("COM16", 1_000_000, timeout=0.02) as s:
    s.dtr = True
    s.rts = True
    time.sleep(0.3)
    s.reset_input_buffer()
    s.dtr = False
    s.rts = False

    deadline = time.time() + 3.0
    while time.time() < deadline:
        data = s.read(8192)
        if data:
            print(data)
```

## BLE 广播验证

PC 端安装 `bleak` 后，可以在释放复位并等待几秒后扫描 Core Devices
厂商 ID `0x0EEA`：

```python
import asyncio
from bleak import BleakScanner

async def main():
    result = await BleakScanner.discover(timeout=10.0, return_adv=True)
    for addr, (dev, adv) in result.items():
        name = dev.name or adv.local_name or ""
        if "pebble" in name.lower() or 0x0EEA in adv.manufacturer_data:
            print(addr, name, adv.rssi)

asyncio.run(main())
```

验证时先保持 DTR/RTS 高电平（板子处于复位态），应扫描不到该 Pebble
设备；释放两路信号后，应重新扫描到 `Pebble A68C`。进一步使用 `BleakClient`
连接该地址可以成功建立 GATT 连接，说明 HCI ACL 和 NimBLE 连接路径已经
打通。

## 验证结果

串口关键输出：

```text
SFBL
PBULP_ENTER
PBULP_INIT_OK
...
E - ... `XT25F128F` ...
...
D - ... testinfra.c:12> Ready for communication.
```

验证条件：

- `SFBL` 只出现一次。
- `PBULP_ENTER` 出现一次，说明进入 PebbleOS `Reset_Handler`。
- `PBULP_INIT_OK` 出现一次，说明 `SystemInit()` 完成。
- QSPI 识别到 `XT25F128F`。
- `Ready for communication.` 之后 PC 可扫描到 `Pebble A68C` 广播并建立
  BLE GATT 连接。
- 20 秒抓包无第二次 `SFBL`、无 core dump、无持续复位。
- 日志到达 `Ready for communication.`。

## CO5300 显示与 FT6146 触摸

本阶段完成了 390x450 CO5300 AMOLED 和 FT6146 触摸的实际硬件验证。

### 显示根因

官方 `example/rt_driver` 在同一块 ULP 板上能够正常显示色条、渐变和纯色，
说明硬件、面板供电和官方 LCDC/CO5300 初始化序列正常。PebbleOS 侧最终确认
有两个关键差异：

1. CO5300 初始化必须使用 `0x20` 厂商页完成密码解锁和配置；使用 `0x00`
   页会导致命令写入看似成功但面板没有真正进入显示状态。
2. LCDC QSPI 图层数据传输必须使用 `HAL_LCDC_SendLayerData2Reg_IT()`。
   在旧 HAL 和本面板组合下，同步逐行 `HAL_LCDC_SendLayerData2Reg()` 不能可靠
   显示完整帧。逐行 IT 可以显示，但刷新次数过多。

### 当前显示架构

- 逻辑 framebuffer 保持 Emery 的 `200x228`，应用和资源无需立即重写。
- 显示驱动用预计算的 RGB222→RGB565 调色板和 X 坐标映射，把逻辑帧放大到
  物理 `390x450`。
- 整帧 RGB565 输出缓冲放在板载 8 MB PSRAM 中，地址使用
  `PSRAM_BASE + 0x100000`。
- PSRAM 通过 ULP 板的 `board_psram_init()` 使用 MPI1 `HAL_MPI_PSRAM_Init()`
  初始化；启动日志 `ULP PSRAM ready=1` 表示读写测试通过。
- 缩放由 EPIC GPU 完成：每次把 16 个源行放大到内部 SRAM strip，再复制到
  PSRAM 整帧缓冲。条带边界会补相邻行，修复每 16 行出现黑线的问题。
- 每帧在 PSRAM 中完整生成后，再通过 LCDC1 中断传输和 TE/VSYNC 同步整帧
  写入 CO5300 GRAM，避免逐条带刷新造成的横向 tearing。

### 触摸映射

FT6146 返回物理面板坐标。全屏显示使用线性放大而不是居中偏移，因此触摸
坐标按以下方式映射回 `200x228` 逻辑坐标：

```text
x = raw_x * PBL_DISPLAY_WIDTH  / 390
y = raw_y * PBL_DISPLAY_HEIGHT / 450
```

X/Y 均不做镜像。四方向、四角和中心触摸已与显示方向核对通过。

### 验证结果

- CO5300 ID 读取为 `0x331100`。
- 屏幕上电后正常显示开机画面和系统 UI。
- 全屏放大后无黑边，触摸方向与显示一致。
- 使用 PSRAM 整帧缓冲后横向条带乱码消失。
- 使用 EPIC GPU 分段缩放后，16 行黑线消失，滚动和页面动画流畅度正常。

## PRF recovery 镜像构建与识别

ULP 目标现在可以独立构建 PRF recovery 镜像，并在普通固件中识别。构建命令：

```shell
cmake -S . -B build-ulp-prf-arm10 -GNinja `
  -DBOARD=sf32lb52_ulp -DVARIANT=prf `
  -DCONFIG_LIBC_NEWLIB_NANO=y -DCONFIG_LIBC_PICOLIBC=n
cmake --build build-ulp-prf-arm10 -j 16
```

本板 PRF 使用 512 KiB `SAFE_FIRMWARE` 区域，链接配置固定为：

```text
CONFIG_FLASH_OFFSET=0xa20000
CONFIG_FW_FLASH_SIZE=0x80000
```

构建结果：

```text
FLASH: 491395 B / 512 KB (93.73%)
```

由于当前仍是 `CONFIG_PBLBOOT=n` 的 legacy raw-XIP 布局，刷入 PRF 前必须
添加 12 字节 `FirmwareDescription`：

```shell
python tools/insert_firmware_descr.py `
  build-ulp-prf-arm10/pebbleos.bin `
  build-ulp-prf-arm10/pebbleos-prf.bin

sftool -c SF32LB52 -p COM16 -b 1000000 --after no_reset `
  write_flash --verify `
  "build-ulp-prf-arm10/pebbleos-prf.bin@0x12A20000"
```

重启普通固件后，用串口 shell 查询：

```text
>version
Running FW:
  tag:v4.37.0-94-gc56b058d9-dirty
  recov:0
  platform:18
Recovery FW:
  tag:v4.37.0-95-g9ee038962-dirty
  recov:1
  platform:18
```

这说明普通固件已经能读取并通过 CRC 校验 PRF。当前仍缺少的是让 SiFli
bootloader/PBLBOOT 选择并启动该 recovery 镜像，以及与其配套的 OTA、
slot 切换和回滚流程；这些完成前，PRF 只能算“已安装并可被系统识别”，
还不能算“可自动恢复启动”。

## 当前限制与下一步

当前版本已经完成最小系统启动，但仍不是可量产镜像：

- LCPU/NimBLE 广告、GATT、Android 配对/绑定、自动重连和 App Store
  表盘安装已验证；通知、时间线、天气和表盘设置等完整手机功能仍需测试。
- watchdog 已启用；手机重连和天气/BLE 同步时仍需消除偶发的 KernelBG 短时滞后。
- PULSE 已关闭，正式日志方案需要恢复协议并接入解码工具。
- battery、sensor、audio 仍为 stub 或未验证；display 和 touch 已通过硬件验证。
- HRM 不在本 ULP 目标范围内，不再列为后续适配项。
- 全屏缩放已通过 EPIC GPU 分段加速完成，旧的 CPU 缩放描述已废弃。
- normal resource map 仍临时复用 Obelix map；PRF 已有 ULP 专用资源映射。
- SDK 预构建 bootloader 只是 bring-up 依赖，需要纳入源码构建。
- `PBULP_ENTER` / `PBULP_INIT_OK` 是非 release 构建的早期 marker，正式版本
  会由 `CONFIG_RELEASE` 自动去掉。
