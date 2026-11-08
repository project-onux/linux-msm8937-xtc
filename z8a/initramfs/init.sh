#!/bin/sh
export PATH=/sbin:/usr/sbin:/bin:/usr/bin
mount -t proc proc /proc 2>/dev/null
mount -t sysfs sys /sys 2>/dev/null
mount -t tmpfs tmp /tmp 2>/dev/null
mount -t devtmpfs dev /dev 2>/dev/null
mount -t configfs none /sys/kernel/config 2>/dev/null

# Leave room for the boot logo: 240 px / 16 px font = 15 rows.
i=0
while [ $i -lt 15 ]; do echo; i=$((i+1)); done

# ---------------------------------------------------------------------------
# eMMC read self-test.  "early" runs before anything else touches the disk,
# "late" is repeated at the end of this script; if only early succeeds, some
# later action is breaking the controller.
# ---------------------------------------------------------------------------
# NOTE: busybox grep will not match inside binary data even with -a, so this
# compares the GPT header sector against the signature via md5 instead.
gpt_ok() {
    dd if=/dev/mmcblk0 bs=512 count=1 skip=1 2>/dev/null | md5sum | cut -c1-8
}

i=0
while [ ! -b /dev/mmcblk0 ] && [ $i -lt 50 ]; do
    sleep 0.1
    i=$((i + 1))
done
echo "mmc probe: early-gpt-md5=[$(gpt_ok)] after ${i}x0.1s" | tee -a /tmp/mmcprobe.log
echo "mmc probe: early-p57-md5=[$(dd if=/dev/mmcblk0p57 bs=512 count=1 2>/dev/null | md5sum | cut -c1-8)]" | tee -a /tmp/mmcprobe.log

echo "========================================"
echo "     Alpine Linux on XTC Z8A (ND07)"
echo "     msm8937 / SDM429W / Wear 4100+"
echo "========================================"
uname -sr
echo
[ -e /proc/device-tree/model ] && \
    echo "model: $(tr -d "\000" < /proc/device-tree/model)"
[ -d /sys/class/graphics/fb0 ] && \
    echo "fb0  : $(cat /sys/class/graphics/fb0/name) $(cat /sys/class/graphics/fb0/virtual_size)"
echo "cpus : $(grep -c ^processor /proc/cpuinfo)"
grep MemTotal /proc/meminfo

# ---------------------------------------------------------------------------
# USB gadget: one CDC-ACM port so the watch can be driven from a PC.
# ---------------------------------------------------------------------------
GADGET=/sys/kernel/config/usb_gadget/z8a
if [ -d /sys/kernel/config/usb_gadget ]; then
    mkdir -p $GADGET
    cd $GADGET || true
    echo 0x1d6b > idVendor
    echo 0x0104 > idProduct
    echo 0x0100 > bcdDevice
    echo 0x0200 > bcdUSB

    mkdir -p strings/0x409
    echo "xtc" > strings/0x409/serialnumber
    echo "XTC" > strings/0x409/manufacturer
    echo "Z8A (ND07)" > strings/0x409/product

    mkdir -p configs/c.1/strings/0x409
    echo "ACM serial" > configs/c.1/strings/0x409/configuration
    echo 250 > configs/c.1/MaxPower

    mkdir -p functions/acm.usb0
    ln -sf functions/acm.usb0 configs/c.1/acm.usb0 2>/dev/null

    mkdir -p functions/ncm.usb0
    ln -sf functions/ncm.usb0 configs/c.1/ncm.usb0 2>/dev/null

    UDC=$(ls /sys/class/udc 2>/dev/null | head -n 1)
    if [ -n "$UDC" ]; then
        echo "$UDC" > UDC 2>/dev/null && echo "gadget: bound to $UDC"
    else
        echo "gadget: no UDC found"
    fi
fi

# USB network: give the watch a fixed address so the host can reach it.
if [ -d /sys/class/net/usb0 ]; then
    ip addr add 192.168.42.1/24 dev usb0 2>/dev/null
    ip link set usb0 up 2>/dev/null && echo "net   : usb0 192.168.42.1/24"
    telnetd -l /bin/sh -p 23 >/dev/null 2>&1 &
fi

# The shell on the USB serial line is started at the very end, once the
# gadget's tty actually exists (it does not when configfs is first bound).

echo
echo "--- dmesg tail ---"
dmesg | tail -n 12

# ---------------------------------------------------------------------------
# One-shot hardware report. Booting mainline kills adb, so this has to say
# everything about driver state in a single screenful.
# ---------------------------------------------------------------------------
check() {
    printf "  %-14s %s\n" "$1" "$2"
}
echo
echo "=== hardware report ==="

# storage
if [ -b /dev/mmcblk0 ]; then
    check storage "$(cat /sys/block/mmcblk0/size) sectors"
else
    check storage FAIL
fi

