#!/usr/bin/env python3
"""Tests for mkgsfw.py: the AES against the FIPS-197 examples, the reader and
the checks against a genuine stock firmware file, and the files we build
against the same checks.

    python3 fw/tools/test_mkgsfw.py

The stock file is read from recon/ at test time (nothing of it is kept
here); without it those tests are skipped and say so.
"""
import os, struct, sys, unittest, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import mkgsfw as G

TOP = os.path.dirname(os.path.dirname(HERE))
STOCK = os.path.join(TOP, 'recon', 'fw', 'x1.0.65.1', 'Release_HT802_1.0.65.1', 'ht802fw.bin')
OLD = os.path.join(TOP, 'recon', 'fw', 'x1.0.11.6', 'Release_HT802_1.0.11.6', 'ht802fw.bin')
FLASH = os.path.join(TOP, 'recon', 'backup')
def rd(path):
    with open(path, 'rb') as f:
        return f.read()

HT802 = G.Device(model='ht802', cpu='dvf9918', generation=1, required_bits=(0, 0, 0, 0))


class Aes(unittest.TestCase):
    def test_fips197_appendix_b(self):
        a = G.AES(bytes.fromhex('2b7e151628aed2a6abf7158809cf4f3c'))
        c = a.encrypt_block(bytes.fromhex('3243f6a8885a308d313198a2e0370734'))
        self.assertEqual(c.hex(), '3925841d02dc09fbdc118597196a0b32')
        self.assertEqual(a.decrypt_block(c).hex(), '3243f6a8885a308d313198a2e0370734')

    def test_fips197_appendix_c1(self):
        a = G.AES(bytes(range(16)))
        p = bytes.fromhex('00112233445566778899aabbccddeeff')
        c = a.encrypt_block(p)
        self.assertEqual(c.hex(), '69c4e0d86a7b0430d8cdb78070b4c55a')
        self.assertEqual(a.decrypt_block(c), p)

    def test_fips197_appendix_c3(self):
        a = G.AES(bytes(range(32)))
        p = bytes.fromhex('00112233445566778899aabbccddeeff')
        c = a.encrypt_block(p)
        self.assertEqual(c.hex(), '8ea2b7ca516745bfeafc49904b496089')
        self.assertEqual(a.decrypt_block(c), p)

    def test_cbc_sp800_38a(self):
        # NIST SP 800-38A F.2.1, first two blocks: one 32-octet unit as the
        # container uses them, only with that example's vector in place of the fixed one
        key = bytes.fromhex('2b7e151628aed2a6abf7158809cf4f3c')
        iv = bytes(range(16))
        p = bytes.fromhex('6bc1bee22e409f96e93d7e117393172a' 'ae2d8a571e03ac9c9eb76fac45af8e51')
        c = G.cbc_units(key, p, False, iv=iv)
        self.assertEqual(c.hex(), '7649abac8119b246cee98e9b12e9197d' '5086cb9b507219ee95db113a917678b2')
        self.assertEqual(G.cbc_units(key, c, True, iv=iv), p)

    def test_units_are_independent(self):
        key = bytes(range(16))
        p = bytes(range(64))
        c = G.cbc_units(key, p, False)
        self.assertEqual(c[32:], G.cbc_units(key, p[32:], False))
        self.assertEqual(G.cbc_units(key, c, True), p)
        with self.assertRaises(ValueError):
            G.cbc_units(key, bytes(40), False)

    def test_refuses_other_key_sizes(self):
        with self.assertRaises(ValueError):
            G.AES(bytes(24))


class Format(unittest.TestCase):
    def test_checksum(self):
        self.assertEqual(G.checksum(b''), 0)
        self.assertEqual(G.checksum(b'\x01\x00\x02\x00'), 0x10000 - 3)
        self.assertEqual(G.checksum(b'\xff\xff\x01\x00'), 0)
        with self.assertRaises(ValueError):
            G.checksum(b'\x01')

    def test_version_text(self):
        self.assertEqual(G.version_int('1.0.65.2'), 0x01004102)
        self.assertEqual(G.version_text(0x5A000100), '90.0.1.0')
        with self.assertRaises(ValueError):
            G.version_int('1.0.256.0')

    def test_uimage(self):
        u = G.uimage(b'abcd' * 8, name='v90d-loader', load=0xC9000000, when=0)
        self.assertEqual(len(u), 64 + 32)
        magic, hcrc, when, size, load, entry, dcrc = struct.unpack('>7I', u[:28])
        self.assertEqual((magic, size, load, entry), (0x27051956, 32, 0xC9000000, 0xC9000000))
        self.assertEqual(dcrc, zlib.crc32(b'abcd' * 8))
        self.assertEqual(hcrc, zlib.crc32(u[:4] + bytes(4) + u[8:64]))
        self.assertEqual(u[32:64].rstrip(b'\0'), b'v90d-loader')


def ours(**kw):
    files = [('core', b'\x11' * 1000), ('base', b'\x22' * 5000), ('prog', b'\x33' * 64)]
    args = dict(model='ht802', version='90.0.1.0', cpu='dvf9918', when=(2026, 11, 15, 12, 0))
    args.update(kw)
    return G.build(files, **args)


