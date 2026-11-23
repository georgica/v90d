# v90d: an original dial-up server firmware for the Grandstream HT802

A hardware modem on FXS port 1 dials, gets a V.90 (or slower) connect and a PPP
session that is routed out of the Ethernet port. Everything above the kernel
and libc is written here from the ITU and IETF texts.

Status lives in `PROGRESS.md`, hardware facts in `HARDWARE.md`. This file says
how the thing is built and why.

## 1. Rules the design follows

- Shipped image: Linux kernel, U-Boot, a C library and busybox as they are;
  everything else original. No Grandstream binary, script or code, no closed
  blob (no Smart Link DSP, no SLIC RAM patch).
- Portable C99. Fixed-point only in the signal path. No allocation after
  start-up: every object is a caller-owned struct, no globals, so one process
  can run several calls.
- All modem, telephony and protocol code builds and passes its tests on x86
  and on the ARM target (cross-built, run under qemu-arm; benchmarks on the
  box itself).
- Reference implementations (spandsp, pppd, D-Modem) are test peers only:
  linked into host-only interop tests, never shipped, never copied.
- No flash without a tested recovery path (full backup taken, see
  `PROGRESS.md`).

## 2. The call path

```
 phone line (2-wire)                                   Ethernet
      |                                                    ^
   SLIC + codec (Si3226x): G.711 octets on TDM, 8 kHz      |
      |                                              kernel: routing, NAT
   line back end: 80 codewords + hook state per 10 ms      ^
      |                                                 TUN device
   +----------------------- srv (the server) ----------------------+
   | tel: dial tone, digits, number check, ringing, busy, drops     |
   |   once answered:                                               |
   | dce: answer modem -> V.42 / V.42bis -> PPP server -> IP        |
   | cfg: settings in force; status: live values and call history   |
   +----------------------------------------------------------------+
        HTTP status and settings pages on the LAN side (cfg/httpd)
```

The box has two FXS ports and each one is a complete copy of this path:
its own SLIC, line device (`/dev/v90line`, `/dev/v90line2`), `srv_t`
(telephony, modem, V.42, PPP), TUN device (`ppp0`, `ppp1`) and peer address
(`ppp_peer`, `ppp_peer2`). The daemon is still one process and one thread: it
waits in `poll()` on both line devices and serves whichever has a 10 ms block
ready (read 80 codewords or, from a codec in linear format, 80 samples; 80
`srv_tick`s or `srv_tick_lin`s; write 80 codewords). Shared between the
ports: the network settings, the PPP user table, the web pages, the log, the
NAT table (one masquerade rule per peer address). Per port: number, tones and
line behaviour (`[line]`, `[line2]`), modulations and rate ceilings
(`[modem]`, `[modem2]`), live status and call history.

Everything is sample-driven, one G.711 codeword in and one out at 8 kHz
(`srv_tick`; V.90 needs its exact codewords on the line), no threads, no
timers of its own. The millisecond timers of V.42 and PPP and the seconds of
the telephony layer are advanced from the same sample clock. On the box the
line back end clocks the loop; in tests a channel simulator does.

The line back end's contract (`srv/line.h`): every codeword handed over is
sent exactly once and in order, and the delay from our output to its echo in
our input does not change during a call. How long that delay is, the modem
finds out by itself (4.2).

### Linear receive from the codec

The codec is ours, so it need not behave like an exchange in the receive
direction. Run in G.711 both ways, it compands what it receives together
with the echo of our own transmitter, which on this hardware is as loud at
the codec as the caller (HARDWARE.md); the steps of the quantiser at the
echo's peaks then bound the upstream rate. The Si3217x has one PCM format
for both directions, so the line driver can run the part in 16-bit linear:

* receive: the 16-bit sample goes to the daemon as it is. The scale is that
  of the G.711 tables (`v90_octet_linear`: mu-law up to +/-32124), which is
  the scale every receiver in `modem/`, `v34/`, `v92/` already works on, so
  nothing is shifted and no bits are dropped (assumption A1 of HARDWARE.md,
  to be confirmed on the unit).
* transmit: still 80 codewords per block from the daemon. The driver turns
  each into the exact linear value of that codeword in the port's law
  (`pump_lin16`, one table look-up in the interrupt). Why there and not in
  the daemon: the write format, the pump's transmit ring, the recordings of
  what we sent and the whole transmit path of the modem stay as they are;
  the driver knows the law already; and the exactness V.90 needs is one
  table tested against `v90/v90_pcm.c` for every codeword of both laws.
* the device: a read gives 81 octets (companded) or 161 (linear: 80 samples,
  16 bits, little endian). The daemon offers room for 161 and takes the
  format from the length, block by block, so a switch needs no handshake.
  A one-octet write, `l` or `c`, selects the format (as `u` / `a` the law);
  the driver starts companded.
