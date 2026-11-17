#!/usr/bin/env python3
"""The running system's side of the boot scheme (`v90nv where`, `confirm`,
`to-stock`, `upgrade`, `conf-save`, and the two it does for the web page),
run on 16 MiB flash images in files and judged by what the box would do at
its next start. That is decided by models of the three programs that take
part: U-Boot's boot command of the bench scheme (written down here from
tools/mkenvlist.py), the stock boot loader's choice of set (test_release.py)
and our loader itself, the real code, on the flash simulator
(loader/test/flash_decide.c).

    python3 fw/tools/test_v90nv_boot.py            the host program
    V90NV="qemu-arm-static fw/tools/v90nv.arm" python3 fw/tools/test_v90nv_boot.py

    python3 fw/tools/test_v90nv_boot.py --make-flash FLASH.bin NEW.uimage
                                                    (a box and an image for other tests)

Power cuts: v90nv stops before flash step N when V90NV_CUT=N is set (a
step is one sector erase or one page program; plain files only), with
V90NV_TORN=1 after doing half of that step.
"""
import os, shutil, socket, struct, subprocess, sys, tempfile, unittest, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import mkgsfw as G
import test_release as R

TOP = os.path.dirname(os.path.dirname(HERE))
NV = os.environ.get('V90NV', os.path.join(HERE, 'v90nv.host')).split()
DECIDE = os.path.join(TOP, 'fw', 'loader', 'build', 'flash_decide')
BACKUP = R.BACKUP
SCRATCH = os.environ.get('V90NV_SCRATCH')       # where a copy of the backup may be worked on

CORE, BASE, PROG = R.PARTS['core'], R.PARTS['base'], R.PARTS['prog']
ENV = 0xFBE000
LOAD = 0xC8008000
BOARD = b'9610003725A\0'
WHEN = R.WHEN


def system(seed, n, name='v90d ht802 0.1.0', load=LOAD):
    data = bytes((seed + i * 31 + (i >> 8)) & 255 for i in range(n))
    return G.uimage(data, name, load, when=1)


LOADER = G.uimage(b'L' * 5776, 'v90d-loader', 0xC9000000, when=1)
OLD_SYS = system(1, 30000)
NEW_SYS = system(2, 21000)


def crc_of(uimg):
    return struct.unpack('>I', uimg[24:28])[0]


def release(sysimg=OLD_SYS, version='90.0.1.0', model='ht802', cpu='dvf9918'):
    return G.build([('core', LOADER), ('base', sysimg), ('prog', b'v90d: not used\n\0')], model=model, version=version,
                   cpu=cpu, when=WHEN)


def stock_flash():
    f = bytearray(R.stock_like('1.0.65.1', 3))
    f[0x4020:0x4020 + len(BOARD)] = BOARD
    return bytes(f)


def loader_flash(our_set=0, sysimg=OLD_SYS):
    """A stock unit whose stock system, running from the other set, has
    installed our firmware file."""
    f = bytearray(stock_flash())
    if our_set == 1:                                    # make set 0 the newer, running stock set
        for kind in R.PARTS:
            a, b = R.PARTS[kind]
            n = R.SIZE[kind]
            f[a:a + n], f[b:b + n] = f[b:b + n], f[a:a + n]
    c = G.parse(release(sysimg))[0]
    return R.stock_upgrade(bytes(f), c, 1 - our_set, R.HT802)


def env_image(tries):
    lines = ['bootcmd=run v90d_try; gsboot', 'bootdelay=1', 'v90d_boot=saveenv && bootm 0x802E0000', 'v90d_tries=%d' % tries]
    data = b''.join(l.encode() + b'\0' for l in lines)
    data = data + bytes(8188 - len(data))
    return struct.pack('<I', zlib.crc32(data) & 0xFFFFFFFF) + data


def blob(data):
    return b'V90D' + struct.pack('<II', len(data), zlib.crc32(data) & 0xFFFFFFFF) + data


def old_flash(sysimg=OLD_SYS, tries=3, conf=b'number 5551234\n'):
    """The bench box: stock in set B, our image raw in base A, settings at
    the start of core A, the trial script in U-Boot's environment."""
    f = bytearray(stock_flash())
    f[CORE[0]:CORE[0] + 0x10000] = b'\xff' * 0x10000
    b = blob(conf)
    f[CORE[0]:CORE[0] + len(b)] = b
    f[BASE[0]:BASE[0] + R.SIZE['base']] = b'\xff' * R.SIZE['base']
    f[BASE[0]:BASE[0] + len(sysimg)] = sysimg
    f[ENV:ENV + 8192] = env_image(tries)
    return bytes(f)


# ---------------------------------------------------------------- the box's next start
def env_vars(flash):
    e = flash[ENV:ENV + 8192]
    if struct.unpack('<I', e[:4])[0] != zlib.crc32(e[4:]) & 0xFFFFFFFF:
        return None
    out = {}
    for v in e[4:].split(b'\0\0')[0].split(b'\0'):
        if b'=' in v:
            k, x = v.split(b'=', 1)
            out[k.decode()] = x.decode()
    return out


