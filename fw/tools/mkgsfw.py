#!/usr/bin/env python3
"""The firmware file format of the stock HT802 upgrade page: build such a
file from our own images, list one, and check one the way the box's own
upgrader does (HARDWARE.md, "Firmware image format" and "How the stock
upgrade works").

    mkgsfw.py list FILE
    mkgsfw.py verify FILE [--model ht802] [--cpu dvf9918] [--generation 1]
                          [--required-bits 0000:0000:0000:0000] [--model-key K] [--quick]
    mkgsfw.py build -o OUT --version A.B.C.D --core F --base F --prog F
                          [--model ht802|ht801] [--cpu dvf9918] [--bits B] [--model-key K]
    mkgsfw.py extract FILE DIR          every file as it would stand in flash
    mkgsfw.py part -o OUT --kind core|base|prog --version A.B.C.D [--counter N] IN
    mkgsfw.py uimage -o OUT --name NAME --load ADDR IN

`build` wraps each of our files in the 512-octet partition header, encrypts
them and puts the 648-octet directory in front. `part` writes one partition
as it stands in flash (header and payload, not encrypted): what our own
system writes when it replaces its image. `uimage` puts a legacy U-Boot
header on a program (the loader).

Written for the v90d project from the format as we established it; Python 3
standard library only. The AES is ours (FIPS-197), because the standard
library has none and the format needs nothing else: 128-bit keys (256 also
works), two blocks chained at a time.
"""
import argparse, collections, os, struct, sys, time, zlib

MAGIC = 0x23C97AF9
DIR_LEN = 0x288                 # the directory in front: 7 slots
SLOTS = 7
HDR_LEN = 0x200                 # in front of every file, and of every partition in flash

# What the stock firmware decrypts with when the unit has no key of its own
# (/proc/gxp/dev_info/hw_features/default_fwkey says "nokey", as on every
# HT802 we know of). It is in the stock upgrader as this text; it is here
# because a file the stock upgrade page accepts cannot be made without it,
# and once more in tools/nvimage.c, because the running system opens the
# same file when it is given to our own Firmware page. It protects nothing:
# the format has no signature.
STOCK_KEY_TEXT = '37d6ae8bc920374649426438bde35493'
CBC_IV = b'Grandstream Inc.'    # the same for every 32-octet unit

# kind -> (image type id in the header, octets of the flash partition)
KINDS = {'boot': (0x0B, 256 * 1024), 'core': (0x0C, 1280 * 1024), 'base': (0x0D, 3072 * 1024), 'prog': (0x0E, 3324 * 1024)}
STOCK_MODEL_KEY = 0xFD21        # header offset 0x1A on every HT802 file and partition seen
STOCK_BITS = (0, 0, 0, 0x7FFF)  # hardware bits of the 1.0.65 files: every board they know
# model -> (model key, hardware bits), as in each model's stock files
MODELS = {'ht802': (0xFD21, (0, 0, 0, 0x7FFF)),
          'ht801': (0xFD20, (0, 0, 0, 0xFFFF))}


def model_key_of(model):
    return MODELS.get(model.lower(), (STOCK_MODEL_KEY, STOCK_BITS))[0]


# ---------------------------------------------------------------- AES (FIPS-197)
def _aes_tables():
    xt = lambda x: ((x << 1) ^ 0x11B) & 0xFF if x & 0x80 else x << 1
    exp, log, x = [0] * 255, [0] * 256, 1
    for i in range(255):
        exp[i], log[x] = x, i
        x ^= xt(x)                              # times 3, a generator of the field
    mul = lambda a, b: exp[(log[a] + log[b]) % 255] if a and b else 0
    rol = lambda v, n: ((v << n) | (v >> (8 - n))) & 0xFF
    sbox = []
    for v in range(256):
        i = exp[(255 - log[v]) % 255] if v else 0
        sbox.append(i ^ rol(i, 1) ^ rol(i, 2) ^ rol(i, 3) ^ rol(i, 4) ^ 0x63)
    inv = [0] * 256
    for v, s in enumerate(sbox):
        inv[s] = v
    ror = lambda w, n: ((w >> n) | (w << (32 - n))) & 0xFFFFFFFF
    te0 = [(mul(s, 2) << 24) | (s << 16) | (s << 8) | mul(s, 3) for s in sbox]
    td0 = [(mul(s, 14) << 24) | (mul(s, 9) << 16) | (mul(s, 13) << 8) | mul(s, 11) for s in inv]
    te = [te0] + [[ror(w, n) for w in te0] for n in (8, 16, 24)]
    td = [td0] + [[ror(w, n) for w in td0] for n in (8, 16, 24)]
    return sbox, inv, te, td

