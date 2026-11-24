#!/usr/bin/env python3
"""What happens to a box when our firmware file goes through the stock
upgrade, played through on a model of the two programs involved: the stock
upgrader (which partitions it writes, with which counter) and the stock boot
loader's choice between the two sets (`gsboot`). Both models are the rules as
we read them out of the unit's own programs (HARDWARE.md, "How the stock
upgrade works" and "How gsboot chooses"), written down again here.

    python3 fw/tools/test_release.py

The first tests use a made-up flash; the last ones start from the backup of
a real unit in recon/backup (read at test time, skipped when absent).
"""
import os, struct, sys, unittest, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import mkgsfw as G

TOP = os.path.dirname(os.path.dirname(HERE))
BACKUP = os.path.join(TOP, 'recon', 'backup', 'flash_all.bin')

# physical offsets: set 0 (A) and set 1 (B)
PARTS = {'core': (0x060000, 0x1A0000), 'base': (0x2E0000, 0x5E0000), 'prog': (0x8E0000, 0xC1F000)}
SIZE = {k: G.KINDS[k][1] for k in PARTS}
UBOOT = (0x020000, 0xFC0000)


def part_ok(flash, off):
    """gsboot's whole test of a partition: the signature, and the payload's
    16-bit sum over the length the header names."""
    h = G.FileHeader(flash[off:off + G.HDR_LEN])
    if h.magic != G.MAGIC:
        return None
    if off + G.HDR_LEN + h.size > len(flash):
        raise RuntimeError('gsboot would run off the flash: a brick')
    body = flash[off + G.HDR_LEN:off + G.HDR_LEN + (h.size & ~1)]
    return h if G.checksum(body) == h.checksum else None


def gsboot_set(flash):
    """The set gsboot starts: 0 or 1."""
    h = [{k: part_ok(flash, PARTS[k][s]) for k in PARTS} for s in (0, 1)]
    ok = [{k: h[s][k] is not None for k in PARTS} for s in (0, 1)]
    counter = [max([h[s][k].counter for k in PARTS if ok[s][k]] or [0]) for s in (0, 1)]
    ver = lambda s, k: h[s][k].version
    def by_counter_then(kinds):
        if counter[0] != counter[1]:
            return int(counter[1] > counter[0])
        for k in kinds:
            if ver(0, k) != ver(1, k):
                return int(ver(1, k) > ver(0, k))
        return 0
    if all(ok[0].values()) and all(ok[1].values()):
        fits = [h[s]['prog'].compatible == (ver(s, 'core'), ver(s, 'base')) for s in (0, 1)]
        if fits[0] != fits[1]:
            return int(fits[1])
        return by_counter_then(('prog', 'base', 'core'))
    if all(ok[0].values()):
        return 0
    if all(ok[1].values()):
        return 1
    both = lambda s: ok[s]['core'] and ok[s]['base']
    if both(0) and both(1):
        return by_counter_then(('base', 'core'))
    if both(0):
        return 0
    if both(1):
        return 1
    return 0


def stock_upgrade(flash, container, running, dev, stop_after=None, cut=None):
    """The stock upgrader run on `flash` by the stock system in set
    `running`: every file of the container that the unit looks for is
    checked and then written, one after the other, into the OTHER set, with
    a counter one above the highest of the running set (its boot loader
    copy included). `stop_after` = power lost after that many files;
    `cut` = (file number, octets) = power lost that far into a file."""
    bad = G.verify(container, dev)
    if bad:
        raise ValueError('; '.join(bad))
    flash = bytearray(flash)
    target = 1 - running
    counters = []
    for off in [PARTS[k][running] for k in PARTS] + [UBOOT[running]]:
        h = G.FileHeader(bytes(flash[off:off + G.HDR_LEN]))
        if h.magic == G.MAGIC:
            counters.append(h.counter)
    new = max(counters) + 1
    names = {v: k for k, v in G.wanted(dev).items()}
    n = 0
    for f in container.files:
        kind = names.get(f.name)
        if kind not in PARTS:
            continue
        if stop_after is not None and n >= stop_after:
            break
        img = bytearray(f.open().raw)
        struct.pack_into('<I', img, 0x30, new)
        off = PARTS[kind][target]
        flash[off:off + SIZE[kind]] = b'\xff' * SIZE[kind]
        if cut is not None and cut[0] == n:
            if cut[1] >= len(img):
                raise ValueError('file %d is only %d octets' % (n, len(img)))
            img = img[:cut[1]]
        flash[off:off + len(img)] = img
        n += 1
        if cut is not None and cut[0] == n - 1:
            break
    return bytes(flash)


def give_up(flash, our_set):
    """What our loader, or the "back to stock" button, does: the first word
    of our core goes to zero."""
    f = bytearray(flash)
    f[PARTS['core'][our_set]:PARTS['core'][our_set] + 4] = bytes(4)
    return bytes(f)