def uimage_ok(flash, off):
    h = flash[off:off + 64]
    if h[:4] != bytes.fromhex('27051956'):
        return False
    if zlib.crc32(h[:4] + bytes(4) + h[8:]) & 0xFFFFFFFF != struct.unpack('>I', h[4:8])[0]:
        return False
    n, = struct.unpack('>I', h[12:16])
    return off + 64 + n <= len(flash) and zlib.crc32(flash[off + 64:off + 64 + n]) & 0xFFFFFFFF == struct.unpack('>I', h[24:28])[0]


def next_start(path):
    """Power on. Returns ('old', crc) for our image started by U-Boot's
    trial script, ('loader', set, image set, crc) for our image started by
    our loader, ('stock', set) for the stock system; the file is left as the
    flash would be after it (counters counted, a set given up)."""
    for _ in range(6):
        with open(path, 'rb') as f:
            flash = bytearray(f.read())
        assert len(flash) == 0x1000000
        env = env_vars(flash)
        # Seen on the unit (HARDWARE.md, Bootloader): with no v90d_tries at
        # all, `test "${v90d_tries}" = 1` of this U-Boot comes out true. The
        # script then sets the variable to 0, saves and starts our image.
        tries = env.get('v90d_tries', '1') if env else None
        if env and 'v90d_try' in env.get('bootcmd', '') and tries in ('1', '2', '3'):
            e = bytearray(flash[ENV:ENV + 8192])
            if 'v90d_tries' in env:
                old = b'v90d_tries=' + tries.encode()
                new = b'v90d_tries=%d' % (int(tries) - 1)
                e = bytearray(e.replace(old, new))
            else:
                end = e.index(b'\0\0', 4)
                add = b'\0v90d_tries=0\0'
                e[end:end + len(add)] = add
            e[:4] = struct.pack('<I', zlib.crc32(bytes(e[4:])) & 0xFFFFFFFF)
            flash[ENV:ENV + 8192] = e
            with open(path, 'wb') as f:
                f.write(flash)
            if uimage_ok(flash, BASE[0]):
                return ('old', crc_of(flash[BASE[0]:BASE[0] + 64]))
        s = R.gsboot_set(bytes(flash))                  # raises if gsboot would run off the flash
        payload = flash[CORE[s] + 0x200:CORE[s] + 0x240]
        if payload[32:44] != b'v90d-loader\0':
            return ('stock', s)
        out = subprocess.run([DECIDE, '-w', path, str(s)], capture_output=True, text=True, check=True).stdout.split()
        if out[0] == 'BOOT':
            return ('loader', s, int(out[3]), int(out[6], 16))
    raise AssertionError('the box never gets anywhere')


class Box:
    """A flash image in a file and v90nv run on it."""
    def __init__(self, test, flash):
        self.dir = tempfile.mkdtemp(prefix='v90nv-')
        test.addCleanup(shutil.rmtree, self.dir, True)
        self.path = os.path.join(self.dir, 'flash.bin')
        self.set(flash)

    def set(self, flash):
        with open(self.path, 'wb') as f:
            f.write(flash)

    def get(self):
        with open(self.path, 'rb') as f:
            return f.read()

    def file(self, name, data):
        p = os.path.join(self.dir, name)
        with open(p, 'wb') as f:
            f.write(data)
        return p

    def nv(self, *args, cut=None, torn=False, stdin=None, extra_env=None, pass_fds=()):
        env = dict(os.environ)
        env.pop('V90NV_CUT', None); env.pop('V90NV_TORN', None)
        if cut is not None:
            env['V90NV_CUT'] = str(cut)
        if torn:
            env['V90NV_TORN'] = '1'
        env.update(extra_env or {})
        p = subprocess.run(NV + ['-f', self.path] + list(args), capture_output=True, env=env, stdin=stdin, pass_fds=pass_fds)
        return p.returncode, p.stdout.decode('latin-1') + p.stderr.decode('latin-1')

    def where(self):
        rc, out = self.nv('where')
        d = dict(l.split('=', 1) for l in out.splitlines() if '=' in l)
        d['rc'] = rc
        return d

    def start(self):
        return next_start(self.path)


def rd(path):
    with open(path, 'rb') as f:
        return f.read()


def diff(a, b):
    """The ranges [lo, hi) in which two flash images differ."""
    out, i, n = [], 0, len(a)
    if a == b:
        return out
    step = 4096
    for s in range(0, n, step):
        if a[s:s + step] != b[s:s + step]:
            lo = s + next(k for k in range(step) if a[s + k] != b[s + k])
            hi = s + step - next(k for k in range(step) if a[s + step - 1 - k] != b[s + step - 1 - k])
            if out and out[-1][1] == s and lo == s:
                out[-1] = (out[-1][0], hi)
            else:
                out.append((lo, hi))
    return out


def within(ranges, allowed):
    return all(any(lo >= a and hi <= b for a, b in allowed) for lo, hi in ranges)