* the entries: `answer_tick_lin(a, int16_t rx)` returns the codeword to
  send; `answer_tick_pcm` is now that function behind a G.711 decoder.
  `dce_tick_lin` and `srv_tick_lin` likewise; `srv_tick` (rig, simulations,
  companded lines) decodes and calls `srv_tick_lin`. One code path, two
  doors.
* one thing in the modem modelled the receive quantiser explicitly: the
  V.92 upstream design (`v92/v92_design.c`), which spaces the constellation
  points by the G.711 step at the point's amplitude plus 1.5 times the echo
  RMS, and subtracts the quantiser's share from the measured noise floor.
  `answer_cfg.rx_linear` -> `v92d_cfg.rx_linear` -> `v92_dsn_cfg.rx_linear`
  turns that term off (and `v92_p34d.c` measures no quantiser variance).
  The allowance for what the frozen canceller leaves under data, 34 dB
  below the echo in companded format, is 40 dB with linear receive
  (`V92_DSN_ECHO_DIV_LIN`): our choice. The simulation is error free at 46
  dB too; a real hybrid's distortion is not in the simulation.
* nothing else needed a change. The V.34 receivers pick their rate from the
  error they measure at the decision point (`p34d.c`: the TRN2d rule with
  `Q_SLACK`; `v34dx.c`: `mse_limit`), the echo cancellers adapt to
  whatever is left; all of them only see less noise.
* not user settings: the receive format is the daemon's `-R`, the maximum
  downstream power of INFO0d its `-P` (`answer_cfg.max_tx_code`, default 23
  = -12 dBm0; it was a compile-time constant).
* recordings (`-w`, `-W`): a linear one begins with "V90DLIN1" and has 240
  octets per block (160 of samples heard, 80 codewords sent);
  `live/replay` reads both kinds. `live/rec.sh` copies the file and needs
  no change.

In this box the SLIC's codec is the "central office" codec of the V.90 model:
the processor writes G.711 octets straight to it, so the path from our
transmitter to the caller's modem is PCM -> one D/A conversion -> wire, which is
exactly what V.90 downstream needs. There is no digital network in between, so
no robbed-bit signalling and no digital pads (the caller's modem still probes
for them with the DIL; ours just sends what it asks for).

## 3. Source map

| Directory | What | Shipped |
|---|---|---|
| `../libv90modem` | the modem, a repository of its own: V.8 to V.92 and V.42 (its own README and DESIGN); the line simulator and the modem tests live there | yes (the library) |
| `cfg/` | configuration file, status page, settings form, HTTP server | yes |
| `arch/` | Archive mode gateway `v90arch`: catch-all DNS, HTTP rewrite to the Protoweb proxy, FTP server over the same proxy (a separate process, started by the daemon) | yes |
| `ppp/` | PPP server: HDLC framing, LCP, PAP/CHAP, IPCP, CCP with Deflate and MPPC (`ppp_deflate`, `ppp_mppc`) | yes |
| `dce/` | one-call glue: modem + V.42 + PPP | yes |
| `tel/` | telephony layer: tones, DTMF and pulse digits, number check, busy, drops, limits | yes |
| `srv/` | the server (tel + dce + cfg + status) and the Linux daemon `v90d`: line back end, TUN, NAT by nftables netlink; the console services (`console`, `dclist`, `dcnow`, `upnp`, `saturn`); the bulletin board calls (`bbs`, `telnet`, `bbslink`) | yes |
| `test/` | `test.h`, the test harness of every sub-project; the rig's test | no |
| `rig/` | real-modem test rig that rides the stock firmware's SIP/RTP (`modemterm`, `sipmodem`, `pppcall`) | no |
| `peers/`, `refs/` | reference implementations and the ITU texts | no |
| `recon/` | flash backup, device tree, root access tooling | no |

## 4. The modem

The modem is libv90modem (its own repository, `../libv90modem`; its `DESIGN.md` has what was
section 4 here: start-up, V.90 and V.92 digital side, why not x2 and
K56flex, fixed-point conventions). v90d uses its public API
(`include/v90modem.h`): `dce/` runs one modem and one V.42 endpoint per
call, sample by sample.