# power
if [ -d /sys/class/power_supply ]; then
    for ps in /sys/class/power_supply/*; do
        [ -e "$ps/type" ] || continue
        check "ps:$(basename $ps)" "$(cat $ps/type 2>/dev/null)"
    done
else
    check power FAIL
fi

# sensors
check iio "$(ls /sys/bus/iio/devices 2>/dev/null | wc -l) devices"
grep -qi ssp /proc/modules 2>/dev/null || true

# input
for i in /sys/class/input/input*; do
    [ -e "$i/name" ] || continue
    check "input" "$(cat $i/name)"
done

# display
[ -d /sys/class/graphics/fb0 ] && check fb0 "$(cat /sys/class/graphics/fb0/virtual_size)"

# network
for n in /sys/class/net/*; do
    check "net:$(basename $n)" "$(cat $n/address 2>/dev/null)"
done

# usb
UDCS=$(ls /sys/class/udc 2>/dev/null | tr '\n' ' ')
check udc "${UDCS:-none}"
[ -c /dev/ttyGS0 ] && check ttyGS0 present

# camera
NVID=$(ls /dev/video* 2>/dev/null | wc -l)
check video "$NVID nodes"

# driver probe results, straight from the log
echo
echo "=== driver probe ==="
dmesg | grep -iE "aw882xx|ssp|wcn36xx|qcom-fg|camss|ov5675|zinitix|nxp-nci|vibra|smb2|chipidea" \
    | grep -iE "probe|fail|error|detected|registered|bound" | head -n 20

# remoteproc / audio / gadget state, the three things that decide the demo
echo
echo "=== subsystems ==="
for rp in /sys/class/remoteproc/*; do
    [ -e "$rp" ] && check "rproc:$(cat $rp/name 2>/dev/null)" "$(cat $rp/state 2>/dev/null)"
done
check udc "$(ls /sys/class/udc 2>/dev/null | tr "\n" " ")"
check sound-cards "$(ls /dev/snd 2>/dev/null | tr "\n" " ")"
[ -r /proc/asound/cards ] && sed -n "2,6p" /proc/asound/cards
check fg "$(cat /sys/class/power_supply/pm660-fg/voltage_now 2>/dev/null) uV"
check fg-soc "$(cat /sys/class/power_supply/pm660-fg/capacity 2>/dev/null) %"
echo "-- regulator/rpm --"
dmesg | grep -iE "regulator|rpm-smd|rpm_smd" | tail -n 5
echo "-- smem/smp2p --"
dmesg | grep -iE "smp2p|smem|mbox" | head -n 5
echo "-- usb --"
dmesg | grep -iE "chipidea|ci_hdrc|dwc3|udc|usb_phy|qusb" | tail -n 5
dmesg | grep -iE "adsp|apr|q6|slim|remoteproc" | tail -n 6

echo
echo "=== usb final ==="
G=/sys/kernel/config/usb_gadget/z8a
echo "udc-list=[$(ls /sys/class/udc 2>/dev/null | tr '\n' ' ')]"
echo "gadget-UDC=[$(cat $G/UDC 2>/dev/null)]"
echo "configs=[$(ls $G/configs/c.1 2>/dev/null | tr '\n' ' ')]"
echo "functions=[$(ls $G/functions 2>/dev/null | tr '\n' ' ')]"
U=ci_hdrc.0
echo "$U" > $G/UDC 2>/dev/null
echo "bind-rc=$?"
echo "after: UDC=[$(cat $G/UDC 2>/dev/null)]"
for f in state current_speed maximum_speed is_otg is_a_peripheral; do
    echo "  $f=[$(cat /sys/class/udc/$U/$f 2>/dev/null)]"
done
dmesg | grep -iE "gadget|udc|chipidea|ci_hdrc|vbus" | tail -n 6
echo "=== usb regs ==="
# chipidea: capoffset 0x100 -> USBCMD 0x140, OTGSC 0x1a4, USBMODE 0x1a8
for a in 0x78db140 0x78db1a4 0x78db1a8 0x6c000 0x6c004 0x6c040; do
    echo "  $a=$(devmem $a 2>/dev/null)"
done
echo "=== phy ==="
ls /sys/class/phy 2>/dev/null | head -3
for d in /sys/bus/platform/drivers/*phy*; do
    [ -e "$d" ] && echo "  drv:$(basename $d) bound=[$(ls $d | grep -v -E 'bind|unbind|uevent|module' | tr '\n' ' ')]"
done
dmesg | grep -iE "femtophy|6c000|snps|phy" | tail -n 8
echo "=== power ==="
for p in /sys/class/power_supply/*; do
    echo "  $(basename $p): online=$(cat $p/online 2>/dev/null) status=$(cat $p/status 2>/dev/null) type=$(cat $p/type 2>/dev/null)"
done
echo "=== usb state ==="
echo "  udc=$U state=[$(cat /sys/class/udc/$U/state 2>/dev/null)]"
echo "  ttyGS0=[$([ -c /dev/ttyGS0 ] && echo present || echo missing)]"
echo "  net=[$(ls /sys/class/net | tr '\n' ' ')]"

echo "mmc probe: late-gpt-md5=[$(gpt_ok)]" | tee -a /tmp/mmcprobe.log
echo "mmc probe: read-back=[$(dd if=/dev/mmcblk0 bs=512 count=4 2>/dev/null | md5sum | cut -c1-8)]" | tee -a /tmp/mmcprobe.log

# ---------------------------------------------------------------------------
# USB serial console.  Wait for the ACM tty to appear, then hand it a shell.
# This is the only way to type into the watch, so it must not be racy.
# ---------------------------------------------------------------------------
i=0
while [ ! -c /dev/ttyGS0 ] && [ $i -lt 40 ]; do
    sleep 0.2
    i=$((i + 1))
done
if [ -c /dev/ttyGS0 ]; then
    echo "=== serial console on /dev/ttyGS0 (waited ${i}x0.2s) ==="
    if [ -x /init-c ]; then
        echo "=== handing over to the static init (/init-c) ==="
        exec /init-c
    fi
    echo "=== /init-c missing, falling back to a busybox shell ==="
    setsid sh -c 'exec sh -i </dev/ttyGS0 >/dev/ttyGS0 2>&1' &
else
    echo "=== no /dev/ttyGS0 after ${i}x0.2s ==="
fi

echo "=== ready ==="

# PID 1 exiting panics the kernel ("Attempted to kill init!"), and a SIGHUP
# from the serial port closing used to do exactly that.  Sleep forever; the
# interactive shell lives on /dev/ttyGS0 as a child.
while true; do
    sleep 3600
done