class Where(unittest.TestCase):
    def test_loader_in_either_set(self):
        for s in (0, 1):
            b = Box(self, loader_flash(s))
            w = b.where()
            self.assertEqual(w['rc'], 0)
            self.assertEqual((w['scheme'], w['set']), ('loader', 'AB'[s]))
            self.assertEqual(int(w['core'], 16), CORE[s])
            self.assertEqual(int(w['state'], 16), CORE[s] + 0x10000)
            self.assertEqual(int(w['settings'], 16), CORE[s] + 0x20000)
            self.assertEqual(int(w['upgrade'], 16), BASE[s])
            self.assertEqual(int(w['image'], 16), BASE[s] + 0x200)
            self.assertEqual(w['image_name'], 'v90d ht802 0.1.0')
            self.assertEqual(w['stock'], 'AB'[1 - s] + ' good')
            self.assertEqual(w['board'], '9610003725A known')
            self.assertEqual(w['tried'], 'yes')
            self.assertEqual(w['unconfirmed'], '0')
            self.assertEqual(b.get(), loader_flash(s))  # looking writes nothing

    def test_old_scheme(self):
        b = Box(self, old_flash(tries=2))
        w = b.where()
        self.assertEqual((w['scheme'], w['set'], w['old_tries']), ('old', '-', '2'))
        self.assertEqual(int(w['settings'], 16), 0x060000)
        self.assertEqual(int(w['upgrade'], 16), 0x2E0000)
        self.assertEqual(int(w['image'], 16), 0x2E0000)
        self.assertEqual(w['stock'], 'B good')

    def test_a_stock_unit_or_a_ram_start(self):
        b = Box(self, stock_flash())
        w = b.where()
        self.assertEqual(w['scheme'], 'none')
        for cmd in (['confirm'], ['to-stock'], ['upgrade', b.file('n', NEW_SYS)], ['conf-save', b.file('c', b'x')]):
            rc, _ = b.nv(*cmd)
            self.assertEqual(rc, 1, cmd)
        self.assertEqual(b.get(), stock_flash())

    def test_installed_over_the_bench_scheme(self):
        # the bench box after our file went through the stock upgrade: the
        # trial script is still in the environment but finds no image
        f = bytearray(loader_flash(0))
        f[ENV:ENV + 8192] = env_image(3)
        b = Box(self, bytes(f))
        w = b.where()
        self.assertEqual((w['scheme'], w['set'], w['old_tries']), ('loader', 'A', '3'))
        self.assertEqual(b.start()[:3], ('loader', 0, 0))
        self.assertEqual(b.nv('confirm')[0], 0)
        # the count in the environment is U-Boot's to run down; we leave it
        self.assertEqual(env_vars(b.get())['v90d_tries'], '2')

    def test_not_a_flash_image(self):
        b = Box(self, b'\xff' * 4096)
        self.assertEqual(b.nv('where')[0], 2)


class Confirm(unittest.TestCase):
    def test_loader(self):
        for s in (0, 1):
            b = Box(self, loader_flash(s))
            self.assertEqual(b.start(), ('loader', s, s, crc_of(OLD_SYS)))
            before = b.get()
            self.assertEqual(b.where()['unconfirmed'], '1')
            self.assertEqual(b.nv('confirm')[0], 0)
            after = b.get()
            self.assertEqual(diff(before, after), [(CORE[s] + 0x10000, CORE[s] + 0x10001)])
            self.assertEqual(after, loader_flash(s))
            self.assertEqual(b.where()['unconfirmed'], '0')
            self.assertEqual(b.nv('confirm')[0], 0)     # nothing to do is not an error
            self.assertEqual(b.get(), after)

    def test_three_starts_nobody_confirms(self):
        for s in (0, 1):
            b = Box(self, loader_flash(s))
            for _ in range(3):
                self.assertEqual(b.start()[0], 'loader')
            self.assertEqual(b.start(), ('stock', 1 - s))
            self.assertEqual(b.where()['scheme'], 'none')
            # with a confirmation after each it goes on for ever
            b.set(loader_flash(s))
            for _ in range(8):
                self.assertEqual(b.start()[0], 'loader')
                self.assertEqual(b.nv('confirm')[0], 0)

    def test_old(self):
        b = Box(self, old_flash(tries=3))
        self.assertEqual(b.start(), ('old', crc_of(OLD_SYS)))
        before = b.get()
        self.assertEqual(env_vars(before)['v90d_tries'], '2')
        self.assertEqual(b.nv('confirm')[0], 0)
        after = b.get()
        self.assertEqual(env_vars(after)['v90d_tries'], '3')
        self.assertTrue(within(diff(before, after), [(ENV, ENV + 8192)]))
        self.assertEqual(after, old_flash(tries=3))

    def test_power_cut_while_confirming(self):
        for s in (0, 1):
            for torn in (False, True):
                b = Box(self, loader_flash(s))
                b.start()
                self.assertEqual(b.nv('confirm', cut=0, torn=torn)[0], 99)
                self.assertIn(b.start()[0], ('loader', 'stock'))