Bell 103 and Bell 212A (answering; libv90modem's DESIGN.md, 4.1) have no
setting of their own: a port that has V.21 on offers Bell 103
(`V90M_MOD_BELL103`), one with V.22 or V.22bis on offers Bell 212A
(`V90M_MOD_BELL212A`), and a call that began during the 2225 Hz tone is
shown as "Bell 212A". Bell 103 has no error control, as V.21.

## 5. Above the modem

- `v42/`: answerer role. Detection phase (ODP/ADP), LAPM with selective
  parameters from XID, V.44 or V.42bis in both directions (V.44 where the
  caller offers it; `v42/README.md`), and an asynchronous
  start-stop fallback for callers without error control.
- `ppp/`: server only. LCP, PAP, CHAP-MD5 and MS-CHAPv2 against a static
  user table, IPCP with address and DNS assignment. VJ compression, IPv6CP
  and multilink are rejected.
- PPP compression (CCP, RFC 1962) is in `ppp/` too, on by default
  (`[ppp] compression`). Two algorithms written from the RFCs:
  Deflate (RFC 1979 over RFC 1951, `ppp_deflate.c`), which is what Linux
  pppd takes, with windows from 256 octets to 32 KiB, and MPPC (RFC 2118,
  `ppp_mppc.c`, 8 KiB history) for Windows callers. Deflate is
  preferred. Each direction is negotiated on its own; only IP is
  compressed; a packet that does not shrink goes out as it is; loss is
  repaired with Reset-Request / Reset-Ack, so it works on calls without
  error control. It runs in the daemon's main loop, never in the modem's
  sample path, and is bounded twice: a cap on the candidates examined per
  packet and a budget of 40 octets per millisecond per port, beyond which
  packets go out uncompressed. About 193 kB of state per port with the
  32 KiB window (`PPP_DEFLATE_MAX_WBITS`, compile time). It works on the
  packets, above V.42 bis / V.44, which then find nothing left to remove;
  the gain over V.44 alone comes from the larger window and the Huffman
  coding. Readings and figures: `ppp/README.md`.
- PPP encryption (MPPE, RFC 3078/3079) is optional, off by default, and set
  per port (`[ppp] encryption` and `encryption2`). On, it forces MS-CHAPv2,
  keys 128-bit stateless MPPE from the authentication, replaces CCP
  compression, and fails closed: a caller that will not do MPPE is refused,
  never carried in the clear. Clean-room from the RFCs (`ppp_crypto.c`,
  `ppp_mppe.c`).
- `dce/`: pumps bits and octets between the three, converts the sample clock
  to milliseconds, turns carrier loss into a clean PPP teardown and a call-end
  reason.

### 5a. The services for game consoles

Written from what the DreamPi project is seen to do (`refs/dreampi-facts.md`,
facts only); nothing of it is copied, and none of it has met a real console.

`service = dreamcast` is an ordinary PPP call with changes around it:

- In the call (`srv/console.c`, pure, in the packet path): the hop count,
  the doubled connection openings, the opening with the push flag set, and
  the last name the console asked the DNS for.
- In the NAT (`srv/nat.c`, one nftables transaction with everything else):
  destination rewrites from a list of dead server addresses
  (`srv/dclist.c`; at most 32 rules, fetched once at start by a child
  running busybox `wget` and read through a pipe), and the games' ports
  let in from the network towards the console (24 ranges, compiled in).
- Around it, in the daemon, each as a child process so that the line's
  loop never waits for the network: the report to Dreamcast Now every 15 s
  (`srv/dcnow.c`, with a SHA-256 of our own), a DHCP lease for the console
  (busybox `udhcpc` with a client identifier of our own, renewed at half
  time, the address dropped when it runs out) with proxy ARP, and the UPnP
  conversation with the router (`srv/upnp.c`: search, description,
  AddPortMapping / DeletePortMapping; the answer to the search is believed
  only from the search port of the device it names).

Children are forked from a loop that runs with a real-time priority and
locked memory: each resets its scheduling and unlocks before anything else.
Those that become another program rely on close-on-exec; the UPnP child
does not, and closes every descriptor above 2 itself (it once held the TUN
device open across a call).

`service = saturn` is not a PPP call at all. The call object has a second
way of working (`dce_cfg_t.raw_rx`): no error control is offered, V.42 goes
straight to its start-stop mode, and the octets go to the owner instead of
to PPP, with a 2 kB queue towards the modem. The telephony layer accepts
any number on such a port and the server reads the digits as an
instruction (`srv/saturn.c`: wait, call an address, or -- with the off-by-default matchmaking switch -- be paired for a game through STUN (`srv/stun.c`) and the match server at saturn.dreampipe.net); a number that is
neither gets no call. The network side lives in the daemon (`sl_poll` in
`srv/main.c`), non-blocking sockets looked at once per 10 ms block: TCP
65432 for the greeting, then UDP 20001 / 20002 with the tunnel's records
(up to five to a datagram, every datagram twice, 256 octets a record at
the most from us, any size from the peer). Only datagrams from the other
player's address are heard; its source port is taken as its router shows
it. Two things are ours and not the tunnel's: a call that waits ends after
two minutes without a caller, and a peer that has been silent for 30 s
(it should ping every second) ends the call.

### 5b. The bulletin board numbers

Two kinds of number belong to the box, not to a port: the telephony layer
takes them beside the port's own (`tel_cfg.also`, where `?` is any digit),
on every port and whatever the port's service, and the server starts such a
call differently (`srv/srv.c`, `bbs_number()`). Both with or without a 1 in
front. A port whose own number looks like one of them keeps its number.

- **227 and seven digits**: one board each. 227 is BBS on the keys of a
  telephone; the seven digits are the board's login name on the same keys,
  zeros behind a short name (`telehack`: 227 835 3422, `agency`:
  227 243 6290). Names that begin alike would share a number; the second
  takes the next number that is free (116 of 1074 boards on 2026-11-16), so
  the number to dial is the one the BBS page or a `?` search shows. Such a
  number can move when the guide gains a board that sorts before it.
- **1 800 BBS DIAL** (`8002273425`): the operator, a prompt.

- **The call.** No PPP: the call object is in its octet mode, as for the
  Saturn, but with `raw_ec`: error control and compression stay as `[modem]`
  says, because the caller is a terminal program with an ordinary modem,
  and V.92 is offered where it is switched on. The daemon gets the octets
  (`srv_set_raw`) and hands them to `bbslink.c` when `srv_link()` says the
  call is a bulletin board call.
- **The prompt.** `bbslink.c` waits a second and a half after the modems
  connect (HyperTerminal under Windows XP is not listening sooner and lost
  the greeting), then shows `BBS:` and reads a line with a small
  editor (`bbs_edit`: echo, backspace, printable ASCII only). A line is a
  board's login name (compared as letters and digits, so "20 For Beers"
  finds `20forbeers`), a board's number, or `?text` for a search (40 rows
  at most, number, login name and name, paced by what the modem has
  taken). A call to a board's own number skips the prompt and goes to the
  board; a number no board has ends at the prompt.