_SBOX, _INV, _TE, _TD = _aes_tables()


class AES:
    """One key; whole blocks in and out."""
    def __init__(self, key):
        if len(key) not in (16, 32):
            raise ValueError('AES key of %d octets' % len(key))
        nk = len(key) // 4
        self.rounds = nk + 6
        w = list(struct.unpack('>%dI' % nk, key))
        sub = lambda v: (_SBOX[v >> 24] << 24) | (_SBOX[(v >> 16) & 255] << 16) | (_SBOX[(v >> 8) & 255] << 8) | _SBOX[v & 255]
        rcon = 1
        for i in range(nk, 4 * (self.rounds + 1)):
            t = w[i - 1]
            if i % nk == 0:
                t = sub(((t << 8) | (t >> 24)) & 0xFFFFFFFF) ^ (rcon << 24)
                rcon = ((rcon << 1) ^ 0x11B) & 0xFF if rcon & 0x80 else rcon << 1
            elif nk > 6 and i % nk == 4:
                t = sub(t)
            w.append(w[i - nk] ^ t)
        self.ek = w
        # the equivalent inverse cipher: round keys backwards, the inner
        # ones through the inverse column mix
        d = []
        for r in range(self.rounds, -1, -1):
            for v in w[4 * r:4 * r + 4]:
                if 0 < r < self.rounds:
                    v = (_TD[0][_SBOX[v >> 24]] ^ _TD[1][_SBOX[(v >> 16) & 255]] ^
                         _TD[2][_SBOX[(v >> 8) & 255]] ^ _TD[3][_SBOX[v & 255]])
                d.append(v)
        self.dk = d

    def encrypt_words(self, s0, s1, s2, s3):
        k = self.ek
        t0, t1, t2, t3 = _TE
        s0 ^= k[0]; s1 ^= k[1]; s2 ^= k[2]; s3 ^= k[3]
        i = 4
        for _ in range(self.rounds - 1):
            a = t0[s0 >> 24] ^ t1[(s1 >> 16) & 255] ^ t2[(s2 >> 8) & 255] ^ t3[s3 & 255] ^ k[i]
            b = t0[s1 >> 24] ^ t1[(s2 >> 16) & 255] ^ t2[(s3 >> 8) & 255] ^ t3[s0 & 255] ^ k[i + 1]
            c = t0[s2 >> 24] ^ t1[(s3 >> 16) & 255] ^ t2[(s0 >> 8) & 255] ^ t3[s1 & 255] ^ k[i + 2]
            d = t0[s3 >> 24] ^ t1[(s0 >> 16) & 255] ^ t2[(s1 >> 8) & 255] ^ t3[s2 & 255] ^ k[i + 3]
            s0, s1, s2, s3 = a, b, c, d
            i += 4
        S = _SBOX
        return (((S[s0 >> 24] << 24) | (S[(s1 >> 16) & 255] << 16) | (S[(s2 >> 8) & 255] << 8) | S[s3 & 255]) ^ k[i],
                ((S[s1 >> 24] << 24) | (S[(s2 >> 16) & 255] << 16) | (S[(s3 >> 8) & 255] << 8) | S[s0 & 255]) ^ k[i + 1],
                ((S[s2 >> 24] << 24) | (S[(s3 >> 16) & 255] << 16) | (S[(s0 >> 8) & 255] << 8) | S[s1 & 255]) ^ k[i + 2],
                ((S[s3 >> 24] << 24) | (S[(s0 >> 16) & 255] << 16) | (S[(s1 >> 8) & 255] << 8) | S[s2 & 255]) ^ k[i + 3])

    def decrypt_words(self, s0, s1, s2, s3):
        k = self.dk
        t0, t1, t2, t3 = _TD
        s0 ^= k[0]; s1 ^= k[1]; s2 ^= k[2]; s3 ^= k[3]
        i = 4
        for _ in range(self.rounds - 1):
            a = t0[s0 >> 24] ^ t1[(s3 >> 16) & 255] ^ t2[(s2 >> 8) & 255] ^ t3[s1 & 255] ^ k[i]
            b = t0[s1 >> 24] ^ t1[(s0 >> 16) & 255] ^ t2[(s3 >> 8) & 255] ^ t3[s2 & 255] ^ k[i + 1]
            c = t0[s2 >> 24] ^ t1[(s1 >> 16) & 255] ^ t2[(s0 >> 8) & 255] ^ t3[s3 & 255] ^ k[i + 2]
            d = t0[s3 >> 24] ^ t1[(s2 >> 16) & 255] ^ t2[(s1 >> 8) & 255] ^ t3[s0 & 255] ^ k[i + 3]
            s0, s1, s2, s3 = a, b, c, d
            i += 4
        S = _INV
        return (((S[s0 >> 24] << 24) | (S[(s3 >> 16) & 255] << 16) | (S[(s2 >> 8) & 255] << 8) | S[s1 & 255]) ^ k[i],
                ((S[s1 >> 24] << 24) | (S[(s0 >> 16) & 255] << 16) | (S[(s3 >> 8) & 255] << 8) | S[s2 & 255]) ^ k[i + 1],
                ((S[s2 >> 24] << 24) | (S[(s1 >> 16) & 255] << 16) | (S[(s0 >> 8) & 255] << 8) | S[s3 & 255]) ^ k[i + 2],
                ((S[s3 >> 24] << 24) | (S[(s2 >> 16) & 255] << 16) | (S[(s1 >> 8) & 255] << 8) | S[s0 & 255]) ^ k[i + 3])

    def encrypt_block(self, b):
        return struct.pack('>4I', *self.encrypt_words(*struct.unpack('>4I', b)))

    def decrypt_block(self, b):
        return struct.pack('>4I', *self.decrypt_words(*struct.unpack('>4I', b)))