class ToStock(unittest.TestCase):
    def test_loader(self):
        for s in (0, 1):
            b = Box(self, loader_flash(s))
            before = b.get()
            rc, out = b.nv('to-stock')
            self.assertEqual(rc, 0, out)
            self.assertEqual(diff(before, b.get()), [(CORE[s], CORE[s] + 4)])
            self.assertEqual(b.get()[CORE[s]:CORE[s] + 4], bytes(4))
            gone = b.get()
            self.assertEqual(b.nv('confirm')[0], 1)             # the boot script, on the way down
            self.assertEqual(b.nv('conf-save', b.file('c', b'x'))[0], 1)
            self.assertEqual(b.get(), gone)
            self.assertEqual(b.start(), ('stock', 1 - s))
            # and the stock system can take our file again, or its own
            again = R.stock_upgrade(b.get(), G.parse(release())[0], 1 - s, R.HT802)
            b.set(again)
            self.assertEqual(b.start()[:2], ('loader', s))

    def test_old(self):
        b = Box(self, old_flash(tries=3))
        before = b.get()
        self.assertEqual(b.nv('to-stock')[0], 0)
        self.assertTrue(within(diff(before, b.get()), [(ENV, ENV + 8192)]))
        # the count is not taken away (this U-Boot reads none as 1) and not
        # set to 0 (that is what a last good start reads): a value no branch
        # of the script matches
        self.assertEqual(env_vars(b.get())['v90d_tries'], 'x')
        self.assertEqual(env_vars(b.get())['bootcmd'], 'run v90d_try; gsboot')     # the rest of it stays
        self.assertEqual(b.start(), ('stock', 1))
        # what the boot script does on the way down must not undo it
        self.assertEqual(b.nv('confirm')[0], 1)
        self.assertEqual(b.nv('conf-save', b.file('c', b'x'))[0], 1)
        self.assertEqual(b.start(), ('stock', 1))
        self.assertEqual(b.where()['scheme'], 'none')

    def test_no_stock_to_go_back_to(self):
        for make in (lambda: loader_flash(0), lambda: loader_flash(1), old_flash):
            f = bytearray(make())
            other = 1 if make().find(b'v90d-loader', CORE[1], CORE[1] + 0x300) < 0 else 0
            f[BASE[other] + 0x300] ^= 0x10                      # the stock base no longer adds up
            b = Box(self, bytes(f))
            rc, out = b.nv('to-stock')
            self.assertEqual(rc, 1)
            self.assertIn('stock', out)
            self.assertEqual(b.get(), bytes(f))
            self.assertIn('damaged', b.where()['stock'])

    def test_both_sets_ours(self):
        # put there by hand: there is no stock firmware left, so no way back
        f = bytearray(loader_flash(0))
        for kind in R.PARTS:
            a, bb = R.PARTS[kind]
            f[bb:bb + R.SIZE[kind]] = f[a:a + R.SIZE[kind]]
        b = Box(self, bytes(f))
        self.assertEqual(b.nv('to-stock')[0], 1)
        self.assertEqual(b.get(), bytes(f))

    def test_power_cut(self):
        for s in (0, 1):
            for torn in (False, True):
                b = Box(self, loader_flash(s))
                self.assertEqual(b.nv('to-stock', cut=0, torn=torn)[0], 99)
                self.assertIn(b.start()[0], ('loader', 'stock'))
        b = Box(self, old_flash())
        for cut in range(0, 40):
            for torn in (False, True):
                b.set(old_flash())
                rc, _ = b.nv('to-stock', cut=cut, torn=torn)
                if rc == 0:
                    break
                self.assertEqual(rc, 99)
                self.assertIn(b.start()[0], ('old', 'stock'))
            else:
                continue
            break
        else:
            self.fail('to-stock never finished')