- **No addresses.** A caller reaches only boards of the list and is never
  shown where one is: not at the prompt, not in "Calling ...", not on the
  page. Where a board is, is the list's business (the log names it).
- **The connection** is made by a child process (`bbsdial.c`: the name
  and the connect within 15 s, then octets both ways with no limit on
  quiet time), with a socket pair as its standard input and output. It
  was busybox's `nc -w 15` at first; that ends a connection the second
  time a quarter minute passes without an octet, which dropped callers who
  were reading (found with SyncTERM on the bench: a call held with one
  key every 20 s ended after 42 s). The
  daemon's loop never resolves a name or waits for a connect. The host is
  checked to be letters, digits, `.` and `-` and not to begin with `-`
  before it becomes an argument; there is no shell. Nothing is read from
  the board unless it can be handed on whole, so a slow modem holds the
  board back through TCP and nothing is dropped. No octet from the board
  before the connection ends means "no answer" and the prompt again;
  otherwise the board has logged the caller off and the line is dropped,
  three seconds after the last octet went to the modem.
- **Telnet** (`telnet.c`, RFC 854/855): the box is the client and asks for
  nothing. It agrees to the board's echo and suppress-go-ahead, to binary
  in either direction, tells terminal type `ANSI` and a window of 80 x 24
  when asked, refuses the rest, and answers only what changes something.
  255 is doubled towards the board; CR is CR NUL while we are not binary.
  A board that speaks no telnet at all (raw TCP) works the same.
- **The list** (format in `srv/bbs.h`). The image has a short one,
  `lists/bbs.txt` as `/etc/v90d.bbs`. The first call to the number or the
  first look at the BBS page makes the box fetch the list, then once a
  day (`fw/bbsfetch.sh`). First from Netstalgia
  (`http://api.netstalgia.online/v1/bbs.txt`: the guide's list already in
  our format, refreshed daily; taken only if its head is `v90d-bbs 2`,
  so an error or challenge page falls through). Else the Telnet BBS
  Guide's own archive from the guide (today's `ibbsMMDDYY.zip`, else the
  month's `ibbsMMYY.zip`, else last month's; nothing without a clock).
  The OBi200 has no awk, so it has only Netstalgia's list and the short one. `fw/bbsguide.awk` turns its
  `dialdirectory.xml` into our lines: telnet boards only, the login name
  is the board's name in lower case letters and digits without the word
  BBS (what 2600.network calls the same board), a second board of one
  name is `name2`, and each gets its number as above. The guide's list is not copied into this repository;
  each box takes it from the guide as it is. What the script prints is
  checked line by line by the daemon (the head line first, at least one
  board) and only then renamed over `/tmp/v90d.bbs`. A box whose owner
  never uses the number fetches nothing. The list is never held in
  memory: it is read from the file, 512 octets per block of the line,
  when a name is looked up. busybox's wget does not verify certificates,
  so somebody on the path could hand the box another list; what that list
  can do is send a caller to another telnet host, which a telnet session
  cannot rule out anyway.
- **The page** (`/bbs`, `status_render_bbs`) shows the numbers and the
  boards, 100 to a page (`/bbs?p=2`) with a search by part of a name or of
  a number (`q=`, kept as letters and digits only). Plain links and a GET
  form, no script, so a period browser can use it. The daemon writes the
  rows (`bbslink_rows`), HTML-escaped, reading the list file anew for each
  page.
