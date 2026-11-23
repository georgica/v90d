#!/usr/bin/env python3
"""mkpayload.py SHIM IMAGE DTB OUT: the shim (0x100 octets) with the Image's
and the device tree's lengths in its last two words, the Image padded to 4,
the tree; OUT ending .xz is compressed for kjump (ARM branch filter)."""
import lzma, struct, sys
shim, image, dtb = (open(p, "rb").read() for p in sys.argv[1:4])
assert len(shim) == 0x100, "shim is %d octets" % len(shim)
image += b"\0" * (-len(image) % 4)
data = shim[:0xf8] + struct.pack("<II", len(image), len(dtb)) + image + dtb
assert len(data) <= 0x600000, "payload too large: %d" % len(data)
if sys.argv[4].endswith(".xz"):
    data = lzma.compress(data, format=lzma.FORMAT_XZ, check=lzma.CHECK_CRC32,
                         filters=[{"id": lzma.FILTER_ARM},
                                  {"id": lzma.FILTER_LZMA2, "preset": 9 | lzma.PRESET_EXTREME,
                                   "dict_size": 1 << 22}])
open(sys.argv[4], "wb").write(data)
print("%s: %d octets" % (sys.argv[4], len(data)))