class Upgrade(unittest.TestCase):
    def allowed(self, s):
        return [(BASE[s], BASE[s] + R.SIZE['base'])]

    def test_image_loader_scheme(self):
        for s in (0, 1):
            b = Box(self, loader_flash(s))
            before = b.get()
            rc, out = b.nv('upgrade', b.file('new.uimage', NEW_SYS))
            self.assertEqual(rc, 0, out)
            after = b.get()
            self.assertTrue(within(diff(before, after), self.allowed(s)))
            self.assertEqual(after[BASE[s] + 0x200:BASE[s] + 0x200 + len(NEW_SYS)], NEW_SYS)
            h0, h1 = (G.FileHeader(x[BASE[s]:BASE[s] + 0x200]) for x in (before, after))
            self.assertEqual((h1.magic, h1.version, h1.counter, h1.image_id, h1.capacity, h1.cpu, h1.model_key, h1.generation),
                             (G.MAGIC, h0.version, h0.counter, 0x0D, 0x300000, h0.cpu, h0.model_key, h0.generation))
            self.assertEqual(R.gsboot_set(after), s)
            self.assertEqual(b.start(), ('loader', s, s, crc_of(NEW_SYS)))
            # the same again: nothing is written
            rc, out = b.nv('upgrade', b.file('new.uimage', NEW_SYS))
            self.assertEqual(rc, 0)
            self.assertIn('already', out)

    def test_firmware_file_loader_scheme(self):
        for s in (0, 1):
            b = Box(self, loader_flash(s))
            before = b.get()
            # a later release calls itself something else; our set must go on naming what its prog names
            rc, out = b.nv('upgrade', b.file('fw.bin', release(NEW_SYS, version='90.0.2.0')))
            self.assertEqual(rc, 0, out)
            after = b.get()
            self.assertTrue(within(diff(before, after), self.allowed(s)))
            self.assertEqual(G.FileHeader(after[BASE[s]:BASE[s] + 0x200]).version, G.version_int('90.0.1.0'))
            self.assertEqual(R.gsboot_set(after), s)
            self.assertEqual(b.start(), ('loader', s, s, crc_of(NEW_SYS)))

    def test_old_scheme(self):
        for data in (NEW_SYS, release(NEW_SYS)):
            b = Box(self, old_flash())
            before = b.get()
            rc, out = b.nv('upgrade', b.file('new', data))
            self.assertEqual(rc, 0, out)
            after = b.get()
            self.assertTrue(within(diff(before, after), self.allowed(0)))
            self.assertEqual(after[BASE[0]:BASE[0] + len(NEW_SYS)], NEW_SYS)
            self.assertEqual(b.start(), ('old', crc_of(NEW_SYS)))

    def test_a_larger_and_a_smaller_image(self):
        big = system(7, 250001)
        b = Box(self, loader_flash(1))
        for img in (big, NEW_SYS, big, OLD_SYS):
            self.assertEqual(b.nv('upgrade', b.file('i', img))[0], 0)
            self.assertEqual(b.start(), ('loader', 1, 1, crc_of(img)))
            self.assertEqual(b.nv('confirm')[0], 0)

    def test_refused(self):
        def bad_crc(u):
            u = bytearray(u); u[100] ^= 1; return bytes(u)
        stock_file = G.build([('core', b'k' * 6000), ('base', b'b' * 6000), ('prog', b'p' * 6000)], model='ht802',
                             version='1.0.67.1', cpu='dvf9918', when=WHEN)
        broken = bytearray(release(NEW_SYS)); broken[len(broken) - 5000] ^= 0x40
        cases = {
            'data crc': bad_crc(NEW_SYS),
            'header crc': NEW_SYS[:40] + b'x' + NEW_SYS[41:],
            'cut short': NEW_SYS[:-1],
            'no board': system(2, 21000, name='v90d'),
            'other board': system(2, 21000, name='v90d ht801 0.1.0'),
            'other program': system(2, 21000, name='linux ht802 0.1.0'),
            'the loader': LOADER,
            'load address': system(2, 21000, load=0xC8800000),
            'too large': system(2, 0x300000 - 0x200 - 64 + 1),
            'stock firmware': stock_file,
            'other model': release(NEW_SYS, model='ht801'),
            'other cpu': release(NEW_SYS, cpu='dvf9919'),
            'damaged file': bytes(broken),
            'half a file': release(NEW_SYS)[:9000],
            'rubbish': bytes(range(256)) * 40,
            'empty': b'',
        }
        for make in (lambda: loader_flash(0), lambda: loader_flash(1), old_flash):
            b = Box(self, make())
            self.addCleanup(lambda p=b.path: os.path.exists(p) and os.remove(p))
            for name, data in cases.items():
                rc, out = b.nv('upgrade', b.file('x', data))
                self.assertEqual(rc, 1, (name, out))
                self.assertEqual(b.get(), make(), name)
                self.assertEqual(b.nv('check', b.file('x', data))[0], 1, name)
            self.assertEqual(b.nv('check', b.file('x', NEW_SYS))[0], 0)
            self.assertEqual(b.nv('check', b.file('x', release(NEW_SYS)))[0], 0)
            self.assertEqual(b.get(), make())

    def test_one_port_board_is_known_but_not_tried(self):
        for pn in (b'9611003610A\0', b'9610003611C\0'):
            f = bytearray(loader_flash(0))
            f[0x4020:0x4020 + 12] = pn
            w = Box(self, bytes(f)).where()
            self.assertEqual(w['board'], pn[:-1].decode() + ' known')
            self.assertEqual(w['tried'], 'no')

    def test_refused_on_another_board(self):
        f = bytearray(loader_flash(0))
        f[0x4020:0x4020 + 12] = b'9610003703A\0'
        b = Box(self, bytes(f))
        rc, out = b.nv('upgrade', b.file('x', NEW_SYS))
        self.assertEqual(rc, 1)
        self.assertIn('board', out)
        self.assertEqual(b.get(), bytes(f))
        self.assertIn('not known', b.where()['board'])
        self.assertEqual(b.where()['tried'], 'no')

    def test_refused_while_the_set_is_half_installed(self):
        # power was lost while the stock upgrade wrote our file: core is
        # ours, base still holds the bench image, the rest is the old stock copy
        c = G.parse(release())[0]
        f = bytearray(R.stock_upgrade(old_flash(), c, 1, R.HT802, stop_after=1))
        b = Box(self, bytes(f))
        w = b.where()
        self.assertEqual((w['scheme'], w['set']), ('loader', 'A'))
        rc, out = b.nv('upgrade', b.file('x', NEW_SYS))
        self.assertEqual(rc, 1, out)
        self.assertEqual(b.get(), bytes(f))
        # it still starts the old way, and saying so keeps it that way
        self.assertEqual(b.start()[0], 'old')
        self.assertEqual(b.nv('confirm')[0], 0)
        self.assertEqual(env_vars(b.get())['v90d_tries'], '3')
        # back to stock from here takes both: the script disarmed, the loader's signature gone
        self.assertEqual(b.nv('to-stock')[0], 0)
        self.assertEqual(b.nv('confirm')[0], 1)
        self.assertEqual(b.start(), ('stock', 1))
        self.assertEqual(b.start(), ('stock', 1))

    def power_cuts(self, make, cmd_file, old_ok, new_ok, torn):
        """Cut the write before every step; after each cut the box must
        start the old image, the new one, or the stock system, never
        anything else, and once it has started the new one it stays so."""
        b = Box(self, make())
        path = b.file('new', cmd_file)
        seen, steps = [], None
        for cut in range(0, 100000):
            b.set(make())
            rc, out = b.nv('upgrade', path, cut=cut, torn=torn)
            if rc == 0:
                steps = cut
                break
            self.assertEqual(rc, 99, out)
            cutflash = b.get()
            got = b.start()                             # raises if the boot loader would run wild
            kind = 'old' if got == old_ok else 'new' if got == new_ok else 'stock' if got[0] == 'stock' else None
            self.assertIsNotNone(kind, (cut, got))
            seen.append(kind)
            # whatever the cut left: the running system can simply write it again
            if cut % 7 == 0:
                b.set(cutflash)
                rc, out = b.nv('upgrade', path)
                self.assertEqual(rc, 0, (cut, out))
                self.assertEqual(b.start(), new_ok, cut)
        self.assertIsNotNone(steps)
        self.assertEqual(b.start(), new_ok)
        # the old one, then for a long while the stock system, then the new one
        # (half a step at the very first one already spoils the old signature)
        self.assertRegex(''.join(k[0] for k in seen), '^o*s+n*$' if torn else '^o+s+n*$')
        return steps, seen

    def test_power_cut_at_every_step(self):
        for torn in (False, True):
            for s in (0, 1):
                steps, seen = self.power_cuts(lambda: loader_flash(s), NEW_SYS, ('loader', s, s, crc_of(OLD_SYS)),
                                              ('loader', s, s, crc_of(NEW_SYS)), torn)
                self.assertGreater(steps, 80)
                self.assertGreater(seen.count('stock'), 60)
            steps, seen = self.power_cuts(old_flash, NEW_SYS, ('old', crc_of(OLD_SYS)), ('old', crc_of(NEW_SYS)), torn)
            self.assertGreater(seen.count('stock'), 60)
        self.power_cuts(lambda: loader_flash(0), release(NEW_SYS), ('loader', 0, 0, crc_of(OLD_SYS)),
                        ('loader', 0, 0, crc_of(NEW_SYS)), True)

    def test_full_size_image_sampled_cuts(self):
        big_old, big_new = system(3, 2182334), system(4, 2190000)
        make = lambda: loader_flash(0, big_old)
        b = Box(self, make())
        path = b.file('new', big_new)
        rc, out = b.nv('upgrade', path)
        self.assertEqual(rc, 0, out)
        self.assertEqual(b.start(), ('loader', 0, 0, crc_of(big_new)))
        # how many steps the whole write is: the first cut it no longer feels
        lo, hi = 0, 20000
        while lo < hi:
            mid = (lo + hi) // 2
            b.set(make())
            if b.nv('upgrade', path, cut=mid)[0] == 0:
                hi = mid
            else:
                lo = mid + 1
        steps = lo
        self.assertGreater(steps, 8000)                         # some 8500 pages and 530 sectors
        outcomes = []
        for cut in [0, 1, 2, 3, 300, 536, 537, 538, 4000] + list(range(steps - 6, steps + 1)):
            for torn in (False, True):
                b.set(make())
                rc, _ = b.nv('upgrade', path, cut=cut, torn=torn)
                self.assertEqual(rc, 0 if cut >= steps else 99)
                got = b.start()
                outcomes.append(got)
                if rc == 0:
                    self.assertEqual(got, ('loader', 0, 0, crc_of(big_new)))
                elif cut == 0 and not torn:
                    self.assertEqual(got, ('loader', 0, 0, crc_of(big_old)))
                else:
                    self.assertEqual(got, ('stock', 1), (cut, torn))