- What a list can name is checked: a host is letters, digits, `.` and `-`,
  never begins with `-`, and is not the box itself (`localhost`, numeric
  addresses in 127/8 and 0/8). What a name resolves to is checked again
  before the connection (`bbsdial_is_self`): 127/8, 0/8, the address of any
  of the box's own interfaces (found by asking the kernel which address it
  would send from towards it; nothing goes out), link-local 169.254/16,
  multicast and broadcast are not connected to. The rest of the local
  network is: a board may stand beside the box, and a dial-up caller
  reaches the local network anyway.
  The host is looked at once more before it becomes an argument, and the
  fetched file is created anew, never through a link.

## 6. Firmware (Track A)

A mainline-derived Linux kernel with board support for the DVF9918 SoC written
here (clock, UART, timer, interrupt controller, GPIO, SPI, Ethernet MAC glue,
TDM), from the hardware facts in `HARDWARE.md`; the vendor's GPL tree was
studied for those facts and is not copied. Our own device tree for the
9610003725A board, a busybox initramfs, and two things of ours: the SLIC/codec
driver with its line back end, and the daemon `v90d` (section 2), which is
also the HTTP server.

Network: the LAN side is busybox's business (udhcpc or a static address, from
`v90d -p`). The PPP side is a TUN device the daemon creates; the kernel
forwards, and masquerades the caller behind the LAN address with one nftables
rule that the daemon installs over netlink itself. Kernel options this needs:
TUN, NF_TABLES, NFT_NAT, NFT_MASQ, NF_CONNTRACK, IP forwarding.

Track A cannot start on the bench until the serial console is wired (RAM boot
is the only safe way to try a kernel) and the SLIC question below is decided.

Open hardware risk, unchanged: the Si3226x SLIC wants a RAM patch at every boot
that we may not ship. See `PROGRESS.md` for the three ways out.

## 7. Testing

- Every module has a host test (`make test`), the same tests run cross-built
  under qemu (`make test-arm`), sub-projects have the same two targets.
