#!/usr/bin/env python3
"""Wrap a zImage (with its device tree appended) in a legacy U-Boot image
header, as the HT802's U-Boot (2012.04) `bootm` wants it: ARM Linux kernel,
not compressed (the zImage unpacks itself), load and entry 0xC8008000.

    mkuimage.py IN OUT [VERSION [MODEL]]

The image names itself "v90d MODEL VERSION" (MODEL defaults to ht802): the
loader takes any name that starts with v90d, the running system's `v90nv
upgrade` only writes an image whose MODEL matches this box. The kernel is the
same for every HT80x model; only the name field differs between a model's
images, so they are wrapped from the one zImage.
"""
import struct, sys, time, zlib

LOAD = 0xC8008000
def main(src, dst, version='dev', model='ht802'):
    data = open(src, 'rb').read()
    name = ('v90d ' + model + ' ' + version).encode('ascii')
    assert len(name) < 32, name
    name = name.ljust(32, b'\0')
    def header(hcrc):
        return struct.pack('>7I4B32s', 0x27051956, hcrc, int(time.time()), len(data), LOAD, LOAD,
                           zlib.crc32(data) & 0xFFFFFFFF, 5, 2, 2, 0, name)   # Linux, ARM, kernel, no compression
    h = header(0)
    hcrc = zlib.crc32(h) & 0xFFFFFFFF
    h = h[:4] + struct.pack('>I', hcrc) + h[8:]
    open(dst, 'wb').write(h + data)
    print('%s: %d octets, data crc %08x' % (dst, len(h) + len(data), zlib.crc32(data) & 0xFFFFFFFF))
if __name__ == '__main__':
    if len(sys.argv) not in (3, 4, 5): sys.exit(__doc__)
    main(*sys.argv[1:])