HT802 = G.Device('ht802', 'dvf9918', 1, (0, 0, 0, 0))
WHEN = (2026, 11, 15, 12, 0)


def stock_like(version, counter, running_first=True):
    """A made-up unit: two stock-like sets, set 1 the newer."""
    flash = bytearray(b'\xff' * 0x1000000)
    for s, c in ((0, counter - 1), (1, counter)):
        for kind in PARTS:
            img = G.partition_image(kind, bytes([s + 1]) * 4096, version, 'any', counter=c, when=WHEN)
            flash[PARTS[kind][s]:PARTS[kind][s] + len(img)] = img
        img = G.partition_image('boot', b'\x07' * 512, version, 'dvf9918', counter=c, when=WHEN)
        flash[UBOOT[s]:UBOOT[s] + len(img)] = img
    return bytes(flash)


def our_container(version='90.0.1.0'):
    return G.parse(G.build([('core', b'L' * 6000), ('base', b'S' * 200000), ('prog', b'P' * 64)],
                           model='ht802', version=version, cpu='dvf9918', when=WHEN))[0]


class Model(unittest.TestCase):
    def test_model_agrees_with_a_stock_unit(self):
        flash = stock_like('1.0.65.1', 3)
        self.assertEqual(gsboot_set(flash), 1)

    def test_install(self):
        flash = stock_like('1.0.65.1', 3)
        after = stock_upgrade(flash, our_container(), 1, HT802)
        self.assertEqual(gsboot_set(after), 0)                  # ours
        # the running stock set is exactly as it was
        for kind in PARTS:
            off = PARTS[kind][1]
            self.assertEqual(after[off:off + SIZE[kind]], flash[off:off + SIZE[kind]])
        self.assertEqual(after[:0x060000], flash[:0x060000])
        h = G.FileHeader(after[PARTS['core'][0]:PARTS['core'][0] + G.HDR_LEN])
        self.assertEqual((h.counter, G.version_text(h.version)), (4, '90.0.1.0'))

    def test_power_lost_during_install(self):
        flash = stock_like('1.0.65.1', 3)
        c = our_container()
        for n in (0, 1, 2):                                     # between files
            self.assertEqual(gsboot_set(stock_upgrade(flash, c, 1, HT802, stop_after=n)), 1, n)
        for n in (0, 1, 2):                                     # inside a file, at a page boundary
            for octets in (0, 0x100, 0x200) + ((0x800,) if n < 2 else ()):
                self.assertEqual(gsboot_set(stock_upgrade(flash, c, 1, HT802, cut=(n, octets))), 1, (n, octets))

    def test_the_stock_upgrade_has_one_bad_moment(self):
        # Not ours to mend, but worth knowing: the header is written first,
        # and a header whose signature has arrived while its length field
        # still reads as erased sends gsboot's checksum loop off the end of
        # the flash. That is inside the chip's very first page program of a
        # partition (about a millisecond), in every stock upgrade.
        flash = stock_like('1.0.65.1', 3)
        with self.assertRaises(RuntimeError):
            gsboot_set(stock_upgrade(flash, our_container(), 1, HT802, cut=(0, 4)))

    def test_without_prog_it_would_never_start(self):
        # why the file carries a "prog" although we have no use for one
        flash = stock_like('1.0.65.1', 3)
        c = G.parse(G.build([('core', b'L' * 6000), ('base', b'S' * 200000)], model='ht802', version='90.0.1.0',
                            cpu='dvf9918', when=WHEN))[0]
        self.assertEqual(gsboot_set(stock_upgrade(flash, c, 1, HT802)), 1)

    def test_back_to_stock_and_in_again(self):
        flash = stock_like('1.0.65.1', 3)
        ours = stock_upgrade(flash, our_container(), 1, HT802)
        back = give_up(ours, 0)
        self.assertEqual(gsboot_set(back), 1)
        # the same file once more, from the stock system: ours again
        again = stock_upgrade(back, our_container(), 1, HT802)
        self.assertEqual(gsboot_set(again), 0)
        # or a stock upgrade over what we left: the new stock set wins
        stock = G.parse(G.build([('core', b'k' * 6000), ('base', b'b' * 6000), ('prog', b'p' * 6000)], model='ht802',
                                version='1.0.67.1', cpu='dvf9918', when=WHEN))[0]
        self.assertEqual(gsboot_set(stock_upgrade(back, stock, 1, HT802)), 0)

    def test_installs_from_either_set(self):
        flash = bytearray(stock_like('1.0.65.1', 3))
        for kind in PARTS:                                      # make set 0 the newer, running one
            a, b = PARTS[kind]
            flash[a:a + SIZE[kind]], flash[b:b + SIZE[kind]] = flash[b:b + SIZE[kind]], flash[a:a + SIZE[kind]]
        flash = bytes(flash)
        self.assertEqual(gsboot_set(flash), 0)
        self.assertEqual(gsboot_set(stock_upgrade(flash, our_container(), 0, HT802)), 1)

    def test_other_units_refuse(self):
        flash = stock_like('1.0.65.1', 3)
        for dev in (HT802._replace(cpu='dvf9919'), HT802._replace(model='ht801'), HT802._replace(generation=2)):
            with self.assertRaises(ValueError):
                stock_upgrade(flash, our_container(), 1, dev)