class Settings(unittest.TestCase):
    def test_round_trip_and_place(self):
        conf = b'number 42\nuser a b\n'
        for make, at in ((lambda: loader_flash(0), CORE[0] + 0x20000), (lambda: loader_flash(1), CORE[1] + 0x20000), (old_flash, 0x060000)):
            b = Box(self, make())
            out = os.path.join(b.dir, 'out')
            if make is old_flash:
                self.assertEqual(b.nv('conf-load', out)[0], 0)
                self.assertEqual(rd(out), b'number 5551234\n')
            else:
                self.assertEqual(b.nv('conf-load', out)[0], 1)
            before = b.get()
            self.assertEqual(b.nv('conf-save', b.file('c', conf))[0], 0)
            after = b.get()
            self.assertTrue(within(diff(before, after), [(at, at + 0x10000)]))
            self.assertEqual(after[at:at + 12 + len(conf)], blob(conf))
            self.assertEqual(b.nv('conf-load', out)[0], 0)
            self.assertEqual(rd(out), conf)
            self.assertEqual(b.nv('conf-save', b.file('c', conf))[0], 0)    # unchanged: not written again
            self.assertEqual(b.get(), after)
            self.assertEqual(b.nv('conf-save', b.file('big', bytes(70000)))[0], 2)
            self.assertEqual(b.get(), after)
            # the box still starts what it started before
            self.assertEqual(b.start()[0], 'old' if make is old_flash else 'loader')

    def test_power_cut(self):
        a, c = b'number 1\n' * 50, b'number 2\n' * 700
        for make in (lambda: loader_flash(1), old_flash):
            b = Box(self, make())
            self.assertEqual(b.nv('conf-save', b.file('a', a))[0], 0)
            start = b.get()
            out = os.path.join(b.dir, 'out')
            for torn in (False, True):
                for cut in range(0, 200):
                    b.set(start)
                    rc, _ = b.nv('conf-save', b.file('c', c), cut=cut, torn=torn)
                    lrc, _ = b.nv('conf-load', out)
                    if lrc == 0:
                        self.assertIn(rd(out), (a, c), cut)
                    else:
                        self.assertEqual(lrc, 1)
                    self.assertEqual(b.start()[0], 'old' if make is old_flash else 'loader')
                    if rc == 0:
                        self.assertEqual(rd(out), c)
                        break
                    self.assertEqual(rc, 99)
                else:
                    self.fail('conf-save never finished')

    def test_a_stock_install_into_our_set_takes_them_along(self):
        # written down because it is a limit of the design, not a wish
        b = Box(self, loader_flash(0))
        self.assertEqual(b.nv('conf-save', b.file('c', b'x=1\n'))[0], 0)
        self.assertEqual(b.nv('to-stock')[0], 0)
        b.set(R.stock_upgrade(b.get(), G.parse(release())[0], 1, R.HT802))
        self.assertEqual(b.nv('conf-load', os.path.join(b.dir, 'out'))[0], 1)


