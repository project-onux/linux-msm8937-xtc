# Linux for XTC Z8A (ND07)

Mainline Linux kernel tree for the **XTC Z8A** smartwatch, based on
[`msm8916-mainline/linux`](https://github.com/msm8916-mainline/linux)
(`wip/msm8916/7.0`).

> **Only the XTC Z8A (ND07) is supported.** Other msm8937/SDM429W devices
> will boot, but have no panel, no PMIC and no storage — the device tree
> here describes this board only.

## The device

| | |
|---|---|
| SoC | Qualcomm **SDM429W** (Wear 4100+, an msm8937 derivative) |
| CPU | 4× Cortex-A53 |
| PMIC | **PM660** + PM660 revision 2 (no PM660L on this board) |
| Display | Tianma RM69092, 320×360, DSI command mode |
| Touch | Zinitix BT541 |
| Storage | 32 GB eMMC, 3 LUNs |
| Sensor hub | Broadcom BCM4775 (GNSS + sensors, Samsung SSP protocol) |
| Camera | OV5675 + SC520CS |
| Audio | WCD9335 codec + AW882xx speaker amp |
| OS shipped | Android 8.1, kernel 4.9.193 |

Bootloader ids, read back from the running device
(`/proc/device-tree/qcom,*`):

```
qcom,msm-id   = <0x1a0 0x0>
qcom,board-id = <0x10b 0x6>
qcom,pmic-id  = <0x2001b 0x0 0x0 0x0>
```

## Status

| Block | State | Notes |
|---|---|---|
| UART console | ✅ | `blsp1_uart2` |
| Framebuffer / fbcon | ✅ | via lk2nd `simple-framebuffer` handover |
| USB gadget console | 🟡 | ChipIdea HDRC + 28nm femto PHY, peripheral mode; CDC-ACM shell on `/dev/ttyGS0` plus an NCM port at 192.168.42.1 |
| PM660 regulators | ✅ | 10 rails, RPM managed |
| Power key | ✅ | `pm8941-pwrkey` via `pm660.dtsi` |
| eMMC | ✅ | `sdhci-msm`, vmmc l19 / vqmmc l13 |
| Vibrator | ✅ | `gpio-vibra`, TLMM gpio 129 |
| NFC | ✅ | `nxp-nci-i2c`, irq 17 / ven 16 / firmware 130 |
| Touch | ✅ | `zinitix,bt541`, irq 65 / reset 64 |
| Charger | 🟡 | mainline `qcom_smbx` already matches `qcom,pm660-charger`; RRADC supplies the voltage/current channels |
| WiFi | 🟡 | WCNSS remoteproc + `wcn36xx`, firmware shipped in the initramfs |
| Sensor hub | 🟡 | `ssp,bcm4775` on SPI (12 MHz, mode 1), power TLMM 137, standby 47, status 66; the vendor pushes the hub patch from userspace, so no in-kernel upload path exists |
| Fuel gauge | 🚧 | `qcom,fg-gen3` is decoded (see below) but the driver is still being written |
| Audio | 🚧 | ADSP remoteproc and the APR/Q6 services are up; mainline already has `wcd9335.c` and the SLIMbus NGD controller, but the LPASS audio clock controller is missing from `gcc-msm8917.c` |
| Camera | 🚧 | CAMSS extended for msm8937, OV5675 wired with all three supplies; the rear SC520CS has no driver |
| Display (DRM) | ❌ | currently only the simplefb handover |

## Reverse-engineered details

The stock 4.9.193 kernel image (`D:/Z8A_V1.6.7/data/kernel`) is a raw ARM32
Image where `vaddr = 0xc0008000 + file offset`, which makes it possible to
recover register maps from it.  Two results worth recording:

**PM660 SMB2 charger.** The `smb_chg_param` table at `0xc0f7f728` gives
fast-charge current `0x1061` with 25 mA steps, float voltage `0x1070` at
`3487500 + 7500 * raw`, and USB input current limit `0x1370` with 25 mA
steps.  A live register dump confirms it: `0x1070 == 0x80 == 128`, and
`3487500 + 128 * 7500 == 4447500 uV` against a device tree value of
4450000 uV.

**FG-gen3 fuel gauge.** The SRAM parameter table lives at `0xc1618970`,
entries are 36 bytes (`u16 word`, `u8 bitoff`, `u8 nbytes`, `u32 cache`,
`u32 denominator`, `u64 multiplier`, `char *name`, `u32 max`), and decoding
is `value = raw * multiplier / denominator`.  Battery voltage is the upper
16 bits of SRAM word `0x61` scaled by `244141/1000`: a raw value of 15600
gives 3808600 uV, while the running device reported 3811025 uV.

## Sensor inventory

The physical sensors are ST parts behind the hub MCU, not Broadcom:

`LSM6DS3(H)` accelerometer, gyroscope and step counter, plus heart rate,
temperature, SpO2, wear status, PPG, motion state, hand wakeup,
start/stop, calorie, exercise duration, motion notify and high CBT notify
(the vendor types start at `88620000`).  26 sensors in total.

## Booting without flashing

Two images are needed, because `aboot` does not merge a dtbo overlay for a
hand-built image, so the base DTB has to be carried by lk2nd:

```
fastboot boot lk2nd-xtc.img      # 32-bit aboot -> lk2nd -> AArch64
fastboot boot alpine-xtc.img     # kernel + initramfs + DTB
```

Nothing is written to flash; a power cycle always returns to Android.

## Building

```sh
export PATH=/usr/lib/llvm22/bin:$PATH
make ARCH=arm64 LLVM=1 msm8916_defconfig
make ARCH=arm64 LLVM=1 -j$(nproc)
```

`arch/arm64/boot/Image` and `arch/arm64/boot/dts/qcom/msm8937-xtc-nd07.dtb`
are what you need.

## Booting without flashing

This board is booted with `fastboot boot` only — nothing is written to
flash.  The chain is:

```
aboot -> lk2nd -> AArch64 -> Linux
```

`aboot` is a 32-bit LK, so it cannot start an arm64 kernel directly.  lk2nd
switches the CPU to AArch64, hands over the framebuffer through
`simple-framebuffer`, and then boots the real kernel.

## Known pitfalls

* LK compares only the **first** entry of `qcom,msm-id` / `qcom,board-id` —
  a list of ids does nothing.
* A boot image must have `dt_size` (offset 0x28) zero, and `second_size`
  must stay zero or LK hits an assert.
* The `simple-framebuffer` node must not reference the MDSS clocks, or
  `simpledrm` defers forever and the console never appears.
* lk2nd needs `lk2nd.pass-simplefb` on the command line for the handover.