class Ours(unittest.TestCase):
    def test_passes_the_stock_checks(self):
        img = ours()
        cs = G.parse(img)
        self.assertEqual(len(cs), 1)
        c = cs[0]
        self.assertEqual([f.name for f in c.files], ['ht802core.bin', 'ht802base.bin', 'ht802prog.bin'])
        self.assertEqual(G.verify(c, HT802), [])
        self.assertEqual(len(img), 0x288 + sum(f.size for f in c.files))
        prog = c.files[2].open()
        self.assertEqual(prog.compatible, (G.version_int('90.0.1.0'),) * 2)
        self.assertEqual(prog.counter, 0)
        for f, fill in zip(c.files, (0x11, 0x22, 0x33)):
            h = f.open()
            self.assertEqual(h.size % 32, 0)
            self.assertEqual(h.payload[:60], bytes([fill]) * 60)
            self.assertEqual(h.cpu, 'dvf9918')
            self.assertEqual(h.model_key, 0xFD21)

    def test_what_the_box_would_refuse(self):
        c = G.parse(ours())[0]
        why = lambda dev: ' / '.join(G.verify(c, dev))
        self.assertIn('cpu', why(HT802._replace(cpu='dvf9919')))
        self.assertIn('name', why(HT802._replace(model='ht801')))
        self.assertIn('generation', why(HT802._replace(generation=2)))
        self.assertEqual(why(HT802._replace(required_bits=(0, 0, 0, 0x4000))), '')
        c2 = G.parse(ours(bits=(0, 0, 0, 0x0010)))[0]
        self.assertEqual(G.verify(c2, HT802._replace(required_bits=(0, 0, 0, 0x0010))), [])
        self.assertIn('bits', ' / '.join(G.verify(c2, HT802._replace(required_bits=(0, 0, 0, 0x4000)))))

    def test_damage_is_seen(self):
        img = bytearray(ours())
        img[0x288 + 0x200 + 700] ^= 1                 # one bit in the first file's body
        self.assertIn('checksum', ' / '.join(G.verify(G.parse(bytes(img))[0], HT802)))
        img = bytearray(ours())
        img[0x1C4] ^= 0x20                            # a size in the container header
        with self.assertRaises(ValueError):
            G.parse(bytes(img))
        img = bytearray(ours())
        img[0x1E0] ^= 1                               # a version in the container header
        self.assertIn('version', ' / '.join(G.verify(G.parse(bytes(img))[0], HT802)))

    def test_too_big_for_its_partition(self):
        with self.assertRaises(ValueError):
            G.build([('core', bytes(1280 * 1024))], model='ht802', version='90.0.1.0', cpu='dvf9918')

    def test_partition_image(self):
        p = G.partition_image('base', b'\x22' * 5000, version='90.0.1.0', cpu='dvf9918', counter=7, when=(2026, 11, 15, 12, 0))
        h = G.FileHeader(p)
        self.assertEqual((h.magic, h.counter, h.image_id), (G.MAGIC, 7, 0x0D))
        self.assertEqual(G.checksum(p[0x200:0x200 + h.size]), h.checksum)
        self.assertEqual(p[0x200:0x200 + 5000], b'\x22' * 5000)


@unittest.skipUnless(os.path.exists(STOCK), 'stock firmware file not here: ' + STOCK)
class Stock(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cs = G.parse(rd(STOCK))

    def test_lists(self):
        self.assertGreaterEqual(len(self.cs), 1)
        c = G.pick(self.cs, HT802)
        names = [f.name for f in c.files]
        for want in ('ht802boot.bin', 'ht802core.bin', 'ht802base.bin', 'ht802prog.bin'):
            self.assertIn(want, names)
        self.assertEqual(c.generation, 1)
        self.assertEqual(G.version_text(c.files[names.index('ht802core.bin')].version), '1.0.65.1')

    def test_verifies(self):
        c = G.pick(self.cs, HT802)
        self.assertEqual(G.verify(c, HT802), [])
        core = [f for f in c.files if f.name == 'ht802core.bin'][0].open()
        self.assertEqual(core.payload[:4], bytes.fromhex('27051956'))   # a U-Boot image inside
        self.assertEqual(core.cpu, 'dvf9918')

    def test_same_header_as_in_flash(self):
        # what the upgrader writes is the decrypted file, with the counter filled in
        path = os.path.join(FLASH, 'mtd_base.bin')
        if not os.path.exists(path):
            self.skipTest('no flash backup here')
        flash = rd(path)
        c = G.pick(self.cs, HT802)
        base = [f for f in c.files if f.name == 'ht802base.bin'][0].open()
        self.assertEqual(flash[:0x30], base.raw[:0x30])
        self.assertEqual(flash[0x34:0x200], base.raw[0x34:0x200])
        self.assertEqual(flash[0x200:0x200 + base.size], base.payload)

    @unittest.skipUnless(os.path.exists(OLD), 'old stock firmware file not here')
    def test_old_release_is_refused_as_on_the_unit(self):
        c = G.parse(rd(OLD))[0]
        self.assertIn('generation', ' / '.join(G.verify(c, HT802, bodies=False)))


if __name__ == '__main__':
    unittest.main(verbosity=1)