- Peers written here (libv90modem's `sim/`): channel with loss, delay, echo, noise, clock and
  carrier offset; calling modem; V.90 Phase 2 analogue side; QAM modulator.
- Independent peers (`make test-interop`, host only): spandsp for V.8,
  V.22bis, V.42 and V.42bis; pppd for PPP.
- Lessons that shaped the tests: a wrong-sign timing loop once passed every
  bit-error test because the equaliser hid it, and a delay line that moved
  under the equaliser update only showed for a third of the sub-sample
  alignments. So the tests check loop and equaliser state (tap centre, loop
  frequency, convergence across alignments), not just error counts.
- `make check` / `make check-arm` run every sub-project; `make check-ns` runs
  the real daemon in a network namespace (TUN, NAT, HTTP), and, where pppd,
  passwordless sudo and the kernel's `ppp_deflate` module are present, calls
  it with a real pppd to check CCP and Deflate against the kernel
  (`srv/test/ns_pppd.c`; skipped with a message otherwise).
- What simulation cannot show: behaviour against real modems and the real
  SLIC. `rig/` exists to put the answering modem behind the stock firmware's
  SIP path for that, before our own firmware boots.

## Starting from flash without a serial cable (2026-11-14)

The user's rule: no UART. U-Boot does not fall back when a kernel with a
valid header hangs, so replacing the stock kernel in its slot would leave no
way back. Instead nothing of the stock system that boots today is touched:

- **U-Boot and the running stock set (B) stay as they are.** Our image (a
  legacy uImage around the zImage with its device tree) goes into the
  inactive stock copy's `base` partition, flash 0x2E0000 (3 MiB); our
  settings into the first 64 KiB of its `core`, 0x060000. With set A no
  longer valid, `gsboot` picks set B by its own rules.
- **U-Boot's saved environment** (8 KiB at 0xFBE000, erased on a stock unit)
  gets the built-in variables unchanged plus:
  `bootcmd=run v90d_try; gsboot`, a counter `v90d_tries=3`, and a trial that
  counts it down, saves the environment, and only then runs
  `bootm 0x802E0000`. Whatever goes wrong in the trial short of our kernel
  starting, `gsboot` follows. An image that hangs gets three power cycles,
  then the unit boots the stock firmware again.
- **Our system confirms a good start** by putting the counter back to 3 after
  the daemon has been up for a minute (`fw/rcS`, `fw/tools/v90nv`).
- **Our flash driver** (`fw/drv/qspi.c`, tested on the host against a model
  of controller and chip; kernel glue `dvf99-qspi`) refuses every erase and
  program outside three ranges (image, settings, environment), and the device
  tree marks everything else read-only as well.
- Entered through `bootm` from `bootcmd` (not through `gsboot`) the kernel
  starts with the MMU and data cache off, as in the RAM boots; U-Boot has set
  up Ethernet, the SLIC's SPI bus, the PCM pins and the flash on every boot.
- Order of the first writes: (1) environment with the trial's boot step
  reduced to `saveenv`, written with the stock firmware's own flash driver;
  reboot; the counter must read 2 and every other partition must be
  unchanged; (2) our image written and read back, the real environment, a
  reboot into our firmware.

Done on the unit on 2026-11-15, both steps, with the results in
PROGRESS.md; the fall-back to stock after three unconfirmed starts was
exercised as well. The flash driver repeats a page program that did not
arrive, after resetting the controller (HARDWARE.md): without that the image
could not be written.

What this cannot cover: an environment that U-Boot accepts and that makes
U-Boot itself hang. The script is kept to `if test`, `setenv`, `saveenv`,
`bootm` and ends in an unconditional `gsboot`; `console` is left at `no`.
Returning to stock on purpose: set the counter to 0 and reboot. Arming from
the stock firmware: its root shell and `v90nv env-write` on `uboot_env`.

## Release image (design, not yet proven on hardware)

Goal: one file, uploaded on the stock firmware's own upgrade page, after
which the box runs v90d; and a way back from our web page. Everything here
is built and tested on models (`make -C fw test-release`); nothing of it has
run on the box. Facts about the stock side are in HARDWARE.md (Bootloader,
Firmware image format).

What decides the design: the stock upgrader writes only the set that is not
running, file by file, and `gsboot` alone picks the set at the next start. It
picks by magic, checksum, the `prog` to core/base version match and the
provision counter; it does not care what the payloads are, as long as `core`
holds a U-Boot image. A unit that never saved a U-Boot environment runs
`gsboot` by default, so the release needs no environment at all (the
`v90d_tries` scheme of the previous section stays a bench tool).

The file (`fw/tools/mkgsfw.py`, `make -C fw release`) is a stock-format
container with three files, all version 90.0.1.0:
- `core`: our loader (`fw/loader`, about 6 KiB) wrapped as a U-Boot image,
  load and entry 0xC9000000, away from where the system is unpacked. Its length
  is padded to whole 32-octet cache lines: `bootm` only cleans the data
  cache behind its copy when the range ends on a line (HARDWARE.md).
- `base`: the system image `v90d.uimage` as it is.
- `prog`: a few octets of text. It is there because `gsboot` wants all
  three partitions good, and its header names our core and base versions,
  which makes our set the one "compatible" set.
No `boot` file: U-Boot is never touched. The version only has to differ
from what is installed; 90.x cannot meet a stock number.

Why a loader in `core` and not the kernel: the system image (2.2 MB) does not
fit `core` (1280 KiB) without taking block devices and more out of the
kernel, and `gsboot` has no second try once a core image passes its CRC. The
loader is that second try, in code of ours that is small enough to test
whole on the host.

The loader (portable part `boot.c`, tested against the flash simulator of
`fw/drv`; `start.S` and `hw.c` are the untested hardware half):
1. turns caches and MMU off cleanly and learns its set from
   `bootpartition=` in the device tree U-Boot passes. It does not touch the
   watchdog: started by the loader, the watchdog reset the box 2 to 3 s
   after every feed (PROGRESS.md, "The loader on the unit"). A hang between
   the loader and the kernel's own watchdog driver (first half second of the
   kernel) is caught by the count in step 3, with three power cycles;
2. refuses to write anything unless its own `core` carries its tag and the
   board part number in `factory` is one it knows; the only flash areas it
   can write at all are the first sector of its own `core` and one state
   sector inside `core` (+0x10000);
