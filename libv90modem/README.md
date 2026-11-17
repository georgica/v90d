# libv90modem

A voice-band data modem in software: the answering side of V.92, V.90,
V.34, V.32bis, V.22bis, V.22 and V.21, started by V.8, with V.42 error
control (LAPM) and V.42bis / V.44 compression. Portable C99, fixed-point in
the signal path, no allocation after start, no global state. It works at a
G.711 codec, 8000 samples per second: give it the samples it hears, it gives
back the samples to send.

It was written for v90d, a dial-up server that runs in a
Grandstream HT802 analogue telephone adapter, from the ITU-T
recommendations, and has answered real modems there (see the table below).

**The API is not stable before version 1.0.** This is 0.1.0.

## Standards

| | implemented | notes |
|---|---|---|
| V.8 | answering: ANSam, CM / JM / CJ | callers without V.8 are answered too (V.22bis sequence, V.32bis AA, V.21 after 8 s) |
| V.21 | yes | 300 bit/s |
| V.22, V.22bis | yes | 1200 / 2400 bit/s, retrain, guard tone |
| V.32, V.32bis | answering | 4800 to 14400 bit/s |
| V.34 | answering | Phase 2 to data mode, retrains, rate renegotiation |
| V.90 | digital (server) side | PCM downstream, V.34 upstream |
| V.92 | digital (server) side | PCM upstream, full start-up only |
| V.14 | yes | start-stop characters on the synchronous bit pump |
| V.42 | yes | LAPM, detection phase, start-stop fallback, breaks |
| V.42bis, V.44 | yes | V.44 preferred when both ends have it |

Not implemented:

- The calling (originating) role. `V90M_ROLE_ORIGINATE` is in the API and
  returns `V90M_ENOTSUP` for now.
- The analogue (client) side of V.90 and V.92.
- V.92 quick connect, short Phase 2 and modem on hold.
- x2 and K56flex, the two 56k schemes from before V.90: not spoken. A modem
  that has nothing newer connects at V.34. (Neither was published; see
  `DESIGN.md`, 4.2b.)
- Bell 103 and Bell 212A.
- Fax (V.17, V.29, V.27ter), V.23, half-duplex V.34.

## Building

```
make                    # build/libv90modem.a, libv90modem.so, libv90modem.pc
make check              # the simulator suite on the host
make check-arm          # the same, cross-built and run under qemu-arm (ARM_CC, QEMU)
make install PREFIX=/usr/local      # make uninstall takes it out; DESTDIR works
make CC=arm-linux-gcc BUILD=build/arm   # cross-compile: ar, ld, objcopy from the same prefix
```

A program builds with `pkg-config --cflags --libs libv90modem` (or
`--static` for the static library, which also needs `-lm`). Only
`v90modem.h` is installed, with the libraries, the `.pc` file and the
`v90m-replay` tool. The static library exports the `v90m_*` functions and
nothing else; everything inside it is local.

`make test-interop SPANDSP=PREFIX` runs V.8 and V.22bis against spandsp
(host only). `make bench-v90` replays a simulated V.90 call through the
modem on the host and under qemu, by the internals and by the public API
(static and shared library): the four transmit hashes must be equal.

## Using it

```c
#include <v90modem.h>

static int  get(void *u) { return 1; }           /* the next bit to send */
static void put(void *u, int bit) { }            /* a bit received */

v90m_cfg c;
v90m_cfg_default(&c);                     /* answer; V.90, V.34, V.32bis, V.22bis, V.22, V.21 */
c.getbit = get; c.putbit = put;
v90m_t *m = v90m_create(&c, &err);        /* the one allocation (or v90m_init on your memory) */

for (;;) {                                /* 10 ms at a time, or any other block size */
    uint8_t rx[80], tx[80];
    read_codec(rx, 80);
    v90m_process_g711(m, rx, tx, 80);
    write_codec(tx, 80);
    v90m_get_state(m, &st);               /* or an on_event callback */
}
```

Data moves as bits: `getbit` and `putbit` are called by the modem, inside
`v90m_process_*`, for every bit it sends and receives once connected. Put a
V.42 endpoint on them (`v90m_ec_*`: `v90m_ec_tx_bit` from `getbit`,
`v90m_ec_rx_bit` from `putbit`, `v90m_ec_tick` every millisecond, octets in
and out with `v90m_ec_write` / `v90m_ec_read`), or a start-stop framer
(`v90m_async_*`) for a caller without error control.

`examples/answer_file.c` (`make examples`) answers a recorded call and
prints the connect line:

```
$ build/answer_file -9 -u 33600 cxa.raw
cxa.raw: 26.4 s, 16-bit linear receive; answer tone at 6.692 s
CONNECT 50666/25333 V.92 at 24.805 s; SNR 60.0 dB, round trip 263 samples
```

## Tested modems

Calls made to the library inside v90d on an HT802, with the modems below
calling; the figures are downstream / upstream bit/s.

| modem | firmware | date | what connected |
|---|---|---|---|
| Conexant CX93001 (USB, V.92) | | 2026-11-15 | every modulation; V.90 49333 to 53333 / 28800; V.92 50666 / 25333 (`AT+PIG=0`); V.34 31200 / 28800 |
| U.S. Robotics 5686 (56K FAX EXT, V.92) | 5.4.62 | 2026-11-16 | V.90 54666 / 26400; V.92 54666 / 40000 |
| U.S. Robotics 56K FAX EXT of 1998 (x2, V.90 upgrade) | flash 5.1.4 | 2026-11-16 | V.90 54666 to 56000 / 28800; V.34 28800 when its V.90 start-up failed |

Not tried: any other modem, and the calling side (not written yet).

## Reporting a bug

Most faults show only with one brand of modem. Please send a recording of
the call: what the modem heard and what it sent, at the codec, in the
format of `tools/README.md` (v90d writes it with `-w FILE`). Then

```
v90m-replay [options as the modem was set up] RECORDING
```

plays it into the modem and prints what it made of the call, phase by
phase. Say which modem called (make, model, firmware: `ATI3`, `ATI6`, `ATI7`
on most), how the library was configured, and what happened. A fix comes
with that recording as a test fixture (`CONTRIBUTING.md`).

## Licence

GPL-2.0-only, see `LICENSE`.