def cbc_units(key, data, decrypt, iv=CBC_IV):
    """The format's way with CBC: the chain starts again, from the same
    vector, every 32 octets (two blocks). A last unit of one block is taken
    as it comes; anything else is not a whole number of blocks."""
    if len(data) % 16:
        raise ValueError('%d octets is not a whole number of AES blocks' % len(data))
    a = AES(key)
    n = len(data) // 4
    w = struct.unpack('>%dI' % n, data)
    v = struct.unpack('>4I', iv)
    out = [0] * n
    if decrypt:
        dec = a.decrypt_words
        for i in range(0, n, 4):
            p = dec(w[i], w[i + 1], w[i + 2], w[i + 3])
            c = v if i % 8 == 0 else w[i - 4:i]
            out[i], out[i + 1], out[i + 2], out[i + 3] = p[0] ^ c[0], p[1] ^ c[1], p[2] ^ c[2], p[3] ^ c[3]
    else:
        enc = a.encrypt_words
        c = v
        for i in range(0, n, 4):
            if i % 8 == 0:
                c = v
            c = enc(w[i] ^ c[0], w[i + 1] ^ c[1], w[i + 2] ^ c[2], w[i + 3] ^ c[3])
            out[i], out[i + 1], out[i + 2], out[i + 3] = c
    return struct.pack('>%dI' % n, *out)


# ---------------------------------------------------------------- the format
def header_key(text=STOCK_KEY_TEXT):
    """The 16 key octets the stock upgrader makes of the 32 characters. It
    reads them four at a time as two little-endian 16-bit units, turns each
    unit's two characters into one number (the later character is the high
    digit) with a hex conversion that only knows digits and capitals, so
    'a'..'f' come out as 0x2A..0x2F, and adds the two numbers up in 16
    bits, the first as the high octet."""
    t = text.encode('ascii')
    if len(t) != 32:
        raise ValueError('key text must be 32 characters')
    digit = lambda c: c - 0x30 if 0x30 <= c <= 0x39 else c - 0x37
    key = bytearray()
    for i in range(0, 32, 4):
        hi = digit(t[i + 1]) * 16 + digit(t[i])
        lo = digit(t[i + 3]) * 16 + digit(t[i + 2])
        w = (hi * 0x100 + lo) & 0xFFFF
        key += bytes((w >> 8, w & 0xFF))
    return bytes(key)


