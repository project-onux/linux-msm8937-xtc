# Z8A (msm8937 / SDM429W) boot assets

`initramfs/init.sh` is PID 1 inside the initramfs: it brings up the USB
gadget so the watch has a serial console, then hands over to
`initramfs/init.c`.

`initramfs/init.c` is a freestanding aarch64 program (raw syscalls, no
libc) that mounts `/dev/mmcblk0p57`, attaches `/data/debian.img` to
`/dev/loop0`, bind-mounts /dev, /proc, /sys, /run, /tmp and /data into the
new root, then `switch_root`s into it.

Build the static init:

    clang --target=aarch64-linux-gnu -nostdlib -static -O2 \
          -ffreestanding -fno-builtin -fno-stack-protector \
          -fuse-ld=lld -o init-c init.c -Wl,-e,_start

`pack2.py` wraps Image + initramfs + dtb into a fastboot-bootable image.
