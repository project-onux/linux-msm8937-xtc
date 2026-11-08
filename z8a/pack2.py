import struct, sys
PAGE = 2048
DTB_OFFSET = 0x2C

def pad(b):
    r = len(b) % PAGE
    out = b if r == 0 else b + b"\0" * (PAGE - r)
    print("  pad: %d -> %d" % (len(b), len(out)))
    return out

img_path = sys.argv[1]
raw = open(img_path, "rb").read()
print("Image file  : %d bytes" % len(raw))
print("ARMd magic  : %r at 0x38" % raw[0x38:0x3C])
kernel = bytearray(raw)
struct.pack_into("<I", kernel, DTB_OFFSET, len(kernel))
print("patched 0x2C: %d" % struct.unpack_from("<I", kernel, DTB_OFFSET)[0])

dtb = open(sys.argv[3], "rb").read()
ramdisk = open(sys.argv[2], "rb").read()
full_kernel = bytes(kernel) + dtb
print("full_kernel : %d (first bytes %s)" % (len(full_kernel), full_kernel[:8].hex()))

ks, ka, rs, ra = len(full_kernel), 0x80080000, len(ramdisk), 0x82000000
hdr = bytearray(PAGE)
hdr[0:8] = b"ANDROID!"
struct.pack_into("<8I", hdr, 0x08, ks, ka, rs, ra, 0, 0x80F00000, 0x80000100, PAGE)
struct.pack_into("<I", hdr, 0x28, 0)
struct.pack_into("<I", hdr, 0x2C, 0x10040138)
cb = sys.argv[5].encode()
hdr[0x40:0x40+len(cb)] = cb

kb = pad(full_kernel); rb = pad(ramdisk)
data = bytes(hdr) + kb + rb
print("total       : %d = %d + %d + %d" % (len(data), PAGE, len(kb), len(rb)))
print("check [2048:2056] =", data[2048:2056].hex())
open(sys.argv[4], "wb").write(data)
print("written     :", sys.argv[4])