class Web(unittest.TestCase):
    """serve-upgrade and serve-stock: v90nv with the browser's connection
    as its standard input and output, as the daemon starts it."""
    def serve(self, b, what, body, ctype=None, early=b'', length=None):
        a, c = socket.socketpair()
        r, w = os.pipe()
        os.write(w, early); os.close(w)
        note = os.path.join(b.dir, 'restart')
        state = os.path.join(b.dir, 'state')
        args = [what, str(len(body) if length is None else length)] + ([ctype] if ctype else [])
        env = dict(os.environ, V90NV_RESTART_NOTE=note, V90NV_STATE=state, V90NV_EARLY_FD=str(r))
        p = subprocess.Popen(NV + ['-f', b.path] + args, stdin=c.fileno(), stdout=c.fileno(), env=env, pass_fds=(r,))
        c.close(); os.close(r)
        try:
            a.sendall(body[len(early):])
        except BrokenPipeError:
            pass
        a.shutdown(socket.SHUT_WR)
        resp = b''
        while True:
            d = a.recv(65536)
            if not d:
                break
            resp += d
        a.close()
        rc = p.wait()
        st = rd(state).decode() if os.path.exists(state) else ''
        return rc, resp.decode('latin-1'), os.path.exists(note), st

    def multipart(self, data, boundary='----x7MA4YWxkTrZu0gW'):
        body = ('--%s\r\nContent-Disposition: form-data; name="image"; filename="v90d.bin"\r\n'
                'Content-Type: application/octet-stream\r\n\r\n' % boundary).encode() + data + ('\r\n--%s--\r\n' % boundary).encode()
        return body, 'multipart/form-data; boundary=' + boundary

    def test_upload(self):
        for s in (0, 1):
            for data in (NEW_SYS, release(NEW_SYS)):
                for form in (True, False):
                    b = Box(self, loader_flash(s))
                    body, ctype = self.multipart(data) if form else (data, 'application/octet-stream')
                    rc, resp, note, st = self.serve(b, 'serve-upgrade', body, ctype, early=body[:700])
                    self.assertEqual(rc, 0, resp)
                    self.assertTrue(resp.startswith('HTTP/1.0 200 '), resp[:200])
                    self.assertIn('v90d ht802 0.1.0', resp)
                    self.assertTrue(note)
                    self.assertIn('written', st)
                    self.assertEqual(b.start(), ('loader', s, s, crc_of(NEW_SYS)))

    def test_upload_refused(self):
        b = Box(self, loader_flash(0))
        bad = bytearray(NEW_SYS); bad[500] ^= 1
        for body, ctype, length in ((bytes(bad), None, None), (self.multipart(bytes(bad))[0], self.multipart(b'')[1], None),
                                    (self.multipart(NEW_SYS)[0], 'multipart/form-data', None),      # no boundary
                                    (NEW_SYS[:5000], None, len(NEW_SYS)),                           # the browser went away
                                    (b'', None, 0), (NEW_SYS, None, 0x400001)):
            rc, resp, note, st = self.serve(b, 'serve-upgrade', body, ctype, length=length)
            self.assertEqual(rc, 1, resp)
            if length is None or length == 0x400001:
                self.assertTrue(resp.startswith('HTTP/1.0 400 ') or resp.startswith('HTTP/1.0 413 '), resp[:100])
            self.assertFalse(note)
            self.assertEqual(b.get(), loader_flash(0))

    def test_back_to_stock(self):
        b = Box(self, loader_flash(0))
        rc, resp, note, st = self.serve(b, 'serve-stock', b'confirm=no')
        self.assertEqual(rc, 1)
        self.assertTrue(resp.startswith('HTTP/1.0 400 '))
        self.assertEqual(b.get(), loader_flash(0))
        rc, resp, note, st = self.serve(b, 'serve-stock', b'')
        self.assertEqual((rc, note), (1, False))
        self.assertEqual(b.get(), loader_flash(0))
        rc, resp, note, st = self.serve(b, 'serve-stock', b'confirm=stock')
        self.assertEqual(rc, 0, resp)
        self.assertTrue(resp.startswith('HTTP/1.0 200 '))
        self.assertTrue(note)
        self.assertEqual(b.start(), ('stock', 1))

    def test_back_to_stock_refused_without_stock(self):
        f = bytearray(loader_flash(0)); f[BASE[1] + 0x300] ^= 1
        b = Box(self, bytes(f))
        rc, resp, note, st = self.serve(b, 'serve-stock', b'confirm=stock')
        self.assertEqual(rc, 1)
        self.assertTrue(resp.startswith('HTTP/1.0 409 '), resp[:100])
        self.assertFalse(note)
        self.assertEqual(b.get(), bytes(f))