3. counts the start in the state sector (one bit cleared per start, no
   erase), finds the system image (own `base`, then the other set's), copies
   it to RAM, checks the CRC there and jumps;
4. gives up after 3 starts that the system did not confirm, or when no good
   image is found: it programs zeros over the magic of its own `core` and
   resets. `gsboot` then finds our set bad and starts the stock set, which
   the install never wrote. Zeros are programmed over the magic, never an
   erase first, so there is no moment with a good magic and a bad length.

Back to stock from our system is the same single write (zero the magic of
our own `core`) followed by a reboot. Stock then runs from the other set and
can upgrade itself or take our file again, both into our set.

Power loss during install: each file is whole in flash before the next
starts and `prog` comes last; until `prog` is good our set is not "all good"
and stock keeps starting (modelled for a cut between any two files and
inside each). One hazard is not ours to remove: a cut inside the first page
of a file, which is the same for every stock upgrade (HARDWARE.md).

Other hardware: the upgrader refuses a container whose names lack the
model or whose bits lack the unit's required bits; the file headers carry
cpu "dvf9918", so a dvf9919 unit refuses it. The loader's part number check
is the last gate: on an unknown board it leaves the stock set to start.

### The stock header key in our files (decided by the owner, 2026-11-15)

The stock upgrader only accepts a file whose header is encrypted with the
key text every HT802 falls back to when it has no key of its own
(`STOCK_KEY_TEXT` in fw/tools/mkgsfw.py; the same 32 characters appear in
fw/tools/nvimage.c so that our own upgrade page takes the same file). It is
a constant read out of the stock firmware, not code, and without it nobody
could install v90d from the stock web page. The originality rule forbids
Grandstream binaries, scripts and code in the image; this constant is the
one deliberate exception, kept by decision of the project owner because the
single uploadable file needs it.

### The running system's side (built and tested on models, not run on the box)

One system image serves both ways a box can start it: the bench way of the
previous section (U-Boot's trial script, image at 0x2E0000, settings at
0x060000) and through the loader from either stock set. Everything below
finds out at run time which it is.

**Nothing is handed from the loader to the kernel, and nothing needs to be.**
The loader enters the kernel with r2 = 0 and the kernel uses its appended
device tree, so there is no channel without changing the loader (a record in
RAM, a patched property) or the kernel. The questions the running system has
are all about the next start, not the last one: which signature takes the
box back to stock, which count must be cleared, where the next start will
look for an image, where the settings are. Each has its answer in the flash,
and reading it there is the same reading the boot programs will do next
time, so it cannot go stale and cannot disagree with them. `v90nv where`
does it (`fw/tools/nvboot.c`):
- the set whose `core` carries the stock signature and, behind the stock
  header, a U-Boot image named `v90d-loader` is our set: scheme "loader".
  That is the loader's own test of itself. (Both sets ours cannot come out
  of an upgrade; then the one with the higher install counter, as the loader
  decides when the boot loader does not tell it.)
- else, a saved U-Boot environment with a `v90d_tries` variable: scheme
  "old", the bench way.
- else "none" (started from RAM, or a stock unit): no flash range is
  allowed and every writing command refuses.
What this cannot tell is who made the last start in the one state where both
could have: the stock upgrade cut off after writing our `core`, on a bench
box whose trial script still finds the old image. Then the scheme is
"loader", `upgrade` refuses (the set is half installed), and `confirm` and
`to-stock` serve both mechanisms. The models cover it.