def body_key(header):
    """The body's key: octets 16..31 of the decrypted header, each pair swapped."""
    k = bytearray(header[16:32])
    k[0::2], k[1::2] = k[1::2], k[0::2]
    return bytes(k)


def checksum(payload):
    """The header's 16-bit value: what makes the payload's little-endian
    16-bit words add up to zero with it."""
    if len(payload) % 2:
        raise ValueError('payload of odd length')
    s = sum(struct.unpack('<%dH' % (len(payload) // 2), payload)) & 0xFFFF
    return (0x10000 - s) & 0xFFFF


def version_int(text):
    p = [int(x) for x in text.split('.')]
    if len(p) != 4 or any(not 0 <= x <= 255 for x in p):
        raise ValueError('version must be A.B.C.D, each 0..255: ' + text)
    return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]


def version_text(v):
    return '%d.%d.%d.%d' % (v >> 24, (v >> 16) & 255, (v >> 8) & 255, v & 255)


def bits_tuple(text):
    b = tuple(int(x, 16) for x in text.split(':'))
    if len(b) != 4 or any(not 0 <= x <= 0xFFFF for x in b):
        raise ValueError('hardware bits must be four 16-bit hex words, a:b:c:d')
    return b


class FileHeader:
    """A decrypted file, or a partition as it stands in flash."""
    def __init__(self, raw):
        if len(raw) < HDR_LEN:
            raise ValueError('shorter than a header')
        self.raw = raw
        (self.magic, self.version, self.capacity, self.size, self.image_id, self.checksum,
         self.year, self.day, self.month, self.minute, self.hour, self.model_key, self.mask) = struct.unpack_from('<4I3H4B2H', raw, 0)
        self.bits = struct.unpack_from('<4H', raw, 0x1E)
        self.compatible = struct.unpack_from('<2I', raw, 0x28)
        self.counter, = struct.unpack_from('<I', raw, 0x30)
        self.cpu = raw[0x34:0x44].split(b'\0')[0].decode('ascii', 'replace')
        self.generation, = struct.unpack_from('<H', raw, 0x54)

    @property
    def payload(self):
        return self.raw[HDR_LEN:HDR_LEN + self.size]


def make_header(kind, payload, version, cpu, compatible, bits, mask, generation, model_key, counter, when):
    image_id, room = KINDS[kind]
    if len(payload) % 2:
        raise ValueError('payload of odd length')
    if HDR_LEN + len(payload) > room:
        raise ValueError('%s: %d octets with the header, the partition holds %d' % (kind, HDR_LEN + len(payload), room))
    year, month, day, hour, minute = when
    h = bytearray(HDR_LEN)
    struct.pack_into('<4I3H4B2H', h, 0, MAGIC, version, room, len(payload), image_id, checksum(payload),
                     year, day, month, minute, hour, model_key, mask)
    struct.pack_into('<4H', h, 0x1E, *bits)
    struct.pack_into('<2I', h, 0x28, *compatible)
    struct.pack_into('<I', h, 0x30, counter)
    c = cpu.encode('ascii')
    if len(c) > 15:
        raise ValueError('cpu name too long')
    h[0x34:0x34 + len(c)] = c
    struct.pack_into('<H', h, 0x54, generation)
    return bytes(h)


def _when(when):
    if when is not None:
        return when
    t = time.gmtime(int(os.environ.get('SOURCE_DATE_EPOCH', time.time())))
    return (t.tm_year, t.tm_mon, t.tm_mday, t.tm_hour, t.tm_min)


def _padded(payload):
    return payload + bytes(-len(payload) % 32)


def partition_image(kind, payload, version, cpu, counter=0, compatible=None, bits=STOCK_BITS, mask=1,
                    generation=1, model_key=STOCK_MODEL_KEY, when=None):
    """Header and payload as they stand in flash."""
    v = version_int(version)
    payload = _padded(payload)
    return make_header(kind, payload, v, cpu, compatible or (v, v), bits, mask, generation, model_key, counter, _when(when)) + payload


class File:
    """One slot of a container: still encrypted."""
    def __init__(self, name, version, raw):
        self.name, self.version, self.raw = name, version, raw
        self.size = len(raw)
        self._open = {}

    def open(self, key_text=STOCK_KEY_TEXT, body=True):
        k = (key_text, body)
        if k not in self._open:
            head = cbc_units(header_key(key_text), self.raw[:32], True)
            if struct.unpack_from('<I', head, 0)[0] != MAGIC:
                raise ValueError('%s: no header under this key' % self.name)
            rest = self.raw[32:HDR_LEN]
            data = cbc_units(body_key(head), self.raw[HDR_LEN:], True) if body else b''
            self._open[k] = FileHeader(head + rest + data)
        return self._open[k]


class Container:
    def __init__(self, raw):
        self.raw = raw
        self.magic, = struct.unpack_from('<I', raw, 0)
        names = [raw[4 + 64 * i:68 + 64 * i].split(b'\0')[0].decode('ascii', 'replace') for i in range(SLOTS)]
        sizes = struct.unpack_from('<7I', raw, 0x1C4)
        versions = struct.unpack_from('<7I', raw, 0x1E0)
        self.bits = struct.unpack_from('<4H', raw, 0x1FC)
        self.mask, self.generation = struct.unpack_from('<2H', raw, 0x204)
        self.slot_names = names
        self.files, at = [], DIR_LEN
        for n, s, v in zip(names, sizes, versions):
            if s:
                self.files.append(File(n, v, raw[at:at + s]))
            at += s


def parse(data):
    """The containers of a firmware file, in order. A stock file may hold
    several, one per flash layout generation."""
    out, at = [], 0
    while at < len(data):
        if len(data) - at < DIR_LEN or struct.unpack_from('<I', data, at)[0] != MAGIC:
            raise ValueError('no container at offset 0x%X' % at)
        total = DIR_LEN + sum(struct.unpack_from('<7I', data, at + 0x1C4))
        if at + total > len(data):
            raise ValueError('container at 0x%X says %d octets, the file has %d left' % (at, total, len(data) - at))
        out.append(Container(data[at:at + total]))
        at += total
    return out


Device = collections.namedtuple('Device', 'model cpu generation required_bits model_key mask',
                                defaults=(STOCK_MODEL_KEY, 1))


def pick(containers, dev):
    """The container the upload page keeps: the first whose first file name
    carries the model and whose generation (0 counts as 1) is the unit's."""
    for c in containers:
        if dev.model.lower() in c.slot_names[0].lower() and (c.generation or 1) == dev.generation:
            return c
    raise ValueError('no container for %s generation %d' % (dev.model, dev.generation))


def wanted(dev):
    """File names the unit's upgrader looks for: kind -> name."""
    pg = '.pg%d' % dev.generation if dev.generation >= 2 else ''
    alt = dev.cpu == 'dvf9919' and not dev.model.lower().startswith('ht81')
    out = {}
    for kind in ('boot', 'core', 'base', 'prog'):
        cpu = '.' + dev.cpu if alt and kind in ('boot', 'core') else ''
        out[kind] = '%s%s%s%s.bin' % (dev.model, kind, cpu, pg)
    return out


def verify(c, dev, bodies=True, key_text=STOCK_KEY_TEXT):
    """What the stock upgrader of firmware 1.0.65 checks before it writes,
    for a unit like `dev`. Returns the reasons it would refuse (empty: it
    would install). Files the unit does not look for are left out, as the
    upgrader leaves them out. Two checks of ours are added and say so."""
    bad = []
    if c.magic != MAGIC:
        return ['container signature']
    for n in c.slot_names:
        if n and dev.model.lower() not in n.lower():
            bad.append('file name %s does not carry the model %s' % (n, dev.model))
    if dev.generation >= 1 and c.generation != dev.generation:
        bad.append('container generation %d, the unit has %d' % (c.generation, dev.generation))
    if any((have & need) != need for have, need in zip(c.bits, dev.required_bits)):
        bad.append('hardware bits %s lack the unit\'s %s' % (':'.join('%04x' % b for b in c.bits), ':'.join('%04x' % b for b in dev.required_bits)))
    if (dev.mask & c.mask) != dev.mask:
        bad.append('container mask 0x%04X lacks the partitions\' 0x%04X' % (c.mask, dev.mask))
    names = wanted(dev)
    for f in c.files:
        if f.name not in names.values():
            continue
        try:
            h = f.open(key_text, body=bodies)
        except ValueError as e:
            bad.append('%s: signature (%s)' % (f.name, e))
            continue
        if h.cpu and dev.cpu and h.cpu != dev.cpu and h.cpu != 'any':
            bad.append('%s: for cpu %s, the unit is %s' % (f.name, h.cpu, dev.cpu))
        if dev.generation >= 1 and (h.generation or 1) != dev.generation:
            bad.append('%s: generation %d, the unit has %d' % (f.name, h.generation, dev.generation))
        if h.version != f.version:
            bad.append('%s: version %s in its header, %s in the container' % (f.name, version_text(h.version), version_text(f.version)))
        if h.size + HDR_LEN != f.size:
            bad.append('%s: size %d in its header, %d in the container' % (f.name, h.size + HDR_LEN, f.size))
        if h.bits != c.bits or h.mask != c.mask:
            bad.append('%s: hardware bits or mask differ from the container (a version mismatch to the upgrader)' % f.name)
        if any((have & need) != need for have, need in zip(h.bits, dev.required_bits)):
            bad.append('%s: hardware bits lack the unit\'s' % f.name)
        if h.model_key and h.model_key != dev.model_key:
            bad.append('%s: model key 0x%04x, the partition has 0x%04x' % (f.name, h.model_key, dev.model_key))
        if (dev.mask & h.mask) != dev.mask:
            bad.append('%s: mask 0x%04X lacks the partition\'s 0x%04X' % (f.name, h.mask, dev.mask))
        if bodies and h.size + HDR_LEN == f.size and checksum(h.payload) != h.checksum:
            bad.append('%s: checksum' % f.name)
        kind = [k for k, n in names.items() if n == f.name][0]
        if dev.generation == 1 and f.size > KINDS[kind][1]:        # the sizes we know are generation 1's
            bad.append('%s: %d octets, too big for its partition of %d (our check; the upgrader has none)' % (f.name, f.size, KINDS[kind][1]))
        if h.image_id != KINDS[kind][0]:
            bad.append('%s: image type 0x%02X in the file for partition type 0x%02X (our check)' % (f.name, h.image_id, KINDS[kind][0]))
    return bad


def build(files, model, version, cpu, bits=None, mask=1, generation=1, model_key=None,
          when=None, key_text=STOCK_KEY_TEXT):
    """A container of our files: [(kind, payload), ...] in the order given.
    All carry the same version, and name it as the version they go with."""
    key0, bits0 = MODELS.get(model.lower(), (STOCK_MODEL_KEY, STOCK_BITS))
    bits = bits0 if bits is None else bits
    model_key = key0 if model_key is None else model_key
    if not 1 <= len(files) <= SLOTS:
        raise ValueError('1 to %d files' % SLOTS)
    v = version_int(version)
    when = _when(when)
    d = bytearray(DIR_LEN)
    struct.pack_into('<I', d, 0, MAGIC)
    struct.pack_into('<4H', d, 0x1FC, *bits)
    struct.pack_into('<2H', d, 0x204, mask, generation)
    body = b''
    for i, (kind, payload) in enumerate(files):
        payload = _padded(payload)
        h = make_header(kind, payload, v, cpu, (v, v), bits, mask, generation, model_key, 0, when)
        enc = cbc_units(header_key(key_text), h[:32], False) + h[32:] + cbc_units(body_key(h), payload, False)
        name = ('%s%s.bin' % (model, kind)).encode('ascii')
        d[4 + 64 * i:4 + 64 * i + len(name)] = name
        struct.pack_into('<I', d, 0x1C4 + 4 * i, len(enc))
        struct.pack_into('<I', d, 0x1E0 + 4 * i, v)
        body += enc
    return bytes(d) + body


def uimage(data, name, load, entry=None, when=None):
    """A legacy U-Boot image header on a program that is started where it
    is loaded: ARM, Linux kernel type (what `bootm` insists on), not compressed."""
    when = int(os.environ.get('SOURCE_DATE_EPOCH', time.time())) if when is None else when
    entry = load if entry is None else entry
    def header(hcrc):
        return struct.pack('>7I4B32s', 0x27051956, hcrc, when, len(data), load, entry, zlib.crc32(data) & 0xFFFFFFFF,
                           5, 2, 2, 0, name.encode('ascii'))
    return header(zlib.crc32(header(0)) & 0xFFFFFFFF) + data


# ---------------------------------------------------------------- command line
def _list(c, n):
    print('container %d: %d octets, generation %d, hardware bits %s, mask 0x%04X' %
          (n, len(c.raw), c.generation, ':'.join('%04x' % b for b in c.bits), c.mask))
    for f in c.files:
        line = '  %-24s %-12s %8d octets' % (f.name, version_text(f.version), f.size)
        try:
            h = f.open(body=False)
            line += '  %04d-%02d-%02d  cpu %-8s goes with core %s base %s' % (
                h.year, h.month, h.day, h.cpu or '-', version_text(h.compatible[0]), version_text(h.compatible[1]))
        except ValueError:
            line += '  (not under the stock key)'
        print(line)


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('list'); p.add_argument('file')
    p = sub.add_parser('verify'); p.add_argument('file')
    p.add_argument('--model', default='ht802'); p.add_argument('--cpu', default='dvf9918')
    p.add_argument('--generation', type=int, default=1); p.add_argument('--required-bits', default='0000:0000:0000:0000')
    p.add_argument('--quick', action='store_true', help='headers only, no bodies')
    p.add_argument('--model-key', type=lambda s: int(s, 0), help='of the partitions in the unit (default: the model\'s)')
    p = sub.add_parser('build'); p.add_argument('-o', required=True); p.add_argument('--version', required=True)
    p.add_argument('--model', default='ht802'); p.add_argument('--cpu', default='dvf9918')
    p.add_argument('--bits', help='hardware bits (default: the model\'s stock files\')')
    p.add_argument('--model-key', type=lambda s: int(s, 0), help='(default: the model\'s)')
    for k in ('core', 'base', 'prog'):
        p.add_argument('--' + k)
    p = sub.add_parser('part'); p.add_argument('-o', required=True); p.add_argument('--kind', required=True, choices=sorted(KINDS))
    p.add_argument('--version', required=True); p.add_argument('--cpu', default='dvf9918'); p.add_argument('--counter', type=int, default=0)
    p.add_argument('file')
    p = sub.add_parser('extract'); p.add_argument('file'); p.add_argument('dir')
    p = sub.add_parser('uimage'); p.add_argument('-o', required=True); p.add_argument('--name', required=True)
    p.add_argument('--load', required=True, type=lambda s: int(s, 0)); p.add_argument('file')
    a = ap.parse_args(argv)
    rd = lambda path: open(path, 'rb').read()
    if a.cmd == 'list':
        for n, c in enumerate(parse(rd(a.file))):
            _list(c, n)
    elif a.cmd == 'verify':
        dev = Device(a.model, a.cpu, a.generation, bits_tuple(a.required_bits),
                     a.model_key if a.model_key is not None else model_key_of(a.model))
        c = pick(parse(rd(a.file)), dev)
        bad = verify(c, dev, bodies=not a.quick)
        names = wanted(dev).values()
        used = [f.name for f in c.files if f.name in names]
        for b in bad:
            print('REFUSED: ' + b)
        if not bad:
            print('%s: a %s (%s, generation %d) would install %s' % (a.file, a.model, a.cpu, a.generation, ', '.join(used) or 'nothing'))
        return 1 if bad or not used else 0
    elif a.cmd == 'build':
        files = [(k, rd(getattr(a, k))) for k in ('core', 'base', 'prog') if getattr(a, k)]
        img = build(files, a.model, a.version, a.cpu, bits=bits_tuple(a.bits) if a.bits else None,
                    model_key=a.model_key)
        open(a.o, 'wb').write(img)
        print('%s: %d octets, version %s, %s' % (a.o, len(img), a.version, ', '.join(k for k, _ in files)))
    elif a.cmd == 'extract':
        os.makedirs(a.dir, exist_ok=True)
        for n, c in enumerate(parse(rd(a.file))):
            for f in c.files:
                out = os.path.join(a.dir, os.path.basename(f.name) if n == 0 else '%d-%s' % (n, os.path.basename(f.name)))
                open(out, 'wb').write(f.open().raw)
                print(out)
    elif a.cmd == 'part':
        img = partition_image(a.kind, rd(a.file), a.version, a.cpu, counter=a.counter)
        open(a.o, 'wb').write(img)
        print('%s: %d octets' % (a.o, len(img)))
    elif a.cmd == 'uimage':
        img = uimage(rd(a.file), a.name, a.load)
        open(a.o, 'wb').write(img)
        print('%s: %d octets, load and entry 0x%08X' % (a.o, len(img), a.load))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