@unittest.skipUnless(os.path.exists(BACKUP) and SCRATCH, 'needs the flash backup and V90NV_SCRATCH (a directory for a copy of it)')
class RealUnit(unittest.TestCase):
    """A copy of the backup of a real unit (never the backup itself)."""
    def setUp(self):
        os.makedirs(SCRATCH, exist_ok=True)
        self.copy = os.path.join(SCRATCH, 'flash_copy.bin')
        shutil.copyfile(BACKUP, self.copy)
        self.addCleanup(os.remove, self.copy)
        with open(self.copy, 'rb') as f:
            self.stock = f.read()

    def test_install_run_upgrade_and_back(self):
        b = Box(self, self.stock)
        w = b.where()
        self.assertEqual((w['scheme'], w['board']), ('none', '9610003725A known'))
        ours = R.stock_upgrade(self.stock, G.parse(release())[0], 1, R.HT802)
        b.set(ours)
        w = b.where()
        self.assertEqual((w['scheme'], w['set'], w['stock']), ('loader', 'A', 'B good'))
        self.assertEqual(b.start(), ('loader', 0, 0, crc_of(OLD_SYS)))
        self.assertEqual(b.nv('confirm')[0], 0)
        self.assertEqual(b.nv('conf-save', b.file('c', b'number 1\n'))[0], 0)
        self.assertEqual(b.nv('upgrade', b.file('n', NEW_SYS))[0], 0)
        self.assertEqual(b.start(), ('loader', 0, 0, crc_of(NEW_SYS)))
        self.assertEqual(b.nv('to-stock')[0], 0)
        after = b.get()
        self.assertEqual(b.start(), ('stock', 1))
        # nothing of the unit outside our set was written, at any point
        self.assertTrue(within(diff(self.stock, after), [(CORE[0], CORE[0] + 0x140000), (BASE[0], BASE[0] + 0x300000),
                                                           (PROG[0], PROG[0] + 0x33F000)]))


if __name__ == '__main__':
    if len(sys.argv) == 4 and sys.argv[1] == '--make-flash':
        # for other tests (srv/test/ns_call.c): a box started by our loader
        # from set A, as a file, and a new system image to give it
        with open(sys.argv[2], 'wb') as f:
            f.write(loader_flash(0))
        with open(sys.argv[3], 'wb') as f:
            f.write(NEW_SYS)
        sys.exit(0)
    if not os.path.exists(DECIDE):
        subprocess.run(['make', '-s', '-C', os.path.join(TOP, 'fw', 'loader'), 'build/flash_decide'], check=True)
    unittest.main(verbosity=1)