**Where things are** (X = start of our set's `core`, 0x060000 or 0x1A0000):

| what | loader scheme | bench scheme |
|---|---|---|
| the signature that makes the box ours | X, 4 octets | `v90d_tries` in the environment |
| count of unconfirmed starts | X + 0x10000, one word | `v90d_tries` |
| settings | X + 0x20000, 64 KiB | 0x060000, 64 KiB |
| system image | our set's `base` + 0x200, behind a stock header | 0x2E0000, bare |

**Settings in the loader scheme are inside our own `core`**, beyond the
loader and its count. Neither stock set is written for them, a self-upgrade
(which only writes `base`) keeps them, and the stock boot loader does not
look there (it adds up only the length the header names). What cannot be
had: the stock upgrader erases all of `core`, `base` and `prog` of the set
it installs into, and that set is always ours, so the settings do not
survive going back to stock and installing again, nor moving from the bench
scheme to the loader scheme (0x060000 becomes the loader's header). Nothing
outside the two sets is free for them: `nvram` and `lang2` are rewritten by
the stock system at every start, `oem` holds stock data, the environment
partition is 8 KiB with one checksum over all of it, and the rest is boot
loaders and `factory`. The pages say so where it matters.

**What may be written.** The flash driver's list (device tree,
`v90d,writable`) now covers what either scheme in either set needs: the
first sector of each `core`, each count sector, each settings area, both
`base` partitions, the environment; seven ranges, the list's size went from
4 to 8 (`fw/drv/qspi.h`). No kernel option and no driver logic changed. The
kernel cannot know which set is ours without reading the flash the way
`v90nv` does, so the narrowing to one set is in `v90nv`: it works on the
whole-chip MTD device ("all", no longer read-only in the device tree) by
physical offset and allows itself only the ranges of the scheme it found
(`nv_locate`). The cost, plainly: the kernel alone no longer protects the
stock set's `base`, the first sector of its `core` and two small areas
inside its `core` from a root shell (`flashcp` on a bring-up build) or from
a bug in `v90nv`; the second is what the host tests are for (every action is
compared, octet for octet, against the ranges it may touch). Boot loaders,
`factory`, `nvram`, both `prog` and the body of both `core` stay out of
reach of any program, as before. A driver that narrowed itself once at boot
would close the gap; it was left out because it is kernel code that cannot
be tested off the box and a mistake in it makes the settings and `confirm`
fail, which after three starts means the stock firmware.

**The actions** (`v90nv`, all through one layer, `nvflash.c`, that takes an
MTD device or a 16 MiB file; a file programs like NOR flash, bits only go to
zero, and can stop before any step for the power-cut tests):
- `confirm`: loader scheme, erase the count sector if a start is counted
  (what `boot.c` expects: all ones). Bench scheme, `v90d_tries` back to 3.
- `to-stock`: only if the other set passes the boot loader's test of all
  three partitions and is not ours; otherwise nothing is written. Loader
  scheme: zeros programmed over the four signature octets of our `core`, as
  the loader does it. Bench scheme: `v90d_tries` is set to `x`, a value no
  branch of the trial script matches and `v90nv` reads as "no count" (not
  0: a 0 is also what a last, good start reads, and the `confirm` that the
  boot script runs on the way down would arm it again; and not removed:
  this U-Boot takes the first branch when the variable is missing, found on
  the unit, HARDWARE.md).
- `upgrade FILE`: FILE is a system image (U-Boot header named
  `v90d ht802 VERSION`, load address ours, both checksums) or a firmware
  file as `mkgsfw.py` builds it, of which only `base` is used (names carry
  the model, layout generation 1, cpu text dvf9918 or any, the stock
  checksum, then the same image checks). The loader in `core` is never
  rewritten from the running system: it is the way back. Refused, with
  nothing written: any failed check, a board whose part number in `factory`
  is not one this ran on, scheme "none", a half installed set. Then, into
  our `base` (bench: 0x2E0000):
  1. zeros over the signatures that are there (stock header, image header);
  2. erase and write everything behind the first sector;
  3. erase the first sector, write it with both signatures left blank;
  4. read all of it back;
  5. program the image's signature, then the stock header's, 4 octets each.
  The stock header is made from the one in flash: version as our `prog`
  names it (or the set stops being the "compatible" one), the set's install
  counter, new length and checksum.
  Power lost before step 1: the old image starts. After the last write: the
  new one. Anywhere between: the boot loader finds our set incomplete and
  starts the stock firmware (bench: `bootm` refuses the image, `gsboot`
  follows), and the file has to be given to the stock page again. There is
  no window in which a half image passes a check; this is modelled for a cut
  before every step and inside every step, both schemes, both sets. While
  the box stays up, the running system is whole in RAM and can simply write
  again.
- `conf-save`, `conf-load`: the settings blob at the scheme's place, tag
  written last.
- Environment rewrites put the checksum in last; cut short, U-Boot uses its
  built-in environment, which starts the stock way.
One hazard is accepted: power lost inside the 50 ms in which `confirm`
erases the count sector can leave a word that counts as three starts, and
the loader then gives the box back to stock. It errs to the safe side.

**Boot script** (`fw/rcS`): `v90nv where` into `/tmp/v90d.where` first; the
settings through `conf-load` and `conf-save`; `v90nv confirm` after a minute
with the daemon alive and before a restart that was asked for. On a board
that no unit has run (`where` says `tried=no`: in the loader's list from
its flash contents alone) the confirmation also waits until the network
port has received a packet (`fw/startgood.sh`), so a box whose Ethernet
does not work under our kernel is back in the stock firmware after three
starts instead of being ours and unreachable. With scheme "none" it writes
nothing.

**Web page** ("Firmware" tab, behind the login like the settings): version
and build, scheme and set, what the next start will run, the count, whether
the stock set is good; an upload form; "Back to the stock firmware" behind a
second page that says what will happen and wants a box ticked. The daemon
never reads an upload and never touches the flash. `cfg/httpd.c` checks the
request (login, same origin, length up to 4 MiB) and hands the connection
itself to the daemon's callback, which refuses while any line is off hook
or a write is running, and otherwise forks `v90nv serve-upgrade` or
`serve-stock` with the socket as its standard input and output (the octets
of the body that had already arrived go ahead through a pipe). The helper
runs without real-time priority, takes the file into its own memory, checks
it completely, answers the browser, and only then writes; how the write
went is left in `/tmp/v90d.fwstate`, which the page shows, and on success
the restart note of the Restart button is left for the boot script. A call
that begins while the flash is being written is not protected: the write
takes a few minutes and ends in a restart.

**Other hardware.** Three gates on the way in (stock upgrader: model name
in the file names, required bits; file headers: cpu text; loader: part
number) and one more in the running system: `v90nv upgrade` wants the
board name in the image's own name and the box's part number in the list
the loader uses (`LD_BOARDS`, one place).

**Version.** `fw/VERSION` (one line). It is in the image's name, in the
firmware file's version (90. in front), on every page's footer and on the
Firmware page.

**`make -C fw release`** builds its own system image without the bring-up
shell and without bench kernel arguments into `fw/build/release/` and wraps
it; `make release ALLOW_DEBUG=1` wraps the bench image that is in
`fw/build/` under the name `v90d-ht802fw-bench.bin`.

Still open, in this order on the box: see PROGRESS.md ("Release image:
system side"). `REL_BITS` in `fw/Makefile` waits for the unit's required
bits (one `cat` on the stock system).
