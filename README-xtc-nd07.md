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
| PM660 regulators | ✅ | 9 rails, RPM managed |
| Power key | ✅ | `pm8941-pwrkey` via `pm660.dtsi` |
| eMMC | ✅ | `sdhci-msm`, vmmc l19 / vqmmc l13 |
| Vibrator | ✅ | `gpio-vibra`, TLMM gpio 129 |
| NFC | ✅ | `nxp-nci-i2c`, irq 17 / ven 16 / firmware 130 |
| Touch | ✅ | `zinitix,bt541`, irq 65 / reset 64 |
| WiFi | 🟡 | WCNSS remoteproc + `wcn36xx`; firmware and the `vddpa` rail still need verification |
| Charger | 🚧 | PM660 SMB2; mainline has no driver for this generation |
| Fuel gauge | ❌ | `qpnp,fg` |
| Sensor hub | 🚧 | SSP framework extended for `ssp,bcm4775`; firmware upload path missing |
| Audio | ❌ | needs the LPASS stack plus WCD9335 and AW882xx drivers |
| Camera | ❌ | CAMSS for msm8937, plus SC520CS |
| Display (DRM) | ❌ | currently only the simplefb handover |

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