@unittest.skipUnless(os.path.exists(BACKUP), 'no flash backup here: ' + BACKUP)
class RealUnit(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with open(BACKUP, 'rb') as f:
            cls.flash = f.read()

    def test_model_picks_the_set_the_unit_runs(self):
        # HARDWARE.md: this unit runs physical set B
        self.assertEqual(gsboot_set(self.flash), 1)

    def test_install_and_back(self):
        c = our_container()
        ours = stock_upgrade(self.flash, c, 1, HT802)
        self.assertEqual(gsboot_set(ours), 0)
        self.assertEqual(ours[0x1A0000:0x2E0000], self.flash[0x1A0000:0x2E0000])
        self.assertEqual(ours[0x5E0000:0x8E0000], self.flash[0x5E0000:0x8E0000])
        self.assertEqual(ours[0xC1F000:], self.flash[0xC1F000:])
        self.assertEqual(gsboot_set(give_up(ours, 0)), 1)
        for n in (0, 1, 2):
            self.assertEqual(gsboot_set(stock_upgrade(self.flash, c, 1, HT802, stop_after=n)), 1)
            self.assertEqual(gsboot_set(stock_upgrade(self.flash, c, 1, HT802, cut=(n, 0x200))), 1)


def _release_files():
    """(firmware file, the system image that went into it) for the release
    and for the bench variant, as far as they are there."""
    d = os.path.join(TOP, 'fw', 'build', 'release')
    pairs = (('v90d-ht802fw.bin', os.path.join(d, 'v90d-ht802.uimage')),
             ('v90d-ht802fw-bench.bin', os.path.join(TOP, 'fw', 'build', 'v90d-ht802-bench.uimage')))
    return [(os.path.join(d, n), u) for n, u in pairs if os.path.exists(os.path.join(d, n))]


@unittest.skipUnless(_release_files() and os.path.exists(BACKUP), 'needs fw/build/release (make -C fw release) and the flash backup')
class ReleaseFileOnRealUnit(unittest.TestCase):
    def test_install_and_back(self):
        with open(BACKUP, 'rb') as f:
            flash = f.read()
        for path, uimage in _release_files():
            with open(path, 'rb') as f:
                data = f.read()
            c = G.pick(G.parse(data), HT802)                    # what the upload page keeps of the file
            self.assertEqual(len(c.raw), len(data))
            ours = stock_upgrade(flash, c, 1, HT802)
            self.assertEqual(gsboot_set(ours), 0)
            # what gsboot would hand to bootm: the loader as a U-Boot image
            core = ours[0x060000 + 0x200:]
            self.assertEqual(core[:4], bytes.fromhex('27051956'))
            self.assertEqual(core[32:64].rstrip(b'\0'), b'v90d-loader')
            self.assertEqual(struct.unpack('>2I', core[16:24]), (0xC9000000, 0xC9000000))
            # bootm copies it there and then cleans the data cache over four
            # times its length, but only if that range ends on a cache line
            # (HARDWARE.md): a length that is not a multiple of 8 is left unflushed
            self.assertEqual(struct.unpack('>I', core[12:16])[0] % 32, 0)
            # and the system where the loader looks for it: an image of ours, whole
            base = ours[0x2E0000 + 0x200:0x5E0000]
            self.assertEqual(base[:4], bytes.fromhex('27051956'))
            self.assertTrue(base[32:64].startswith(b'v90d ht802 '), base[32:64])
            n, = struct.unpack('>I', base[12:16])
            self.assertEqual(zlib.crc32(base[64:64 + n]) & 0xFFFFFFFF, struct.unpack('>I', base[24:28])[0])
            # the very image it was made from, unless that has been built again since
            if os.path.exists(uimage) and os.path.getmtime(uimage) <= os.path.getmtime(path):
                with open(uimage, 'rb') as f:
                    u = f.read()
                self.assertEqual(base[:len(u)], u)
            else:
                print('  (%s: its system image has been rebuilt since, not compared)' % os.path.basename(path))
            self.assertEqual(gsboot_set(give_up(ours, 0)), 1)
            for n in (0, 1, 2):
                self.assertEqual(gsboot_set(stock_upgrade(flash, c, 1, HT802, stop_after=n)), 1)


if __name__ == '__main__':
    unittest.main(verbosity=1)
