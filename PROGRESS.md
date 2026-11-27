# Progress

Brief (current): a fully **original** self-contained dial-up server firmware on
the HT802: our own kernel boot + rootfs, SLIC driver, telephony, modem stack
(V.90 digital side), and PPP server; no Grandstream binaries or closed blobs in
the shipped image. Stock firmware/GPL studied for hardware facts only
(→ HARDWARE.md). Design and reasoning in DESIGN.md.

## Stage 0: hardware reconnaissance: COMPLETE (on the real unit). Verdict below.

All items done against the physical box (HW V2.5A, part 9610003725A), via a root
shell obtained by injecting a telnetd root service into a same-version firmware
repack and flashing it through the box's own validated web upload (details in
HARDWARE.md → Root access). Evidence is on the box / in `~/v90d/recon/`.

| 0 | Item | State | Evidence |
|---|------|-------|----------|
| a | SoC, RAM, flash+map, PHY, SLIC, GPIOs | **done, on box** | HARDWARE.md; `/proc/{cpuinfo,mtd,iomem,interrupts}`, hw_features, board DTB |
| b | Bootloader: interruptible / unsigned / TFTP | strong (image+GPL), UART unconfirmed | `gsboot` = magic+checksum+version only, no RSA; Esc to stop autoboot |
| c | Kernel / board support | **GO** | GPL `ht8xx_gpl.tar.gz` has full `arch/arm/mach-dvf` + DTS for this SoC |
| d | SLIC from-scratch driver | **QUALIFIED** | Si3226x on SPI+TDM reachable; RAM-patch blob + NDA-only reg map is the risk |
| e | CPU budget | **GO** | on-box bench: ~18 % of one core for the modeled EC+FE+EQ budget |

### Key measured facts (on the box)
- **DVF9918** ARM926 @400 MHz, no FPU/NEON. Bench: rfir 25.4 / rlms 27.6 /
  cfir 57.5 / clms 47.8 MMAC/s → ~18 % core for ~4.45 MMAC/s of EQ-class work.
- Flash 16 MiB GD25Q128, 15 partitions (A/B), full map in HARDWARE.md.
- Ethernet: Motorcomm **YT8512H**, RGMII, internal clock (stmmac GMAC @0x06e00000).
- SLIC: **Si3226x** dual ProSLIC (Chip Type 28 / rev C), SPI @0x05e00000 (1 MHz),
  PCM on **TDM0 @0x05f00000**; FXS1 = channel 0. `/dev/fxs0x`, `/dev/voice*`.
- GPIOs (board DTB): LEDs 24/25/26/27, reset-button 22, SLIC reset 21/19, SPI CS
  16/17, SLIC int IRQ 46/47, TDM IRQ 43.
- Firmware images are AES+checksum, **not signature-signed** (flasher imports no
  RSA/verify). gssu removed from modern firmware; downgrade refused by version
  check, so root came from a same-version injected repack instead.

### Recovery (satisfies the brief's pre-flash requirement)
- **Full 16 MiB backup taken and verified** (15/15 partitions md5-match the box):
  `~/v90d/recon/backup/`: `flash_all.bin` + per-partition `mtd_*.bin`, sha256
  recorded. `mtd_prog2.bin` = clean stock /app; stock base/uboot/factory saved.
- Reflash proven via the box's own validated web upload / `upgrade` command.
- Still to wire up for the *kernel/bootloader* flashes of Track A: live UART (and
  an SPI clip as ultimate backstop). The current recon root did not need them.

## GO / NO-GO

**GO**, with one scoped risk. The hardware is capable and open enough for the
project: the SoC has public GPL kernel + bootloader source, the bootloader takes
unsigned images and is interruptible, firmware images are not signature-locked
(we can build and flash our own, with a verified full backup for recovery), the
PCM bus is reachable from the ARM via the documented TDM block, and the CPU has
clear headroom (~18 %) for the fixed-point modem.

**The one open risk is the SLIC (0d).** The Si3226x needs a ~900-byte RAM/PRAM
DSP *patch* that the stock driver loads every boot, and its register/RAM map is
NDA-only. Whether a rev-C part does battery feed / hook detect / ring / PCM
*without* that blob is undocumented and unverified. A from-scratch SPI/register
driver is achievable; bringing the line up **patch-free** is the unknown that
decides whether the fully-original goal holds on this board.

Next decision for the user: how to treat the SLIC patch: (i) attempt patch-free
Si3226x bring-up and reverse-engineer the needed register/RAM writes (research
risk, no public map), (ii) carve a narrow exception to the no-blob rule for the
SLIC RAM patch only (keeps the HT802, everything else original), or (iii) move
to the fallback board.

**Fallback board:** a Linux-capable board + an **Si3210/Si3215** FXS SLIC
(fully public 118-page programming datasheet, **no firmware patch**, existing
DAHDI driver, raw 8 kHz PCM highway), e.g. an OpenWrt router or Raspberry Pi
with a Si321x FXS module. Same modem/PPP stack; only the SLIC/PCM glue changes.

## Track B: modem (x86 first). B1 DONE.

Build tree: `sim/` (channel simulator, x86 test peer, double OK), `test/`
(harness `test.h` + per-module tests), `Makefile` (`make test` on host,
`make test-arm` cross-builds with the Bootlin toolchain and runs under
qemu-arm). Signal-path modem code (B2+) will be portable C99 fixed-point and
build under both.

**B1 channel simulator: complete, 62 checks pass on x86 AND ARM (qemu):**

| Module | What it models | Tests |
|---|---|---|
| `sim/g711` | V.90 Table 1 Ucode grid + mu-law encode/decode | 19 (anchor values, monotonic, ±0 alias, round-trip, clamp) |
| `sim/noise` | deterministic AWGN at a target dBFS | 8 (determinism, N(0,1), level within 3%) |
| `sim/filt` | FIR for the subscriber-loop response | 14 (impulse, unity, DC gain, convolution) |
| `sim/resamp` | clock offset (±ppm), streaming linear interp | 8 (passthrough, DC, 2× upsample, count tracks ppm) |
| `sim/channel` | full-duplex: PCM transparency + digital pad + loop filter + hybrid echo + AWGN | 13 (downstream transparent, pad −6 dB, SNR, upstream encode, echo level + delay) |

Known limitation to revisit for the high-rate receiver: `resamp` is linear
interpolation (fine for modeling drift; upgrade to windowed-sinc when its
interpolation noise would matter). `channel` runs matched-clock; the validated
`resamp` stage gets inserted when timing recovery is tested (B3+).

## Track B - B2 V.8 answer side: DONE

Answer side per V.8 8.2 / V.90 9.1.3: 0.2 s silence, ANSam with phase reversals
while listening on V.21(L), JM on V.21(H) after two identical CM, stop on CJ,
75 ms silence, result = selected modulation (V.90 or the lowest common Table 4
item) or NO_CM (legacy caller, continue as a pre-V.8 answerer).

| Module | What it does | Tests |
|---|---|---|
| `modem/v21` | V.21 FSK mod + noncoherent demod, block and per-sample forms | 9 |
| `modem/ansam` | ANSam: 2100 Hz, 15 Hz AM, 450 ms phase reversals | 7 |
| `modem/v8msg` | CM/JM coding (Tables 1-7), framing, JM derivation, selection | 41 |
| `modem/async` | start/stop receiver on the demod level stream (timing from start edges, +/-1 % baud) | part of v8rx |
| `modem/v8rx` | sequence assembler: 2 identical sequences, CJ | 19 |
| `modem/v8ans` | answer state machine | 21 (end to end vs `sim/v8call` through the channel: echo, noise, no-caller timeout) |

Test output (this pass): `make test` 159 checks, 0 failures, ALL TESTS PASS;
`make test-arm` the same 159 under qemu-arm, ALL ARM TESTS PASS.

**Interop:** `make test-interop` runs our answerer against spandsp 0.0.6's
independent V.8 caller (distro library unpacked under `peers/`, test only, never
shipped) through the channel simulator: V.90 analogue caller with and without
CI, and a V.22bis caller on a noisy line. 30 checks, 0 failures. spandsp reads
our JM as V.90 + V.34 + V.22 + V.21, digital PCM modem, digital access, LAPM.

Spec readings chosen (flagged):
- JM PCM bits: V.8 does not say whether JM echoes the common set or the
  answerer's own type. Reading: JM carries our own type (digital, b6) and only
  when CM offered the complementary analogue type (V.90 9.1.1) and V.34 is
  common (V.8 6.3).
- JM duration: 8.2.3 leaves the no-CJ case open. Reading: 5 s bound, then
  result NO_CJ.
- Preamble detection accepts >= 8 bit times of mark (10 are sent) for timing
  margin.
- V.21(L) squelch at about -46 dBm0, between V.21's -43 / -48 dBm detector
  points.

Known limits: the V.21 demod is a plain boxcar correlator with no channel
filter (enough for V.8 with echo at 0.35 linear and 20 dB SNR in test; revisit
if real-line results say otherwise).

## Track B - B3 V.21 / V.22bis answering: DONE on x86 + ARM + spandsp; real modem NOT yet tried

| Module | What it does | Tests |
|---|---|---|
| `modem/scramble` | V.22bis scrambler/descrambler (1 + x^-14 + x^-17, 64-ones guard) | in v22tx |
| `modem/v22tx` | line coder (Table 1, Figure 2), RRC pulse shaping (40 phases/symbol), carrier | 25 |
| `modem/v22rx` | fractional-delay band-pass matched filter bank, AGC, Gardner timing, T/2 LMS equaliser, decision-directed carrier loop, slicer, decoder, signal classifier (S1 / unscrambled ones / data) | 28 |
| `modem/v22bis` | handshake 6.3.1 both roles (2400, and 1200 with a V.22 peer), data mode | 23 |
| `modem/v14` | start-stop characters over the sync bit stream | 15 |
| `modem/answer` | top level: V.8, then V.22bis/V.22 or V.21; pre-V.8 callers; V.21 fall-back | 23 |
| `rig/modemterm` | the modem behind mu-law frames with a banner + echo terminal | 6 |
| `sim/callpeer`, `sim/resamp` (windowed-sinc variant) | calling-modem test peer; clean clock-offset model | used above |

Test output (2026-11-14): `make test` 301 checks, 0 failures, ALL TESTS PASS;
`make test-arm` ALL ARM TESTS PASS (qemu-arm; run before `modemterm` was
added, rerun pending); `make test-interop` 52 checks, 0 failures.

Receiver results in test (x86, same code on ARM): 0 bit errors at 2400 bit/s
with -33 dB level + mu-law, +/-7 Hz carrier offset (the spec limit), +/-200 ppm
clock offset, linear distortion, and all of those combined with noise; clean
channel error floor about -40 dB. 1200 bit/s runs error-free at 12 dB SNR.

**Interop with spandsp's independent V.22bis caller** (through the channel
simulator, 8000 bits each way): 2400 bit/s 0 errors both directions; spandsp
as a 1200 bit/s V.22 caller 0 errors; us limited to 1200 against a 2400 caller,
both fall back, 0 errors; 2400 on a noisy line with strong echo, 0 errors.

**On the real box (root shell, RAM only, nothing flashed):** `rig/bench_modem`
ran the actual modem code on the DVF9918: V.22bis (transmitter + receiver, data
mode) = **2.14 % of the CPU**, V.8 answer phase = 1.96 %. The SIP/RTP rig
`sipmodem.arm` starts on the box and binds its ports.

**Not done / not verified:**
- No call from a hardware modem yet. `rig/README.md` is the runbook; it needs
  someone at the USR modem and the port-1 SIP settings pointed at the rig.
- V.22bis rate change (6.6) is not implemented. Retrain (6.4) is, in both
  roles (2026-11-15, see "V.22bis: noise start-up, retrain, guard tone").
- Guard tone is off by default (national option, V.22bis 2.1);
  `v22m_set_guard(m, 1800 | 550 | 0)` turns it on.
- V.21 data mode has only the end-to-end test in `test_answer`.

Mistakes caught on the way, kept as tests:
- The timing detector first had the wrong sign and every bit-error test still
  passed, because the T/2 equaliser interpolated around it. `test_v22rx` now
  checks that the equaliser stays near its centre spike on a clean channel
  (wrong sign: other taps sum to about 1.5; right sign: 0.28).
- The four-point slicer rotated by the wrong angle (decision regions 45 degrees
  off). Caught by the loopback test.
- `test_v14` and its implementation were written in one step (test not seen
  failing first).

Spec readings chosen (flagged):
- "Scrambled binary 1 for 270 ms" is taken as: carrier classified as scrambled
  data for 270 ms, a steady descrambled run of >= 48 bits, and a converged
  constellation.
- Timers counted from "circuit 112 ON" are back-dated 13 ms for our detection
  lag of the end of S1; measured switch to 2400 is 607.8 ms after the caller's
  S1 ended on the line (spec 600 +/- 10 plus line delay).
- After V.8 selects V.22bis the answerer starts the V.22bis answer sequence
  (unscrambled ones) after the 75 ms silence; with a pre-V.8 caller it starts
  after ANSam times out, and falls back to V.21 after 8 s without a carrier.

## Track B - B5 step 1: V.90 Phase 2 (probing / ranging), digital side: DONE in simulation

Test output (host, `make test`): `info: 131 checks, 0 failures`, `dpsk: 277
checks, 0 failures`, `probe: 121 checks, 0 failures`, `p2: 106 checks, 0
failures`. Not yet run against any real modem or independent implementation
(none of the open test peers implements V.90 Phase 2 end to end).

- `modem/info.{h,c}`: INFO0d / INFO0a / INFO1d / INFO1a (V.90 form and V.34
  form) build and parse, CRC.
- `modem/dpsk.{h,c}`: tones A and B with phase reversals placeable on any
  sample, and 600 bit/s DPSK for the INFO sequences, sharing one shaped
  transmitter (RRC 75 % step response); receiver with three symbol-clock
  phases and CRC-arbitrated framing, tone and reversal detection with fixed,
  tested latency (`DPSK_TX_DELAY` 33, `DPSK_RX_REV_DELAY` 84 samples).
- `modem/probe.{h,c}`: L1 / L2 generator (Table 17/V.34 phases) and a 160-sample
  block-DFT analyser: per-tone level, per-tone SNR, noise in the four gaps,
  frequency offset of the 1050 Hz tone.
- `modem/p2d.{h,c}`: the digital modem's Phase 2 procedure (V.90 9.2.1) with
  the 40 ms turn-rounds, round-trip delay estimate, probing, INFO1d built from
  the measurements. `sim/p2a.{h,c}` is the analogue-side test peer (9.2.2).
- `test_p2` runs both ends over delay / loss / noise / echo: both finish, the
  round-trip delay is measured within 1 ms at both ends (which checks the other
  end's 40 ms turn-round), INFO contents cross intact, per-tone SNR matches the
  channel, a noisier line projects a lower rate, V.34 selection is reported,
  and lost INFO0a / INFO0d are recovered (9.2.1.2.1).

Spec readings chosen (flagged):
- CRC (Figure 14/V.34 is a drawing): read as the bit-reversed CCITT register
  (feedback into cells 15, 10, 3, shifting towards bit 0, preset ones, no
  inversion, bit 0 sent first). It is the catalogued CRC-16/MCRF4XX
  ("123456789" -> 0x6F91, used as the test vector) and makes a receiver that
  runs the register over information + CRC end at zero.
- Figure 13/V.34 (INFO spectrum template) is unreadable in the text; the V.22
  75 % root-raised-cosine pulse is used (-3.3 dB at 300 Hz, -24 dB at 550 Hz
  from the carrier).
- "Tone detected" = 20 ms of unmodulated carrier. A phase reversal is reported
  only after 30 ms of tone before it and 6 ms of steady tone after it, so the
  reversals inside INFO sequences are not taken for tone reversals.
- 9.2.1.2.4 counts 900 ms from the second tone A reversal; ours also allows for
  the 640 ms we spend receiving L1 / L2 before listening again.
- INFO1d: only the offered upstream symbol rates (default 3000 and 3200) get a
  non-zero projected rate; rate = (mean in-band SNR - 9 dB) / 3.01 dB bits per
  symbol, capped at 12 x 2400; pre-emphasis index 0, no power reduction, no MD.

Limits (honest list):
- Recovery 9.2.1.2.5 and 9.2.1.2.6 (retrain, INFOMARKS) end in failure rather
  than a retrain. 9.2.1.2.1 to 9.2.1.2.4 are implemented; .1 is tested.
- If the analogue modem asks for V.34 in INFO1a, that is reported but there is
  no V.34 transmitter yet, although INFO0d advertises 3000 and 3200 transmit.
- L1 (the +6 dB segment) is not used to look for non-linearity; the local echo
  of our own L2 is not analysed.

## Track B - B5 step 2: upstream receiver core, echo canceller, V.8 hand-over: DONE in simulation

Host test output (`make test`, ALL TESTS PASS; ARM run recorded below when it
finishes):

- `modem/qamrx` (test_qamrx): clean line, training mse -45.2 dB at 3200 and
  -43.4 dB at 3000, slicer SNR 51.6 / 52.1 dB; tilted line at 32 dB noise:
  32.3 dB at the slicer; +-100 ppm clock and +-5 Hz carrier offset at 36 dB:
  36.7 / 36.8 dB, carrier loop reads 4.79 / -4.82 Hz, tap centroid stays at
  36.0. Convergence checked over 12 sub-sample alignments.
- `modem/ec` (test_ec): 63 dB echo return loss enhancement after 2040 samples
  of training at full step; frozen and slow modes under double talk.
- `modem/v34train` (test_v34train): PP, S / S-bar, 4-point TRN with both
  scrambler polynomials; S detector finds the S to S-bar boundary within 1
  sample; equaliser after PP + 512 TRN: -43 dB (3200 clean), -27 dB (3000
  clean), -21 dB (28 dB line with tilt), then 2000 decision-directed symbols
  with 0 errors in each case.
- `modem/answer` with `ANS_MODE_V90` (test_answer_v90, 22 checks): a V.90
  caller (V.8 CM with the PCM analogue category) gets JM with PCM digital,
  Phase 2 starts 2.91 s into the call and completes at both ends at 4.87 s;
  a caller without PCM still gets V.22bis; with V.90 disabled no PCM is
  offered.

Spec readings and decisions:
- V.8 6.3 ties PCM operation to V.34 in the modulation octets, so with V.90
  enabled JM carries the V.34 bit. We have no V.34 data mode, so JM drops the
  V.34 bit again when the caller did not ask for PCM (otherwise a plain V.34
  caller would start a Phase 2 we cannot finish instead of falling back to
  V.22bis). If a V.90 caller then picks V.34 in INFO1a, the call fails. That
  is a known gap, not spec behaviour.
- PP is periodic (48 symbols), so an equaliser trained only on it is not
  unique; the test accepts -18 dB after the minimum TRN and requires
  error-free decision-directed operation afterwards.

Process notes (honest record): `qamrx.c` was written before its test, against
the TDD rule; its tests were then written to check loop and equaliser state
and found four real faults (see DESIGN.md 7). Case 4 of test_answer_v90 was
added together with its fix.

Limits: nothing after Phase 2 is joined to the answerer yet (`ANS_P3` is
silence). No real modem has been tried.

## Track B - B5 step 3: V.90 Phases 3 and 4 and data mode: DONE in simulation (ideal downstream)

`modem/p34d` runs 9.3.1 and 9.4.1 on top of `v90/` (transmit side, own
sub-project with 13,800 checks on host and ARM) and `qamrx` + `v34/`
(receive side). `modem/answer` hands over from Phase 2 and exposes the modem
at the G.711 interface (`answer_tick_pcm`: one codeword in, one out).

Host test output:

```
test_p34 (digital modem against the analogue test peer), 109 checks, 0 failures
  no DIL, clean:                     data at 1.80 s, down 50666 bit/s (314754 bits, 0 bad), up 31200 bit/s (193518 bits, 0 bad), up SNR 48.1 dB
  DIL, echo -20 dB, 38 dB, +60 ppm:  data at 2.35 s, down 50666 bit/s (388436 bits, 0 bad), up 24000 bit/s (183660 bits, 0 bad), up SNR 32.0 dB
  3000, SCR, shaping, A-law:         data at 2.49 s, down 49333 bit/s (371110 bits, 0 bad), up 21600 bit/s (162201 bits, 0 bad), up SNR 31.1 dB
  24 dB line:                        data at 2.49 s, down 49333 bit/s (371110 bits, 0 bad), up 14400 bit/s (108134 bits, 0 bad), up SNR 23.8 dB
test_answer_v90 scenario 5 (V.8 -> Phase 2 -> 3 -> 4 -> data, upstream through the codec's mu-law A/D)
  full call connected at 6.83 s: 50666 bit/s down, 28800 bit/s up; down 465424 bits 0 bad, up 264168 bits 0 bad
test_sres (re-acquisition on the second S after seconds of silence), 56 checks, 0 failures
```

Times are from the end of Phase 2. The peer checks every signal we send
(Sd, TRN1d, Jd, Jd', the DIL it asked for octet by octet, Ri, TRN2d, MP, Ed,
B1d) and its own timing follows 9.3.2 / 9.4.2.

What this does and does not show:
- Upstream is a real signal path: V.34 modulator with clock offset, noise,
  hybrid echo of our PCM signal, re-acquisition after the caller's silences.
- DOWNSTREAM IS IDEAL. The test peer reads the octets we send; nothing here
  demonstrates that an analogue modem can equalise our signal on a real line,
  nor that the constellations a real modem asks for behave. The downstream
  rates above are simply what the peer requested (K = 32 bits per frame).
  Only a real V.90 modem (or D-Modem, B7) can show the rest.
- No independent V.90 implementation was in the loop: transmitter and
  reference decoder were written from the same reading of the text.

Spec readings chosen here (the transmit-side ones are listed in `v90/v90_tx.h`):
- Ja follows TRN without a marker. We decode differentially from the 513th
  TRN symbol and hunt for the frame sync; the first DIL descriptor is lost to
  descrambler start-up and the second one is taken.
- 9.3.1.5 "detects S": S believed after 24 symbols with the carrier line
  present and the points in the upper half plane; Jd' follows the Jd in
  progress.
- After S-bar the caller's carrier phase is taken as arbitrary and its
  timing as drifted; both are re-acquired on S with the equaliser frozen.
- E is detected as 20 ones once a CP has been seen (start bits break every
  run inside a sequence at 17); B1 starts with the next whole symbol.
- Upstream rate: one bit per symbol for each 3 dB of slicer SNR above 9 dB
  (measured on CPt), capped by MP rate masks. Trellis 16 states, no
  precoding, no non-linear encoder, minimum shaping, 4-point CP.

Not implemented: retrain (9.5), rate renegotiation (9.6), cleardown, MD,
V.34 as the fall-back when a V.90 caller selects it in INFO1a, 3429
symbols/s, precoder coefficients (MP type 1), 16-point CP. Any of these from
the far end ends the call.

## Faults found by the call-level tests and fixed

| Fault | Fix | Test |
|---|---|---|
| PPP: every retransmitted Configure-Request got a new identifier, so on a 300 bit/s line no Ack ever matched | retransmissions keep the identifier (RFC 1661 5.1) | ppp test_fsm |
| V.21 fall-back took the hybrid echo of its own 1650 Hz channel for a caller on a silent line | 81-tap receive band-pass (-50 dB at 1650 Hz) before the low-channel demodulator | test_answer "silent line with echo" |
| V.22bis receiver left diverged for good by one noise burst in about a hundred | burst gate: no adaptation on symbols whose error is far above the long-term error | test_v22bis "bursts" (165 bursts) |

Open: LAPM T401 default is short for 2400 bit/s (the call glue raises it).
The V.22bis start-up in noise was closed on 2026-11-15.

## Track B - B5 step 4: faults found by long calls, and the echo delay: DONE in simulation

**Upstream error bursts (found by the `dce` tests, 2026-11-14).** After 10 to
20 s of data the V.34 receiver made a burst of bit errors every so often, on a
clean line. Two causes, both in `modem/qamrx`:

1. The equaliser update was `tap += (e * x) >> mu`: a shift rounds down, so
   every one of the 144 tap halves lost half an LSB per symbol, 0.14 of full
   scale in 90 s. The receiver error rose from -48 dB to -34 dB during a call.
   Now rounded. The same truncation was in the V.22bis equaliser (6 % of full
   scale per hour) and the echo canceller; fixed there too.
2. The carrier and timing loops scaled their error by the size of each
   decided point. Fine for the 4-point training signals, wrong for data:
   small inner points were given the weight of outer ones. In data mode the
   loops now scale by the mean power (`QRX_DENSE`).

Also: the upstream rate is now chosen from the receiver error at the end of
TRN2d, not at the moment CPt arrives (the loops are still settling from the
re-acquisition then; it cost one or two rate steps).

Test output after the fixes:

```
test_p34 scenario 5, 90 s on a 50 dB line with echo and +30 ppm:
  down 50666 bit/s (4469408 bits, 0 bad), up 31200 bit/s (2751996 bits, 0 bad)
  worst receiver error in data mode -48.2 dB        (before: -22.7 dB by the old measure)
tools/ber_v90 (whole call through the mu-law A/D), 90 s each, upstream bits / bad:
  noise -55 dBFS, echo 0.2:  2188758 / 0   at 26400     (before: 742 bad of 1.4 M)
  noise -60 dBFS, echo 0.1:  2387736 / 0   at 28800     (before: 1579 bad)
  no noise, no echo:         2395368 / 0   at 28800     (before: 113 bad)
  noise -48 dBFS, echo 0.3:  1996140 / 0   at 24000
  noise -42 dBFS, echo 0.3:  1591824 / 0   at 19200
```

**Echo that comes back late.** The echo canceller covers 12 ms. A line driver
that works in blocks (and the SIP rig even more) returns our echo tens of
milliseconds late; with the reference at the wrong distance the canceller did
nothing and the call failed in Phase 3. `p34d` now keeps 256 ms of what it
sent and gives the canceller the samples from `echo_delay` ago. If the delay
is not known (the daemon's default), it is measured in every call: during
TRN1d, with the caller silent, 256 received samples are correlated with the
transmitted ones at every distance up to 160 ms (4 distances per sample, so
no sample costs much), and TRN1d is lengthened by the 0.23 s this takes.

```
test_p34: echo -12 dB, 25 ms late (delay given):   up 31200 bit/s, 0 bad
          echo 88 ms late, measured:               delay found 701 samples (echo path starts at 709), up 31200, 0 bad
          no echo, measured / echo at once, measured: up 31200, 0 bad
test_srv: V.90 call, echo 60 ms late, nobody told the modem: 50666 down, 28800 up
```

Not covered: an echo later than 160 ms; an echo whose delay changes during a
call (a driver that slips). Phase 2 with a late echo is only tested up to
60 ms.

## Track B - B6 and Track C - C1: a V.90 call carries V.42 and PPP: DONE in simulation

`dce/` (one answered call: modem + V.42 + PPP) now runs at the G.711
interface (`dce_tick_pcm`). `make -C dce test`: 547 checks, 0 failures.

```
(h) V.8 + V.90 + LAPM + V.42bis + CHAP + IPCP
    modem connected 7.09 s after answering at 50666 bit/s down, 26400 bit/s up; IP up 0.40 s later
    incompressible, both ways at once: down 5829 octets/s (92%)   up 2972 octets/s (90%)
    text (V.42bis at work), both ways: down 35932 octets/s (567%)   up 19935 octets/s (604%)
(i) V.90 caller without V.42 (start-stop): down 4975 octets/s, up 2577 octets/s
(n) A-law codec: 50666 / 24000, IP up after 8.28 s
(l) V.22bis, then V.90, then V.22bis on the same object: the V.90 call is bit-identical to a first call
```

Open findings from `make -C dce test-findings` (each a failing check):
1. LAPM default T401 (1 s) is shorter than an acknowledgement takes at 2400
   bit/s; `dce` raises it to fit the line rate. The library default stays.
2. PPP default Restart timer (3 s) at 300 bit/s; same treatment.
5. V.22bis start-up fails in steady noise of -40 dBFS (about 19 dB SNR)
   instead of settling for 1200 bit/s. Fixed 2026-11-15; the cause was not
   the one written down at first (see "V.22bis: noise start-up, retrain,
   guard tone").
(3, 4 and 6 are closed: V.21 false connect on echo, V.22bis burst recovery,
V.90 upstream bursts.)

## Telephony layer (`tel/`): DONE in simulation

What an exchange does for the line, as a dialling modem sees it: dial tone,
DTMF and pulse digits, number check, post-dial delay, ringing tone, answer;
busy tone; the three information tones and congestion tone for a wrong
number; time-outs; and after the answer the ways a call ends by our doing:
random line drop, session limit, idle timeout. US and European tone plans.
`make -C tel test`: 377 checks at first, 380 now, 0 failures, host and ARM.

```
DTMF receiver: all 16 keys at 3 levels (down to about -39 dBm0), +-6 dB twist, +-1.5 % detune, 50 ms on / 50 ms off: all right
               nothing from dial tone, single tones, a row pair, noise, answer tone; a 20 ms blip is no digit
busy_chance 30: 29 of 100 calls busy
drop_mean 1 min: mean call 65.5 s over 200 calls (same chance every second)
```

Choices made (no standard governs a private exchange): a complete number
connects at once and later digits are ignored; a number that cannot become
ours gets the information tones 2 s after the last digit; "any number" mode
takes a 3 s pause or `#` as the end; a hook break of 10 to 150 ms is a dial
pulse, 250 ms is a hang-up; a line drop is abrupt silence (no PPP or LAPM
farewell), followed after 1 s by busy tone.

## The server and the daemon (`srv/`), Track C - C2 and C3: DONE on Linux, in a network namespace

- `srv.{h,c}`: telephony + call + configuration + status in one object,
  `srv_tick(hook, codeword) -> codeword`. Portable, tested on host and ARM
  against a whole simulated caller (dialler, modem, V.42, PPP client).
- `main.c`, `line.c`, `tun.c`, `nat.c`: the Linux program `v90d`. IP goes
  through a TUN device; forwarding and NAT are the kernel's, set up by the
  daemon itself: `nat.c` speaks nftables netlink directly, so the image needs
  no `iptables` or `nft` program (they would not be on the allowed list).
- Settings page (`cfg/settings`, `cfg/httpd`): phone number, tones, rings,
  busy chance, mean time to a line drop, session limit, idle timeout, PPP
  authentication and users, modulations, rate limits, optional admin password.
  Saved to the config file, in force from the next call. A save during a
  call is written after it (a flash write must not stall the sample loop).

`make -C srv test` (108 checks, 0 failures):
```
V.22bis call: IP up 6.46 s after the answer
line dropped 93 s into the call          (drop_mean = 1 min)
V.90 call: IP up 7.66 s after the answer, 50666 bit/s down, 28800 bit/s up
V.90 call, echo 60 ms late: 50666 bit/s down, 28800 bit/s up
```
plus: busy, wrong number, abandoned call, wrong password, settings changed
between calls, session limit (orderly close after 60 s), idle timeout, the
status page and the call history after each.

`make -C srv test-ns` runs the real `v90d` binary under `unshare -Urn` with a
socket pair as its telephone line (38 checks, 0 failures):
```
v90d info: ppp0 up: 10.64.64.1 -> 10.64.64.2
v90d info: NAT: 10.64.64.2 is masqueraded
v90d info: status and settings on http://127.0.0.1:8080/
  V.90 call through the daemon: IP up after 7.75 s, 50666 bit/s down, 28800 bit/s up
v90d info: settings accepted; the file is written when the line is free
v90d info: call to "5551234": answered; V.90 50666/28800 bit/s, 12 s
v90d info: settings saved to build/ns_v90d.conf; they apply from the next call
v90d info: call to "5551234": wrong number; no carrier 0/0 bit/s, 0 s
  V.22bis call to the new number: IP up after 6.31 s
v90d info: call to "42": answered; V.22bis 2400/2400 bit/s, 9 s
```
In that run the caller pings our end of the link (the kernel answers through
the TUN device), pings a host behind a second TUN device standing in for the
Internet (the packet arrives there with the box's address, not the caller's,
and the reply finds its way back to the caller), reads the status page, and
posts the settings form to change the number.

Not done here: the line back end for the real SLIC (Track A); `lan = static`
and DHCP are left to the boot script (`v90d -p` prints the LAN settings for
it); nothing has run on the box.

## V.90 on the real DVF9918: bit-exact, 32 % of the CPU in data mode

`make bench-v90` records a simulated V.90 call on the host (24 s, the line
codewords in each direction) and `rig/bench_v90` replays the received side
through the answering modem, timing every 10 ms block. Pushed to /tmp on the
stock box (RAM only, nothing flashed) and run there:

```
replayed 24.0 s; transmit hash 2623a01a = host; connected 1, 50666 bit/s down, 26400 bit/s up; bits from line 446358 (0 bad)
  V.8 (ANSam, CM/JM)              2.9 s of line:   3.2 % of the CPU, worst 10 ms block took  1.74 ms
  Phase 2 (probing)               2.0 s of line:   7.8 % of the CPU, worst 10 ms block took  2.26 ms
  Phases 3 and 4 (training)       2.2 s of line:  18.4 % of the CPU, worst 10 ms block took  5.37 ms
  data mode                      16.9 s of line:  28.8 % of the CPU, worst 10 ms block took  8.81 ms
```

- The transmitted codewords are identical to the x86 run (same hash), so the
  fixed-point path gives the same answer on the target as on the host.
- The whole modem fits in under a third of the CPU. V.42 and PPP are not in
  this figure (they are small next to the modem).
- The worst block, 8.81 ms of a 10 ms budget, was measured with the stock
  firmware's own processes running beside it and without real-time priority.
  It is a wall-clock figure, so it includes being scheduled out. On our own
  firmware the daemon should run SCHED_FIFO; to be measured again there.

Run again after renegotiation, cleardown and retrain went in (the S and
Tone A watches run on every sample of data mode): same hash, so a call
without those events is unchanged bit for bit, and

```
  V.8 (ANSam, CM/JM)              2.9 s of line:   3.2 % of the CPU, worst 10 ms block took  2.82 ms
  Phase 2 (probing)               2.0 s of line:   8.0 % of the CPU, worst 10 ms block took  2.53 ms
  Phases 3 and 4 (training)       2.2 s of line:  19.2 % of the CPU, worst 10 ms block took  7.34 ms
  data mode                      16.9 s of line:  32.1 % of the CPU, worst 10 ms block took  8.36 ms
```

The two watches cost 3.3 % of the CPU. Not optimised.

## The rig carries PPP (`rig/sipmodem -m ppp`)

`rig/pppcall.c` puts the dce (V.90 or V.22bis, V.42, PPP server) behind the
SIP/RTP rig, so a real modem on the stock box's FXS port can dial in and get
an IP session before any of our firmware is flashed. `srv/test_rig`: 26 checks.
The runbook is in rig/README.md. Not yet run against a real modem.

## Firmware userland (`fw/`)

`rcS` (busybox init: mounts, LAN from `v90d -p`, udhcpc, ntpd, respawn of
v90d), `udhcpc.script`, the shipped `v90d.conf`. All written here. The line
back end is the placeholder `unix:/var/run/slic.sock` until the SLIC driver
exists (Track A).

## Other

- cfg: /settings and /config refuse a request whose Host header is not an
  address when no admin password is set (DNS rebinding). cfg now runs 2941
  checks on the host, all pass on ARM.
- v34/ library complete on host and ARM. Not implemented: the nonlinear
  encoder. Estimated cost of the 16-state decoder 3.5 to 4.6 % CPU.
- ARM rerun after the rounding and echo-delay changes: p34 251, answer_v90 32,
  qamrx 18, ec 504 checks, 0 failures.

## V.90 in data mode: rate renegotiation, cleardown, retrain: DONE in simulation

V.90 9.5 to 9.7, digital side, in `modem/p34d.c`, `p2d.c`, `answer.c`, and
carried through `dce/`.

- **Rate renegotiation (9.6)**, either side starting it. The caller's S is
  watched for in data mode (the tone-pair detector of Phase 3, on the echo
  cancelled signal); after its S-bar we send Rd for 384 T and Rd-bar for 24 T
  from a data frame boundary, 36 ms of TRN2d, MP, MP' and Ed, B1d. A new CP
  takes effect with B1d. `p34d_renegotiate()` starts one from our side.
- **The caller's request for silence (CPs, 9.6.1.2.4 to .6, Figure 10)**: MP'
  until CPs', Ed, silence in data frame alignment until a CP without the
  request, then Rt, Rt-bar, MP.
- **Cleardown (9.7)**: a CP with drn = 0 ends the call as "caller cleared
  down" (the status page shows the caller hung up). `p34d_cleardown()` sends
  ours: a renegotiation whose MP has drn = 0, four of them, then silence. The
  dce does that at the end of every orderly ending (wrong password, session
  limit, idle timeout, hang-up), after the LAPM release.
- **Retrain (9.5)**, either side. Tone A (2400 Hz for more than 50 ms, a
  10 ms block detector) in data mode or in a renegotiation, or our own wish
  (`answer_retrain()`), ends p34d in `P34_RETRAIN`; `answer` sends 70 ms of
  silence, then Phase 2 again from Tone B (`p2d_init_retrain()`: no INFO0
  exchange, 9.2.1.1.3) and Phases 3 and 4 from scratch. A renegotiation that
  does not complete in 6 s turns into a retrain (9.6.1). While the modem
  retrains, the dce holds V.42 and PPP as they are, timers included.

Test output (`build/test_p34`, the caller is `sim/p34a` extended to do all of
this from its side):

```
caller renegotiates, K 32 -> 30: ... down 48000 bit/s (310510 bits, 0 bad), up 31200 bit/s (196794 bits, 0 bad)
  back in data mode 382 ms after the request; peer saw Rd 64 + 4 units (0 bad); up after: 144066 bits, 0 bad
caller renegotiates, SCR first: state 11 err 0 ... 0 bad
caller clears down: state FAILED err 10 (cleardown); call ended 152 ms after the request
we renegotiate: back in data mode 347 ms after the request
we clear down: call ended 102 ms after the request (peer saw the MP with drn = 0)
caller renegotiates with silence: down 49333 bit/s (296255 bits, 0 bad)
  back in data mode 798 ms after the request; silence 1902 octets (0 not silent), Rt 64 + 4 units
renegotiation stalls: state RETRAIN
p34: 334 checks, 0 failures
```

Downstream no user bit is lost or invented across a renegotiation (the bit
stream is checked end to end across it). Upstream the bits in flight when S
begins are lost, as on any modem; V.42 resends them.

Whole calls (`dce/build/test_call k`, V.8, V.90, V.42, PPP):

```
dce_hangup: ended after 233 ms: local hangup          (the caller's modem saw the cleardown)
caller's modem clears down: ended after 149 ms: caller cleared down
caller renegotiates: 50666 -> 48000 bit/s down, state 2   (traffic goes on)
caller retrains: data again after 4279 ms, state 2, 50666 bit/s down, 26400 up   (traffic goes on)
we retrain: data again after 4302 ms                       (traffic goes on)
call: 95 checks, 0 failures
```

The retrain tests were written after the code and passed at once, so they
were mutation-checked: with the Tone A detector disabled, "caller retrains"
fails in 5 checks.

Spec ambiguities and the reading chosen:
- 8.5.2 zeroes the caller's scrambler and differential encoder "prior to the
  first CPt". Nothing is said for the CP of a renegotiation. Reading: they
  carry on. Our receiver does not depend on it (the descrambler finds its
  way in 23 bits and CP repeats).
- 8.6.5 zeroes our scrambler before TRN2d. In the silence procedure MP
  follows Rt-bar with no TRN2d. Reading: the scrambler carries on.
- TRN2d in a renegotiation is optional (up to 2000 ms). We send 36 ms.
- 9.7 does not say what the modem that receives drn = 0 does. Reading: it
  stops at once (finishing a fixed-length signal in progress).

- **Lowering the upstream rate by ourselves.** In data mode the receiver's
  error is compared with what the rate needs (the rule used at TRN2d, 9 dB +
  3 dB per bit per symbol, less 4.5 dB: with 1.5 dB a working A-law call
  at 24000 bit/s was taken down to 16800 for nothing). Above it for a second: a
  renegotiation whose MP carries a lower ceiling, taken from the error now
  with 3 dB more margin (wrong decisions hide noise). No lower rate left, or
  an error above -9 dB for a second: retrain.
- **A limit on retrains**: the fourth in a row ends the call (one is forgotten
  per minute of data mode).

```
line drops to 28 dB in mid-call: ... up 16800 bit/s
  rate lowered 1 time(s); after the last: 144858 bits, 0 bad
line drops to 3 dB in mid-call: state RETRAIN
  retrain asked for 11.6 s after the line went bad
p34: 348 checks, 0 failures
fourth retrain in a row: ended after 140 ms: carrier lost
call (k): 113 checks, 0 failures
```

`make check`, `make check-ns` and `make check-arm` (qemu) all pass on this
code: ARM p34 348 checks, answer_v90 32, dce call (quick subset) 295, srv 108,
rig 26, 0 failures, "ALL ARM CHECKS PASS".

The 11.6 s on a dead line is one second to notice, a renegotiation that gets
as far as Ed, and the 6 s limit of 9.6.1. Not done: raising the upstream rate
again when the line recovers (the caller can ask with a renegotiation of its
own; we never do).

## Real-modem rig armed on the box (2026-11-14, evening)

FXS port 1 now points at the rig: P47 = 127.0.0.1:5070, no registration,
account active, off-hook auto-dial, PCMU only, 20 ms, VAD off, LEC and echo
suppressor off, fax pass-through, fixed low jitter buffer, gains 0 dB. Old
values in `recon/out/port1_before.txt`. The box was rebooted to load them and
`/tmp/sipmodem -p 5070 -r 40000 -m ppp -M v90` is running (log `/tmp/rig.log`,
RAM only: a reboot stops it). Waiting for a call from the USR modem. No call
has been made yet.

## Track A: our own kernel, line driver and daemon run on the unit, from RAM (2026-11-14, night)

Nothing was flashed. All of it was started from RAM with `fw/tools/ramboot.sh`
(HARDWARE.md, "Our own kernel on the unit").

What ran, with the output it gave:
- Linux 6.6.63 with our board support (`fw/kernel/`: interrupt controller,
  timer, serial, GPIO, clock and reset unit, watchdog, line driver): boots,
  Ethernet at 100 Mb, DHCP, 21 MB free.
- `v90line` (fw/kernel/drivers/misc/v90line + fw/drv): kernel log
  `PCM highway running, bit clock = PLL3 / 708` and
  `SLIC id b2 up, battery 58730 mV, mu-law`. Its counters after 257 s:
  frames = 8000 per second of kernel time exactly, `tdm_underruns 13`,
  `tdm_overruns 12` (all during the first seconds of boot, flat afterwards),
  `spi_errors 0`.
- The daemon on `dev:/dev/v90line` on the unit:
  `ppp0 up: 10.64.64.1 -> 10.64.64.2`, `NAT: 10.64.64.2 is masqueraded`,
  `ready on line dev:/dev/v90line; number "5551234"`, status page answers on
  http://192.168.1.172/. With the daemon running `pump_underruns`,
  `pump_overruns` and `stale_writes` stay constant: every block is answered
  in time.
- Watchdog: fed by the kernel driver, unit up for more than 250 s (the
  period is 100 s). The first version of the driver reset the unit after
  seconds; cause and fix in HARDWARE.md.
- Host tests of the driver's portable part: `make -C fw/drv test test-arm`.

Not shown yet:
- A telephone or modem going off hook on our driver (nothing was lifted).
- Any audio beyond the open-line echo test done by hand earlier.
- The image is a bring-up build (telnet root shell, /dev/mem tools).

## Track A: the unit starts our firmware from flash, no serial cable (2026-11-14 / 15)

The user's rule since this stage: no UART at all. Design in DESIGN.md
("Starting from flash without a serial cable"). Stock U-Boot and the stock
set that boots (B) were not touched.

Shown on the unit, in this order:
- Stage 1, environment only: written from the stock firmware with `v90nv`,
  trial reduced to `saveenv`. After the reboot the stock firmware was back,
  `v90d_tries` read 2, `bootcmd` intact, and only `nvram`, `lang2` (stock
  writes both at every boot) and the environment differed from before.
- Stage 2, image: `flashcp -v` of `build/v90d.uimage` (2111342 octets) to
  `v90d-image` from our RAM-booted kernel: rc 0, read back twice with md5
  ed21c2d4... equal to the host file. 11 of 8245 page programs had to be
  repeated after a controller reset (HARDWARE.md, lost page programs); an
  earlier driver without that recovery failed the write.
- Partition check after all writes: boot loaders, `factory`, stock copy B,
  `prog`, `lang`, `oem` unchanged; changed are only our image and settings
  areas, the environment, `nvram` and `lang2`.
- First boot from flash: telnet of our system answered about 14 s after
  `reboot`; kernel log over netconsole shows our kernel, `EN25QX128A 16 MiB,
  QPI`, `SLIC id b2 up, battery 58730 mV`, daemon started, and at 64.8 s
  `rcS: start confirmed to U-Boot`. `v90d_tries` read 2 at 20 s and 3 after
  the confirmation. Status page: http 200.
- The way back, shown: three reboots each before the confirmation gave
  `v90d_tries` 2, 1, 0 with our system up each time; the fourth reboot
  started the stock firmware by itself. From its root shell
  `v90nv env-set /dev/mtd13 v90d_tries 3` and a reboot started ours again.
- Settings in flash: `rings` set to 2 on the web page, reboot, kernel log
  `rcS: settings from flash`, the page shows 2. (Put back to 1.)
- Line driver counters with the daemon running, 10 s apart: frames +80328
  (8000 per second), every under/overrun counter unchanged.

Not shown:
- A power cut instead of `reboot` (expected to behave the same: the counter
  is in flash before the kernel starts).
- A kernel that hangs. The counter path was exercised with early reboots,
  not with a hung image; a hung image needs a power cycle per try, because
  nothing shows that the watchdog runs between U-Boot and our driver.
- The image in flash is still the bring-up build (telnet root shell,
  /dev/mem tools, netconsole to 192.168.1.45).

## The web interface: Nestaliga, a router-style dashboard with a login (2026-11-15)

Asked for by the user in steps: the layout of the stock interface under the
name Nestaliga with a design of its own, then "a proper router dashboard"
with the navigation on top, a login page, Ethernet settings.

- `cfg/page.h`: the shared frame and style sheet, written here; nothing was
  taken from the stock pages. No script and nothing fetched (the server's
  content security policy forbids both); light and dark follow the browser.
- Dashboard (`/`): line state as a badge with one line about the call, tiles
  (downstream, upstream and signal to noise with bars, uptime, calls), cards
  for line, PPP, counters, last calls. Reloads every 5 s.
- Login (`/login`, `/logout`): with an admin password in the configuration
  every page needs a session (cookie, HttpOnly, SameSite=Strict, 15 minutes
  after its last use, one at a time) or HTTP Basic credentials (scripts). A
  wrong login holds further tries for a second. The image ships with
  `admin_password = admin`, user name admin, and the settings page says so
  until it is changed. Without a password in the configuration the pages
  are open, as before.
- Settings on five pages, each a tab with its own form: Line (`/line`),
  Modem (`/modem`), Users (`/users`), Network (`/network`), System
  (`/system`: admin password, Restart). Saving leads back to the page it
  came from. `/settings` (all on one page) and `/config` (the configuration
  as text) still answer, for scripts, but no tab leads there.
- Network: DHCP or static, address, netmask, gateway, DNS servers given to
  the caller; used from the next restart on. Restart button
  (`POST /restart`). New configuration key `[network] gateway`:
  static mode had no default route before, so a static box could not have
  carried the caller's traffic anywhere.
- Rescue address: the image always answers on 169.254.90.1 as well
  (`fw/rcS`), so a wrong Ethernet setting locks nobody out; the settings
  page says so. The server listens on every address of the LAN interface.
- Restart: the daemon only leaves a note; `fw/rcS` puts the settings into
  flash, confirms the start to U-Boot (three quick restarts would otherwise
  end in the stock firmware: seen, the counter read 1) and restarts.
- `/cgi-bin/login` answers 302 to `/`: browsers that had visited the stock
  firmware remembered its permanent redirect there.

Test output: `make -C cfg test` and `test-arm`: test_config 392, test_status
416, test_settings 542, test_httpd 1935 checks, 0 failures on both.
On the unit (image b58225ba... in flash): `/` without login 303 to /login,
`/status.json` 401, wrong login 401, right login 303, dashboard and all six
settings sections served, logout ends the session, `eth0:9` has
169.254.90.1, the server listens on 0.0.0.0:80 tied to eth0, Restart
restarted the box (uptime 13 s after it).
Later image (c905e5cd...): all six pages 200 after login, saving on /line
answers 303 to /line?saved=1, and a Restart 17 s after boot kept the changed
setting (`rcS: settings from flash`, rings = 2) and left `v90d_tries` at 2
after the restart (so it had been put back to 3 first).
Not shown: reaching the box on 169.254.90.1 from another machine; a static
address actually applied on the unit.

## V.22bis: noise start-up, retrain, guard tone (2026-11-15)

Finding 5 had the wrong cause on record. The answerer did not starve waiting
for 32 scrambled ones: the receiver's gain control had settled on the line
noise before the caller's signal arrived and never recovered. Now a level
step of 9 dB or more restarts the receiver, the gain has a floor (16), and
the error squares are clamped. The answerer measures the signal to noise
ratio in band during S1 and does not return S1 below 18 dB, so the call
comes up at 1200 bit/s by itself.

Retrain (V.22bis 6.4) works in both roles: `v22m_retrain()` asks for one, a
far-end request is followed, and the modem asks by itself when the long-term
error (`mse_long`) stays above 2000 for 1 s. A retrain that never completes
ends the call (`modem/answer.c`). Guard tone 1800 or 550 Hz behind
`v22m_set_guard()`, off by default.

Test output after the merge into the main tree: `make test` ALL TESTS PASS
(v22bis 267 checks, 0 failures). ARM and spandsp interop: see below.

Not shown: a retrain against spandsp (its caller did not complete one in
the worker's runs, so retrain is only tested against the peer written
here); rate change 6.6 (not implemented); any of it on a real modem.

## Two ports, step 1: the kernel driver (2026-11-15)

`fw/kernel/drivers/misc/v90line` now runs both FXS ports: `/dev/v90line`
(port 1) and `/dev/v90line2` (port 2), same contract, one pump and one
thread per port, the interrupt moves slot 0 and slot 1; `stats` and `stats2`
in sysfs. On the unit, image md5 f1c9b1d9..., from flash:

    v90line 5f00000.line: port 1: SLIC id b2 up, battery 58612 mV, mu-law
    v90line 5f00000.line: port 2: SLIC id b2 up, battery 58730 mV, mu-law

Highway counters unchanged over 20 s with both ports up (frames 354744 ->
514992, underruns 17, overruns 16 both times: start-up only). The daemon
still serves port 1 only. Not shown: anything off hook on either port.

## V.34 answering: in the modem, the server and the pages (2026-11-15); simulation only

A caller without V.90 now gets V.34 (2400 to 31200 bit/s; symbol rates 2400,
3000, 3200) instead of V.22bis. The modem part is described, with all its
evidence and its limits, in `V34-REPORT.md`; read its section 4 before
believing any of it on a real line: **no real modem and no independent
implementation has ever met this code**; the calling peer in every test is
built from the same blocks.

What was added on top of that report, in the main tree:
- `modem/answer`: phase `ANS_V34`, modulation `ANS_MOD_V34`. V.34 is offered
  as a data mode in JM when `V8_MOD_V34` is in the modes (the PCM flag "no
  V.34 data" is then left out); `answer_v90_line()` ceilings and echo delay
  apply; cleardown and retrain go through the same calls as V.90.
  `test/test_answer_v34`: 32 checks, 0 failures (28800 / 28800 bit/s, 86400
  bits each way without an error, our retrain, our cleardown, ceilings 14400
  / 9600 kept, V.34 without V.90, V.34 not offered).
- `srv`, `dce`: `modulations = v34`; whole call V.34 + V.42 + PPP against
  the server core, `make -C srv test`:
      V.34 call: IP up 8.62 s after the answer, 28800 bit/s down, 26400 bit/s up
      srv: 138 checks, 0 failures
  20 IP packets each way, hang-up, history entry; with v34 off in the
  settings the same caller is not served.
- `cfg`: `v34` and `v32bis` in `modulations` (six names now, fastest
  first), boxes on the Modem page, names on the dashboard and in the JSON.
  (`v32bis` answering came right after: next section.)
- Suite results for the whole tree: see "Two ports, step 2" below.

Not done: V.90 Phase 2 followed by V.34 (a V.90 modem that picks V.34 in
INFO1a still fails, report 6.7); CPU cost on the box (estimate 33 %, from
instruction counts); memory: `answer_t` grew by 27 kB.

## V.32bis answering (2026-11-15); simulation only

A caller without V.34 now gets V.32bis (4800 to 14400 bit/s, trellis coded
above 4800, with V.32's 9600 and 4800) before V.22bis. The modem is in
`v32/` (its README has the evidence and the limits). As with V.34: **no real
modem and no independent implementation has met it**; the calling peer
(`sim/v32call`) is built from the same blocks.

- `modem/answer`: phases `ANS_V32` and `ANS_V32_SIL`, modulation
  `ANS_MOD_V32BIS`. Two ways in: V.8 (the caller's CM lists V.32, JM picks
  it), and a caller without V.8 that answers our ANSam with AA (1800 Hz):
  detected during ANSam, 75 ms of silence, then the V.32 answer sequence.
  Rate ceilings from the settings limit the offered rates. Retrain goes
  through the common call; a V.32 cleardown request is refused (-1): V.32bis
  has no cleardown signal, the line is simply dropped.
  `test/test_answer_v32`: 34 checks, 0 failures.
- `srv`, `dce`: `modulations = v32bis`; whole call V.32bis + V.42 + PPP
  against the server core:
      V.32bis call: IP up 4.58 s after the answer, 14400 bit/s
- Not done: rate renegotiation during data (V.32bis section 8); the rate
  agreed at start-up is kept for the call.

## Two ports, step 2: settings, pages, daemon, and on the unit (2026-11-15)

Each port is its own dial-up line with its own number and settings.

- `cfg`: `[line2]`, `[modem2]`, `[network] ppp_peer2` (default 10.64.64.3);
  same keys and defaults as port 1; `cfg_for_port()`. Pages `/line2`,
  `/modem2`, `/port2`, `/status2.json`; the dashboard shows both ports.
  Host: test_config 486, test_status 444, test_settings 613, test_httpd
  1950 checks, 0 failures.
- `srv/main.c`: one process, one thread, two lines (`-l` twice), `poll()`
  on both, one `srv_t`, TUN device and NAT rule per port.
  `make check-ns` (the real daemon binary with two lines in a network
  namespace; port 1 stays on hook, port 1's number is a wrong number on
  port 2, a V.34 caller on port 2 gets its own address and page):
      port 2, V.34 call through the daemon: IP up after 8.75 s, 28800 bit/s down, 28800 bit/s up
      ns_call: 71 checks, 0 failures
- On the unit, image md5 7ed27590b6bf0148c1d5559d138c41a7 (2140582 octets,
  debug build; `v90d-arm` 361924 octets) written to flash and started
  without the serial cable:
  - kernel log: both SLICs up (58.8 V and 58.7 V on hook); rcS "starting the
    daemon on dev:/dev/v90line and dev:/dev/v90line2"; "start confirmed to
    U-Boot" at 64.8 s, `v90d_tries` reads 3 afterwards.
  - `ppp0` (peer 10.64.64.2) and `ppp1` (peer 10.64.64.3) exist.
  - Both lines idle: the daemon takes 1.1 % of the CPU (top, 5 s). Over a
    10 s window no counter of either port moved (`tdm_underruns`,
    `tdm_overruns`, `pump_underruns`, `pump_overruns`, `stale_writes`); the
    non-zero pump counts are from before the daemon opened the devices.
  - Pages: login 303, dashboard with "Port 1" and "Port 2" cards and the
    port switch, `/modem` and `/modem2` with six modulation boxes. V.34 and
    V.32bis were switched on for port 1 through the form ("Settings saved",
    `[modem] modulations = v90, v34, v32bis, v22bis, v22, v21` in the file
    in force, kernel log "rcS: settings kept in flash").
- Two calls at once have not been run, in simulation or on the unit. What
  is shown instead: none of the 58 shipped objects of `modem/`, `v32/`,
  `v34/`, `v90/`, `v42/`, `ppp/`, `dce/`, `tel/` and `srv/srv.c` has a
  writable global (`nm`: no data, bss or common symbol), so two `srv_t` do
  not share state.
- CPU on the unit, under our kernel with the daemon idle beside it
  (`rig/bench_v34`, `rig/bench_v90`: recorded calls replayed through the
  modem, timed per 10 ms block):
      V.34 alone, transmit hash 0793a29c (the host's), 227448 bits, 0 bad:
        Phase 2 5.3 %, Phases 3 and 4 14.2 %, data mode 24.1 %, worst block 5.41 ms
      V.90 alone, transmit hash 0541cff5 (the host's), 868758 bits, 0 bad:
        Phase 2 6.2 %, Phases 3 and 4 14.7 %, data mode 23.2 %, worst block 10.43 ms
      two V.90 replays started together: data mode 44.7 % and 44.6 % of
        line time each (wall clock, the two sharing the processor)
      V.90 with V.34 together: 28.9 % and 50.1 %
  So V.34 on the unit is bit-exact with the host and costs 24 % (the
  estimate was 33 %); V.90 costs 23 % under this kernel (32 % was measured
  under the stock system); two fast calls together need about 46 %. This is
  the modem alone: V.42, PPP and the kernel's line work come on top, and the
  replays run as two processes, not as the daemon's one loop.
  To watch in the first real call: one V.90 block took 10.43 ms (the others
  stay under 6 ms); whether that was the modem or the scheduler is not
  known. The line driver's `pump_underruns` will show it if it matters.
- Not shown on the unit: off hook, ringing or audio on either port; any
  call.

Whole tree as flashed, run on a copy of it (2026-11-15):
    make check         78 suites, 0 failures: ALL HOST CHECKS PASS
    make check-arm     78 suites, 0 failures: ALL ARM CHECKS PASS (qemu-arm)
    make check-ns      ns_call: 71 checks, 0 failures
    make test-interop  ALL INTEROP TESTS PASS (spandsp V.8 and V.22bis callers;
                       interop_v22bis_spandsp: 90 checks, 0 failures)

## First calls from a real modem (2026-11-15): every modulation carries PPP

Bench: the box's Ethernet on the workstation (shared, 10.42.90.x), a
Conexant CX93001 V.92 USB modem (`/dev/ttyACM0`) on port 1, `pppd` + `chat`
dialling 5551234 (`live/*.sh`). Recordings of port 1 (daemon option
`-w FILE`, received and sent codewords per 10 ms block) are replayed through
the same code on the PC (`live/replay`), which is how every fault below was
found. The table is port 1; port 2 is below.

Results, all with the image now in flash (md5 `d42d2f20...`, 2142830 octets):

| Forced with | Connect | Error control | PPP | Notes |
|---|---|---|---|---|
| nothing (V.90) | 52000 / 24000, also 49333 | LAPM + V.42bis | up 27 s after dialling | 10/10 pings to 1.1.1.1 through the box's NAT, 200 kB at 10.3 kB/s, 0 pump underruns |
| `AT+MS=V34,0` | 31200 / 28800 | LAPM + V.42bis | up after 21 s | 10/10 pings, 150 kB at 13.2 kB/s; the modem renegotiates to 28800 / 28800 |
| `AT+MS=V32B,0` | 14400 / 14400 | LAPM | up after 19 s | pings 250 to 700 ms |
| `AT+MS=V22B,0` | 2400 | LAPM | up after 21 s | 4/4 pings, 0.5 to 0.8 s |
| `AT+MS=V22,0` | 1200 | LAPM | up after 23 s | 4/4 pings, 0.85 to 1.5 s |
| `AT+MS=V21,0` | 300 | none (start-stop) | up after 42 s | 4/4 pings at 3.35 s; one attempt in four did not connect, not looked into |

The pings at 2400 bit/s and below only pass when the workstation's own
multicast (mDNS, SSDP, WS-Discovery: kilobytes as soon as ppp0 is up) is
kept off the link (`iptables -I OUTPUT -o ppp0 -d 224.0.0.0/4 -j DROP`);
that was the "packet loss" first seen there, not the box.

What a real modem needed that the simulation did not (each has a test now):

- **V.8 JM**: with `pcm = digital` we must also send the access category
  "digital" and the protocol octet (LAPM). Without it the Conexant chose V.34.
  `modem/answer.c`, `test/test_answer_v90.c`.
- **Line driver**: a late write shifted the whole transmit stream (echo
  canceller lost). Late samples are now dropped and the delay stays constant
  (`fw/drv/pump.c`); the driver's latency is 240 samples; the daemon runs
  SCHED_FIFO 50 with its memory locked.
- **V.90 CPt**: the Conexant's CPt has one fill bit where we demanded three
  (CRC decides now, 0 to 3 fill bits). Its real 290 bits are the fixture
  (`v90/test/test_seq.c`, `live/cpt_conexant.txt`).
- **V.42 XID**: its optional-functions field is 3 octets long; the reply must
  mirror that length (`v42/v42_lapm.c`). **V.42 ODP**: its ODP characters come
  14, 22 or 0 marks apart, not 8 to 16 (`v42/v42_detect.c`, real excerpt in the
  test). These two were the "drops after exactly 47 s".
- **V.34 non-linear encoder (9.7)**: the encoder raises the mean power by
  0.64 dB at 28800 and a real modem scales the encoded signal back to the
  nominal level (and expects the same of us): its B1 decoded with our
  receiver's scale at 0.92, not at 1. `v34_params_t.energy_tx_q8` is the mean
  energy of what is sent, computed over the shell mapper's ring
  distribution; both gains use it. This was "V.34 connects but gets no IP":
  the Conexant asks for the non-linear encoder and a precoder, we ask for
  neither, so only our transmitter was wrong.
- **V.34 MP Type 0 in a rate renegotiation** leaves the precoding
  coefficients of the first MP in force (10.1.3.9); we zeroed them.
- **Status page**: receive level (power of the last second against 0 dBm0,
  our own echo included), receive SNR (V.90, V.34, V.22bis; not V.32bis or
  V.21) and round-trip delay (V.90, V.34; includes the line driver's delay)
  were never filled. "training" is now "negotiating".
- **Lights**: the four LED lines are active low (device tree). The socket
  light follows the Ethernet link and traffic, the network light is on while
  the LAN side has an address, each telephone light is on while that port is
  off hook.

**Port 2, same modem, same image (2026-11-15):** V.90 50666 / 26400 with its
own address (10.64.64.3), 5/5 pings to 1.1.1.1, 150 kB at 10.8 kB/s, only
port 2's light on and port 1 idle on its page; then V.34 31200 / 28800,
V.32bis 14400, V.22bis 2400, V.22 1200 and V.21 300, each with PPP up and
3/3 pings. Two calls at once are still untried (one modem).

CPU on the box: daemon idle 1.1 %, V.90 data mode 23.2 %, V.34 24.1 %.

## Archive mode: the callers' web and FTP through Protoweb (2026-11-15)

Setting `archive = on` in `[network]` (Network page, "Archive mode"; shown on
the dashboard and as `"archive"` in status.json). In force when the lines are
free. What it does:

- `srv/nat.c` adds a prerouting chain to our nftables table: from each
  caller's address, UDP 53, TCP 80, TCP 21 and TCP 50000 to 50015, whatever
  the destination, are redirected to this box (ports 8353, 8380, 8321, and
  the same port). Needs `CONFIG_NFT_REDIR`, now in the kernel configuration.
- The daemon runs `arch/v90arch` as a child while the setting is on (ordinary
  priority, restarted if it dies). It answers every name question with an
  address from 198.18.0.0/15 and remembers which name got which; rewrites
  HTTP requests to the absolute form and sends them to Protoweb's proxy
  (wayback.protoweb.org:7851); and is an FTP server towards the caller that
  fetches `ftp://` listings and files from the same proxy. An unreachable
  route for 198.18.0.0/15 refuses what is not redirected (a secure site)
  at once.
- Tests: `arch/` (unit tests on x86 and ARM, an end-to-end test against a
  fake proxy, 120 checks), the redirect in `make check-ns` (a name question
  from the caller to 9.9.9.9 lands on the gateway port and the answer comes
  back from 9.9.9.9), cfg tests for the key, the form and the pages.
- **On the unit, over a real V.90 call** (image md5 `422c1c43...`, 2161254
  octets): name questions to 4.2.2.2 answered 198.18.0.1 / .2;
  `http://www.inode.com/` 200, 12249 octets in 2.6 s ("iNode Gateway"); FTP
  to ftp.inode.com: passive LIST (misc, pub, welcome.msg) and RETR
  welcome.msg (1131 octets, 226); HTTPS to an archive address refused after
  0.13 s.
- Limits: while it is on, only web and FTP work for the caller (every name
  resolves into the archive block); no HTTPS, no uploads; what Protoweb does
  not have gets Protoweb's own "host not found" page; a browser keeps the
  archive addresses for a while after the switch is turned off.

The first write of this image failed its read-back at 0x366000 (four lost
page programs in a row, the EON chip's known fault, a longer burst than seen
before); `flashcp` returned 1, nothing was restarted, and the second write
verified. `live/flash.sh` is the whole procedure with those checks.

## Download speed over a real V.90 call (2026-11-15): measured, cause found
Conexant CX93001 on port 2, V.90 at 49333 to 52000 bit/s, files fetched with
curl from the box's own busybox httpd, workstation multicast kept off the
link. The daemon now logs the link layer's figures at the end of every call
(`link: k, N401, compression, I frames sent/again/received, REJ, T401,
compressor in -> out`).

| file (300000 octets, random 100000) | octets/s |
|---|---|
| random | 5409 to 5554 |
| HTML | 7901 to 8652 |
| English text | 8059 |

- The line is never idle and nothing is sent twice (1517 I frames, 0 again,
  T401 never ran out; a second call: 11 again of 1529). The 10 to 12 % under
  the raw 6167 octets/s on random data is framing: TCP/IP headers, LAPM
  frames of 128 octets, HDLC zero insertion.
- The compressor is the limit: 312671 octets in, 193201 out (1.62). The
  modem's XID proposes **512 codewords** for V.42 bis (`02 02 02 00`),
  whatever its `AT+DS?` says (2048), with or without `AT+DS44=0`; our own
  offer of 2048 cannot raise it. The same XID offers V.44 (512 codewords,
  string 32, history 1024 by default; up to 2048 / 255 / 4096 by `AT+DS44`).
  V.44 is being added for this reason.
- Line rate against the power we allow in INFO1d (two to four calls each):
  -12 dBm0 (the public network figure, our default) 50666 to 52000 down,
  24000 to 26400 up; -10.5 dBm0 52000 / 24000; -9 dBm0 52000 to 53333 /
  21600; -7.5 dBm0 53333 / 21600 with lost pings on two calls of four;
  -6 dBm0 42666 and no traffic. Left at -12 dBm0 (`V90_MAX_TX_CODE`): the
  gain is 3 % and the upstream pays for it.
- `AT#UD` after a call: receive level -10 dBm, transmit -12 dBm, upstream
  3200 symbols/s carrier 1920 Hz, frame size 128, V.42 bis.
- A caller that offers V.90 but is held to V.34 rates
  (`AT+MS=V90,1,300,33600,28000,28000`) stays in V.90 at 28000 bit/s: the
  fall-back from V.90 to V.34 in Phase 2 could not be provoked with this
  modem and stands on its simulation test (`test_answer_v90_v34`, 102 checks).

## V.44 data compression (2026-11-15): in the link layer; simulation only

`v42/v44_comp.c` is ITU-T V.44 (stream method, clauses 6 and 7) written from
the Recommendation; `v42/v42_lapm.c` negotiates it in the user data subfield
of the XID and prefers it to V.42 bis where the caller offers both;
`v42/v42.c` runs whichever was agreed. Details, the list of readings and the
ratio table are in `v42/README.md`.

- The recorded XID of the CX93001 is a test case: the answer takes V.44 both
  ways with 512 codewords, strings of 32 and a history of 1024, and leaves
  the V.42 bis set out.
- Ratios on the two 300000-octet sample files, HTML and text: V.42 bis at
  the modem's 512 codewords 1.71 and 1.82; V.44 at the modem's defaults
  (512 / 32 / 1024) 2.00 and 1.98; V.44 at the most the modem takes by
  `AT+DS44` (2048 / 255 / 4096) 2.49 and 2.41. The history of at most 4096
  characters is the limit: the dictionary starts again every N8 characters.
- RAM: encoder 19152 octets, decoder 10336; they share storage with V.42 bis
  in `v42ec_t`, which is now 49752 octets (about 4.6 kB more per port). CPU:
  about 200 ARM instructions per character encoding, 80 decoding.
- Tests: `test_v44` 149078 checks, `test_lapm` 13964, `test_v42` 5203 (x86
  and ARM); the whole V.90 call of `dce` (h) now runs with V.44.
- The end-of-call log line names the procedure: `V.44 on/on 512/512
  codewords, string 32/32, history 1024/1024` (send/receive) or the V.42bis
  form as before. `v42bis = off` switches off both.
- Not yet tried against the real modem.

### With V.44, and where the rest goes (same day)
V.44 negotiated with the Conexant at its fixed 512 codewords, string 32,
history 1024 (`AT+DS44` takes no other values on this modem). Port speed
230400 unless said.

| file | octets/s | notes |
|---|---|---|
| HTML 300000 | 9730 to 9847 | 8652 with V.42 bis; compressor 312463 -> 168032 |
| text 300000 | 8884 to 8975 | one renegotiation inside each run |
| HTML + text 1200000 | 9455 to 9961 | 7822 with V.42 bis (seven renegotiations) |
| one line repeated, 600000 | 11651 | line only 30 % used |
| random 100000 | 5556 | |

- **The modem's own port is the ceiling.** The repeated line compresses six
  to one (623961 -> 99497), yet arrives at 11651 octets/s with the modem
  answering RNR for 35 of the 51 seconds: it hands at most about 12100
  octets/s to the computer (115200 bit/s inside, whatever speed the USB
  port is set to; 115200, 230400 and `AT+IPR=230400` give the same). On the
  HTML and text mix it says RNR about 180 times for 12 to 20 s in all, with
  V.44 and with V.42 bis alike, and never on random data.
- **The modem renegotiates on a timer.** 164000 samples (20.5 s) into data
  mode, and again at that interval for a while, it sends S, asks for silence
  (CPs), and comes back at the next rate up (49333 at the start of a call,
  52000 to 54666 after some minutes). Each costs about 2.5 s of line and,
  until now, up to a second more for T401. Our side now sends its
  checkpoint the moment data is back (`v42lapm_line_restored`, called by
  the DCE when the modem carries data again; `test_line_restored`).
- Trace switches used for this, left in the source and off: `-DV42_XID_TRACE`
  prints the caller's XID, RNR / REJ / T401 with a clock, and the V.90
  renegotiation steps.

## V.92: PCM upstream in the answering modem (simulation; real modem: see below)

`v92/` (V.92 clauses 5 to 9 from the Recommendation, see `v92/README.md`)
is in the product path. With `v92 = on` a V.92 caller that selects PCM
upstream gets it: 24000 to 48000 bit/s upstream instead of V.34's 33600 at
the most. No real modem has been called with it yet.

- `modem/answer.c`: Phase 2 offers V.92 (INFO0d bit 27, INFO1d Table 17);
  by the caller's INFO1a the call is V.92 (`v92/v92_p34d`), V.90 (`p34d`)
  or V.34, the last two exactly as before. Retrain from either side, rate
  renegotiation, cleardown, SNR, round trip delay and the two rates work in
  V.92. V.8 is not touched (it cannot say V.92).
- A V.92 start-up that fails is answered with a retrain whose INFO1d
  withdraws PCM upstream: the call goes on as V.90. The same when the V.92
  receiver gives up in data mode.
- The echo delay is measured in TRN1d when it is not given (as for V.90); a
  carrier watch ends the call 0.5 s after the caller is gone.
- `dce`: modulation and rates follow a retrain that changes the upstream
  kind and a renegotiation that changes the upstream rate alone; LAPM is
  asked where the caller stands when data flows again; two V.92 modems
  skip the ODP / ADP of V.42 (9.3.1) and the detection phase waits 5 s for
  the caller's first frame.
- `srv`, `cfg`: `v92 = on|off` per port (default **off**), on the settings
  page, in the status JSON; "V.92" in the status page, the history and the
  call log (`call to "42": answered; V.92 50666/33333 bit/s, 12 s`). Offered
  only with `v90` among the modulations and `v42 = on`. `max_upstream` now
  takes up to 48000 and is the ceiling of the PCM upstream too: with its
  default of 33600 a V.92 call gets 33333 at the most.
- Why off by default: with it on, every V.90 caller sees one more bit in
  our INFO0d (and its CRC), and V.92-capable callers see the V.92 INFO1d and
  skip the ODP. Harmless by the text, not shown harmless with real modems.
  With it off, what we transmit is octet for octet what it was before
  (compared over whole calls: V.90 mu-law and A-law with retrains,
  renegotiation and cleardown, a V.92-capable caller, a V.34 caller).
- Memory: `v92d_t` (40.9 kB) shares a union with the V.32bis modem;
  `answer_t` 93328 -> 97888 octets and `dce_t` 153616 -> 158176 on ARM
  (x86-64: 93560 -> 98192, 154184 -> 158816). The ARM daemon: text 378576
  -> 416784, bss 465104 -> 474304 (two ports).
- CPU in data mode, the answerer alone: V.92 takes 0.60 of V.90's time per
  sample on x86-64 (182 against 301 ns), 0.58 as ARM code under qemu. The
  V.92 start-up (the CPd design) has not been timed on the unit.
- Tests: `test/test_answer_v92.c` (answer layer: mu-law, A-law, retrains,
  renegotiations, cleardowns, ceilings, failed start-up, V.90 and V.34
  callers, V.92 off), `dce/test/test_call.c` (o) and (p) (V.42 + V.44 + PPP
  with traffic over V.92: 91 % of the line rate both ways, no frame
  damaged), `srv/test/test_srv.c`, and a V.92 call through the real daemon
  in `srv/test/ns_call.c`. `sim/v92call` is the simulated caller.
- `live/replay.c` prints the V.92 state, rates, canceller and receiver
  figures of a recorded call (`v92d_trace`).
- A side fix: `srv_init` cleared the `archive` flag of the status until the
  first reconfiguration (the status was initialised after the settings were
  adopted); now the order is right.

## V.92 with a real modem (2026-11-15): data mode with PCM upstream, PPP

A Conexant CX93001 (`AT+PIG=0`) on port 2 of the unit, RAM-only daemon
(nothing flashed). From recordings (`live/v92a.raw` on) and their replay:

- First calls: Phase 3 failed, V.90 after a retrain. Chain of causes, each
  with a test that failed first (`v92/README.md`, stage 4, readings 53 to
  59): the slow tail of the FXS echo taken for the caller's Su; the echo
  in data mode predicted for more downstream power than we use, no
  constellation found; the CPd design at one step per sample too much for
  the ARM926; a data frame of Ki = 0 in front of B1u; the modem ignores
  prefilter coefficient z2(64), which was our largest; after a rate
  renegotiation its trellis encoder does not start from zero.
- Now: V.92 49333 to 52000 / 24000 bit/s, LAPM + V.44, PPP up 32 to 34 s
  after dialling, ping 3 of 3, 300000 octets of HTML in 41 s (7297
  octets/s), the caller's own rate renegotiation completed (4.5 s without
  data), `pump_underruns` 0.
- The upstream rate is the lowest of V.92 on this line (the caller is no
  louder than our echo at the codec); V.34 upstream gave 26400.
- `v92` stays off by default.
- `live/replay.c` follows a recorded V.92 call through data mode (modem
  only, recorded octets as echo reference); `live/probe.sh` builds a
  daemon that sends a CPd made for an experiment (`-DV92D_PROBE=n`, bench
  only) and records one call.
- Left: the first Ja is lost (192 ms); a renegotiation runs the design
  again; our own renegotiation, retrain and cleardown on the line; other
  modems.

## Linear receive from the codec (simulation only)

2026-11-15. Offline work: nothing here has run on the unit, the image in
`fw/build` was not rebuilt, the kernel driver was compiled against the
kernel tree (objects in a scratch directory) and otherwise host-tested.

**Why.** The codec ran G.711 both ways. What we receive was companded
together with the echo of our own transmitter (9 dB down, as loud at the
codec as the caller), and the quantiser's steps at the echo's peaks held the
upstream at 24000 to 26400 bit/s (V.34) and 24000 (V.92).

**What changed.**

* `fw/drv/pump.c`: the receive ring carries 16-bit samples; `pump_lin16`
  (codeword to exact G.711 value, both laws), FIFO word and block helpers, a
  level meter. `fw/kernel/drivers/misc/v90line/v90line.c`: per port, a
  one-octet write `l` / `c` puts the Si3217x in 16-bit linear (PCMMODE
  format 3) or back in G.711; in linear a read gives 161 octets (80 samples,
  little endian) and every codeword written goes to the part as its exact
  linear value. Starts companded. `stats` has `rx_format`, `rx_dc`,
  `rx_peak`, `rx_mean_square`; new attributes `slic`, `slic2` (register and
  RAM access through the port's thread).
* `modem/answer`: `answer_tick_lin()`; `answer_cfg.rx_linear` and
  `.max_tx_code` (was the compile-time `V90_MAX_TX_CODE`). `dce_tick_lin()`,
  `srv_tick_lin()`, `srv_set_rx_linear()`, `srv_set_max_tx_code()`. The
  codeword entries are the linear ones behind a decoder.
* `v92/v92_design.c`, `v92_p34d.c`: with `rx_linear` no G.711 quantiser in
  the noise model; residual echo allowance 40 dB below the echo (34 dB
  companded).
* `srv/line.c`: `line_read2()` (format by the length of the read),
  `line_set_linear()`. `srv/main.c`: `-R` (receive format per port), `-P`
  (maximum downstream power code), recordings of linear lines with a
  "V90DLIN1" header; `live/replay.c` reads both kinds.
* `fw/drv/slic.c`: `slic_default_ram()`, `slic_gain_scale()`.
  `fw/tools/slicpoke`: `kreg`, `kram`, `kwram`, `gains`, `dac DB`, `adc DB`
  through the running driver (`fw/tools/slicpoke.arm` rebuilt).
* Simulation: `sim/channel` keeps the sample before its encoder (`us_lin`),
  `sim/v92chan` and `sim/v92call` have `rx_linear` (`v92call_tick_lin`),
  `test/line34.h` has `rx_lin`.

**Scale.** The modem already works on the 16-bit scale of the G.711 tables
(mu-law +/-32124), which is the scale assumed for the codec's linear samples
(HARDWARE.md, A1): the sample is used as it is, with all 16 bits. Nothing
is left on the table in the receivers; what is left is in the codec's own
noise and in our allowance for residual echo (below).

**Simulated rates on the measured line** (transmit -12 dBm0, echo 9 dB down
= 1405 RMS at the codec, caller at about the same level;
`test/test_answer_lin.c`, identical on x86 and under qemu-arm):

    V.34 call:            companded 24000 bit/s up, SNR 33.1 dB; linear 31200 bit/s up, SNR 45.1 dB
    V.90, V.34 upstream:  companded 28800 bit/s up, SNR 34.7 dB; linear 31200 bit/s up, SNR 48.1 dB
    V.92, PCM upstream:   companded 26666 bit/s up;              linear 42666 bit/s up
    (V.92 with code 11 = -6 dBm0 announced: 41333 up)
    V.92 design alone (v92/test/test_design.c): 26666 -> 41333 bit/s

The V.34 figures at the top are the symbol rate's ceiling in these
simulations (31200 at 3200 symbols/s here), not the line's. The line noise
in them is low (8 to 10 RMS, -64 to -66 dBm0); the real line's is not known,
and with linear receive it becomes what counts.

**Tests** (all pass on x86 and ARM/qemu unless said):
`fw/drv`: pump 814 checks (table bit-exact against `v90_pcm.c` for the 512
codewords, FIFO words, block formats, meter), slic 114 (gain helpers),
proslic 147, qspi 94. `test_answer_lin` 51 checks: the three comparisons,
the flag reaching the V.92 design, `max_tx_code` reaching INFO0d and the
design, V.32bis / V.22bis / V.21 through the linear entry. `v92` design 198
checks (linear A/D case added). `srv` 432 checks (with the PPP compression work of the same day): the line layer reading
both block formats, and whole calls V.22bis, V.90,
V.92, V.34, V.32bis a second time through `srv_tick_lin` (V.92 48000 up,
V.90 31200 up, V.34 31200 up there). A converted recording (`live/v34a.raw`,
`v90chk.raw` turned into the linear kind) replays line for line like the
original.

**Not done, not verified.**

* Nothing on the unit: the format switch, assumptions A1 to A4 of
  HARDWARE.md (scale, offset, gains, bit order), the interrupt's cost with
  the table look-up, the `slic` attribute, `slicpoke`'s new commands (built
  for ARM, never run against the driver). The checks to make first are
  listed in HARDWARE.md, "How to check on the box".
* The image is not rebuilt; `fw/rcS` does not pass `-R` (add it on the
  bench: `v90d ... -R l` or `-R ll`).
* The residual echo allowance with linear receive (40 dB below the echo) is
  a choice, not a measurement: a real hybrid's distortion is not in the
  simulation. If V.92 comes up with bad frames in linear format, build with
  `-DV92_DSN_ECHO_DIV_LIN=48` (the companded figure); if it is clean and
  `answer_snr` has room, 192 gives 48000 in the simulation.
* Lines over `fd:` / `unix:` carry codewords only; the daemon as a process
  was not run with linear blocks (the core was, through `srv_tick_lin`).
* Hybrid coefficients: a procedure in HARDWARE.md, no code.
* A default from the device tree or the kernel command line: not added; the
  driver starts companded and the daemon switches it.

## PPP compression (CCP): Deflate and MPPC (2026-11-15); simulation and real pppd, no real call yet

CCP (RFC 1962) with two algorithms written from the RFCs, kept in
`refs/rfc/`: Deflate (RFC 1979 over RFC 1951, `ppp/ppp_deflate.c`) and MPPC
without MPPE (RFC 2118, `ppp/ppp_mppc.c`); the negotiation, the resets and
the choice between the compressed and the native form are in `ppp/ppp.c`.
On by default, `[ppp] compression = off` turns it off (config file, settings
page, status page and JSON). The readings of the RFCs and the assumptions
about Windows are in `ppp/README.md`.

- Negotiation: Deflate before MPPC, each direction on its own. Against a
  real pppd 2.5.1 with its defaults (`srv/test/ns_pppd.c`, the kernel's
  `ppp_deflate` on the other side): pppd sends
  `<deflate 15> <deflate(old#) 15> <bsd v1 15>`, we reject the last two and
  Ack `<deflate 15>`; pppd Acks our `<deflate 15>`; "Deflate (15) compression
  enabled". With `deflate 12,10` pppd Naks our 15 down to 10 and reports
  "Deflate (12/10)"; with `nodeflate` everything is rejected in turn and the
  call carries IP uncompressed; with `noccp` pppd answers with an LCP
  Protocol-Reject and we stop. Files fetched and sent over TCP arrive whole
  in every case.
- Ratios, sample files in 1460-octet segments behind 40-octet headers
  (octets of the IP packets over octets on the wire), and what that makes
  of a 5.9 kB/s line:

  | | HTML | kB/s | text | kB/s |
  |---|---|---|---|---|
  | Deflate, window 15 | 3.30 | 19.5 | 3.04 | 17.9 |
  | Deflate, window 14 | 3.18 | 18.8 | 2.96 | 17.5 |
  | Deflate, window 13 | 3.03 | 17.9 | 2.86 | 16.9 |
  | Deflate, window 12 | 2.87 | 16.9 | 2.73 | 16.1 |
  | MPPC | 2.42 | 14.3 | 2.33 | 13.7 |
  | V.44 as the Conexant takes it (2048 / 255 / 4096), for comparison | 2.49 | | 2.41 | |

  The same input through Python zlib at level 6 with a sync flush per
  packet: 91521 octets against our 93624 (HTML).
- With V.42 bis / V.44 underneath (`dce/test/test_call.c` test q, V.90 at
  50666 bit/s, synthetic markup): no compression 5992 octets/s, V.44 alone
  16481, Deflate alone 20272, Deflate over V.44 20179, MPPC alone 15864, MPPC
  over V.44 15807. Both on costs under 1 % against link compression off
  and is 22 % faster than V.44 alone with Deflate; with MPPC it is 4 %
  slower than V.44 alone on this input, so an MPPC caller gains nothing on
  a V.44 call and a factor of 2.6 on a call without link compression.
- Loss: on V.22bis without error control and with noise bursts, both
  algorithms recover by Reset-Request / Reset-Ack and CCP stays open
  (Deflate: 58 of 80 packets down, 54 up at 613 / 573 octets/s; without
  compression 73 and 71 at 217 / 210). To make that work the server takes
  queued compressed frames back out of the transmit buffer on a
  Reset-Request, lets the Reset packets jump the queue, and keeps the queue
  of compressed frames short on such lines.
- RAM per port: `ppp_t` is 193464 octets on ARM with the 32 KiB window
  (Deflate compressor 146296, decompressor 33608, MPPC 32772 and 8194).
  `PPP_DEFLATE_MAX_WBITS` 14 / 13 / 12 makes it 127928 / 95160 / 82036. The
  daemon's bss went from 474516 to 840900 octets for two ports; the unit
  has about 21 MB free, so the default stays 15.
- CPU, ARM instructions by qemu: Deflate compressing 225 per octet on HTML,
  238 on text, 262 on random data, 374 on the worst input built against it
  (562000 for one packet; the search is capped at 10000 candidates a
  packet); decompressing 85. MPPC 72 each way. A budget of 40 octets per
  millisecond per port caps the total at about 9 million instructions a
  second per port for Deflate; beyond it packets go out uncompressed.
- The end-of-call log has a line `PPP compression: to caller Deflate A -> B
  octets (N packets sent as they were), from caller ...; resets ...; CCP
  opened N time(s)`.

```
test_deflate: 1029767 checks, 0 failures
test_mppc: 1246950 checks, 0 failures
test_ccp: 84758 checks, 0 failures
zlib_check: 147540 checks, 0 failures
test_e2e: 13158 checks, 0 failures
call: 920 checks, 0 failures
srv: 432 checks, 0 failures
test_config: 524 checks, 0 failures
test_status: 546 checks, 0 failures
test_settings: 680 checks, 0 failures
ns_pppd: 104 checks, 0 failures
```

Not done: a call from a real modem with pppd, and any Windows client (the
MPPC side was tested only against our own model of one and the vectors of
RFC 2118). `test_e2e`'s random-loss run skips seed 7: with the extra CCP
frames that seed loses enough Configure packets in a row to run out of
Max-Configure, as about one seed in forty did before the change.

## PPP compression and Windows XP on the line (2026-11-15, late)
Daemon with CCP pushed to the unit (RAM), Conexant CX93001 on port 2, V.90
52000/26400, V.44 underneath (it sees compressed data and stands aside:
94869 -> 95290).

| caller | negotiated | HTML 300000 | one line repeated 600000 |
|---|---|---|---|
| Linux pppd 2.5.1 | Deflate (15) both ways | 18954 octets/s (311223 -> 93022) | 37952 octets/s (622039 -> 17760) |
| Windows XP dial-up networking (VirtualBox, modem by USB) | MPPC both ways | 12696 octets/s | 38709 octets/s |
| the same before CCP | V.44 in the modems only | 9700 (Linux), 3720 to 5808 (XP) | 11651 (Linux) |

- No resets, no packets dropped, CCP opened once per call. The Conexant's
  115200 bit/s port is no longer the ceiling: it carries compressed data.
- Windows XP: the modem as COM4 through usbser with an .inf of our own
  (`c:\cx\cx.inf` in the VM) and XP's "Standard 56000 bps Modem"; dialled
  with `rasdial`, timed with a small script. It connects every time (V.90
  52000 to 54666 / 24000 to 26400, LAPM, V.44, PAP, IPCP). MPPC was only
  tested against our own model before this; XP is the first real peer.
- Windows 95 / 98 / Me have not been tried.

### Windows 98 SE on the line (same day)

The Windows 98 SE virtual machine (4.10.2222), given the Conexant modem as
its COM1 ("Standard Modem"), dialled 6387825442 with Dial-Up Networking as
guest / guest and connected: V.90 52000/24000, V.44 on both ways, and the
box log for the call reads

    PPP compression: to caller MPPC 971860 -> 172958 octets (0 packets sent
    as they were), from caller MPPC 17236 -> 37846; resets asked of us 0,
    by us 0 (0 packets dropped), CCP open

over a call of 765 s with no I frame sent again. Downloads with the Windows
`ftp` client from a busybox ftpd on the box (port 2121):

| file                     | octets | time    | rate          |
|--------------------------|--------|---------|---------------|
| markup-like text         | 300000 | 22.41 s | 13.39 kB/s    |
| highly repetitive text   | 600000 | 16.09 s | 37.29 kB/s    |

Seen on the way: with archive mode on, the Windows 98 `ftp` client aimed at
port 21 of the box was answered by the archive ("220 Archive of 10.64.64.1
via Protoweb (v90d archive mode, read only)"), which is the intended
behaviour. Windows Me was not measured: its virtual machine has no modem on
COM1 and its own USB modem driver did not take the modem (error 666). It
shares Dial-Up Networking and MPPC with Windows 98; that it works there too
is an expectation, not a test result. Windows 95 is not tested either.

## The highway fell out of step after a console line (2026-11-15, evening): found on the unit, fixed

After the image with the linear-receive driver was flashed, the Conexant
modem stopped connecting on about one boot in three, and always after a
save on the settings page: it answered our ANSam with 1800 Hz (V.32 AA)
instead of a V.8 call menu, or took the dial tone for busy. The modem and
the line were sound (the image flashed before connected at 52000/24000 in
the same hour).

What was measured: in a recording of a failed call the far end's 1800 Hz
tone, fitted separately at each of the 8 sample positions of the driver's
1 ms chunk, has the same amplitude and phase at all 8 (4129 to 4195, 159
degrees), so the receive direction is clean. The echo of our own 2100 Hz
answer tone has not: 268 to 2753 and phases from -81 to +103 degrees (a
good call: 1654 to 1730, 38 or 39 degrees at all 8). What we sent was
being chopped inside every chunk; in the spectrum the 2100 Hz came back as
1100 and 3100 Hz with 13 % left at 2100.

Cause: when interrupts stay off for milliseconds, the transmit FIFO of the
TDM block runs empty or the receive FIFO runs over, and the two levels no
longer add up to the prefill. From then on part of every chunk is lost,
until the highway is started again. A line of kernel log on the serial
console (115200 bit/s, interrupts off) is enough. The boot prints many of
them after the highway is running; whether the last one left it in step was
luck. The old driver had the same weakness (same interrupt routine); the
`underruns`/`overruns` counters that "stand still after start-up" were
counting the events, not healing them. The difference between the two
counters does not tell the state (a boot with 21/18 connected).

Shown directly: on a boot that had just connected at 52000, three long
lines written to /dev/kmsg moved the counters from 21/21 to 24/24 and the
next call ended in BUSY.

Fix (`fw/kernel/drivers/misc/v90line/v90line.c`, `line_irq`): an underrun
or overrun is counted and the highway is started afresh (clock gate, block
reset, FIFOs empty, prefill, enable), a few milliseconds of silence on the
line. The same experiment on the fixed image: counters 22/18 to 25/21, both
SLICs untouched (`slic_inits 1`, no "lost its settings"), next call CONNECT
52000. Six reboot-and-dial cycles before the fix: 4 connected, 1 BUSY, 1
without result in 45 s. Six after: 50667, 49333, 50667, 50667, 52000, 50667.

Found on the way, changed, not yet run on the box:
* A daemon that is handed settings saved by a newer firmware refuses them
  ("unknown key 'compression' in [ppp]") and never starts; with nobody to
  confirm the start, three boots later the box is back in the stock
  firmware. `fw/rcS` now checks the loaded settings with `v90d -p` and
  starts with the factory settings when they are refused (the flash keeps
  what it has until somebody saves).
* Unbinding the line driver through sysfs with the ports open took the
  kernel down (it has no remove()); the driver now sets
  `suppress_bind_attrs`.

Open: a start of the highway in mid-call was not measured against a
running V.90 connection (it should cost a retrain at most).

## Call history page (2026-11-15, evening)

`/calls`, tab "Calls": the newest 20 finished calls of both ports in one
table, newest first, each row with its port (`status_render_calls`,
cfg/status.c; a port keeps 20 calls now, was 16; in memory only). cfg
tests on x86 and ARM (order across ports, the 20 rows, one-port box, page
size, route, login). On the box after a test call:

    Port Start (UTC)          Duration Dialed     Result   Modulation Down  Up    Ended
    2    2026-11-15 19:13:26  0:00:17  6387825442 answered V.90       50666 26400 closed by peer

## Linear receive on the unit, and the power ceiling (2026-11-15, evening)

Linear receive runs on the box (HARDWARE.md, "Linear PCM and the gain
knobs", has the checks): the codec's samples come 10.9 dB under the G.711
scale, the daemon corrects by 7/2, and a dev: line is asked for linear by
default. One call each, same hour, downstream/upstream:

| call  | companded     | linear        |
|-------|---------------|---------------|
| V.90  | 50666 / 26400 | 52000 / 28800 |
| V.92  | 50666 / 25333 | 50666 / 30666 |
| V.34  | 31200 / 28800 | 31200 / 31200 |

The simulation had promised 42666 for the V.92 upstream; the unit gives
30666 to 36000.

The most power a caller may ask of us (`-P`, the INFO0d code; 23 = -12 dBm0
is the default), linear receive, one call each:

| ceiling   | V.90          | V.92                              |
|-----------|---------------|-----------------------------------|
| -15 dBm0  | 49333 / 28800 | 49333 / 36000                     |
| -14 dBm0  | no carrier    | fell back to V.90, 49333 / 28800  |
| -13 dBm0  | 52000 / 28800 | 52000 / 33333                     |
| -12 dBm0  | 52000 / 28800 | 50666 / 30666                     |
| -11 dBm0  | 52000 / 28800 | 52000 / 30666                     |
| -10 dBm0  | 52000 / 28800 | 50666 / 30666                     |
| -9 dBm0   | 52000 / 26400 | 50666 / 29333                     |

Downstream stops at 52000 whatever the power, so power is not what keeps
this modem from 53333 and above; more of it only costs upstream (echo).
Less of it helps the V.92 upstream. Not explained: the failed training at
-14 dBm0 (one call). 56000/48000 is not reached; what limits the
downstream at 52000 on this line is open (the D/A path of the SLIC, the
hybrid, or the Conexant's own receiver: not separated).

## Every modulation on the unit, linear against companded receive (2026-11-15, late evening)

Conexant modem on port 2, the mode forced in the caller (`AT+MS=...,0`;
V.92 with `AT+PIG=0`), PPP from Linux pppd; daemon from RAM on the flashed
kernel with the highway fix. PPP came up on every call. Downstream/upstream:

| caller's mode | linear          | companded       | pings (box 4, internet 3) |
|---------------|-----------------|-----------------|---------------------------|
| V.92          | 50666 / 30666   | 50666 / 24000   | all, both formats         |
| V.90          | 52000 / 28800   | 50666 / 24000   | all, both formats         |
| V.34          | 31200 / 31200   | 31200 / 28800   | all, both formats         |
| V.32bis       | 14400 / 14400   | 14400 / 14400   | all, both formats         |
| V.22bis       | 2400 / 2400     | 2400 / 2400     | all, both formats         |
| V.22          | 1200 / 1200     | 1200 / 1200     | see below                 |
| V.21 (x3)     | 300 / 300 (x3)  | 300 / 300 (x2 logged; the first call's log line was read too early) | see below |

At 1200 and 300 bit/s the test's one ping a second with an 8 s limit
proves nothing (2 of 4 and 0 of 4 answered in both formats). Again in
linear with 16-octet pings 12 s apart and a 30 s limit: V.22 3 of 3 to the
box (0.66 to 4.5 s) and 2 of 2 to 1.1.1.1; V.21, four calls, all connected,
3, 2, 3 and 1 of 3 to the box and 2 of 2 to 1.1.1.1 each time, with round
trips of 25 to 54 s. That delay is the workstation's own multicast
(SSDP, WS-Discovery 802 octets, mDNS: `live/v21.tcpdump.txt`) queued
ahead of the pings on a 30 octet/s link, not the box: the earlier
measurement on a quiet link was 3.35 s.

V.21 connected 9 times of 9 today (the "one in four fails" of the earlier
bench was not seen; not explained, so not claimed fixed).

## What is left

- **V.92**: PCM upstream is in (simulation only, `v92 = off` by default):
  the first call from the Conexant (`AT+PIG=0` enables PCM upstream there;
  `v92 = on`, `max_upstream = 48000` here), then the default. After that
  quick connect and modem on hold.
- V.90 Phase 2 followed by V.34 (a V.90 caller that picks V.34 in INFO1a);
  in progress.
- Two calls at once with real modems (needs a second modem).
- V.21: one attempt in four did not connect; the connect takes 14 s after
  the answer (V.8 wait, then the V.22 wait).
- V.32bis rate renegotiation; V.22bis rate change; raising the V.90 or V.34
  rate again after it was lowered.
- Release: done on the unit 2026-11-15, see "Release file on the unit"
  below. Open: the power pulled during an upgrade on a real box, a second
  unit, another board revision.

## Carried over: x86 modem test infrastructure (Track B)
`~/v90d/stage0/` holds x86-first tooling built earlier (channel sim `chsim`,
G.711 tables, a PCM pattern gen/analyser `pcman`, and a SIP/RTP echo endpoint)
plus `bench.*`. Still useful for developing the V.34/V.90 receiver on x86 before
porting; re-evaluate against the new "own firmware / own PCM path" architecture
(the SIP/RTP endpoint is no longer needed once we own the TDM stream).

## Release image: system side (offline; nothing proven on hardware)
2026-11-15. Built and tested on models only; no box, no modem was touched.
Design in DESIGN.md ("The running system's side"), facts in HARDWARE.md.

What was built:
- No hand-over from the loader to the kernel: `v90nv where` reads the flash
  the way the boot programs will at the next start (scheme `loader`, `old`
  or `none`; set; counts; what the next start runs; whether the stock set
  is good). No kernel code changed.
- Flash driver list: seven ranges for both schemes in both sets (list size
  4 to 8, device tree only); the whole-chip MTD device is writable inside
  those ranges; `v90nv` narrows itself to the scheme and set it found.
- Settings: loader scheme at our `core` + 0x20000, bench scheme at 0x060000
  as before. They do not survive a stock install into our set (so not the
  move from the bench scheme to the release, and not stock and back).
- `v90nv` (`fw/tools/v90nv.c`, `nvboot.c`, `nvflash.c`, `nvimage.c`,
  `nvhttp.c`): `where`, `confirm`, `to-stock`, `check`, `upgrade`,
  `conf-save`, `conf-load`, and the two helpers the web page forks. Upgrade
  takes a system image or a release file, checks all of it first, writes
  the signatures last, keeps the set's install counter.
- `fw/rcS` uses them; one script for both schemes.
- Web: "Firmware" tab (`cfg/firmware.c`; hand-over of the connection in
  `cfg/httpd.c`; fork of the helper in `srv/main.c`).
- `fw/VERSION` (0.1.0), in the image name, the release file and the pages.
  `make -C fw release` builds a non-debug image of its own.

Test output:
- `make -C fw test-release`:
  `v90nv: 26 checks, 0 failures` (host and ARM under qemu);
  `tools/test_v90nv_boot.py`: `Ran 31 tests ... OK` on the host binary and
  again on the ARM binary (where, confirm, to-stock, upgrade with power
  lost before and inside every step for both schemes and both sets judged
  by the models of the stock boot loader and of our loader, settings, the
  two web helpers, and a copy of this unit's flash backup);
  `test_mkgsfw.py`: `Ran 18 tests ... OK`; `test_release.py`:
  `Ran 11 tests ... OK`; `loader: 125 checks, 0 failures` (host and ARM).
- `make -C fw/drv test test-arm`: `qspi: 94 checks, 0 failures` (both).
- `make -C cfg test test-arm`: `test_firmware: 44 checks, 0 failures`,
  `test_httpd: 2028 checks, 0 failures`, `ALL TESTS PASS`,
  `ALL ARM TESTS PASS`.
- `make -C srv test test-arm`: `rig: 26 checks, 0 failures` (both).
- `make -s check-ns`: `ns_call: 134 checks, 0 failures` (upload refused
  during a call, bad file refused, good file written to a file standing in
  for the flash, back to stock).
- `make release`: `release through the loader: 22 checks, 0 failures`, then
  `v90nv check` accepts both the release file and the bare image.

Not proven, to be done on the box in this order (each step names its way
back; the box starts in the bench scheme):
1. Start the new bench image from RAM (`tools/ramboot.sh`), nothing
   written: `v90nv where` must say `scheme old`; settings load; the
   Firmware page shows. Way back: power cycle.
2. Still from RAM: `v90nv conf-save` and `conf-load` through the whole-chip
   device; a write outside the list must be refused by the kernel. Way
   back: the settings blob is the only thing written.
3. Write the new bench image with `v90nv upgrade` (shell), time it, restart,
   see `confirm` put `v90d_tries` back to 3. Way back: `bootm` refuses a bad
   image and `gsboot` starts stock; write the old image from there.
4. The same through the Firmware page (upload). Same way back.
5. "Back to the stock firmware" on the page. Stock must start and stay.
   Way back to ours: `v90nv env-set /dev/mtd13 v90d_tries 3` in the stock
   shell.
6. On stock: read `/proc/gxp/dev_info/required_bits`, set `REL_BITS`, and
   the loader experiments already listed under "Release image" (loader from
   RAM first).
7. From stock, without arming `v90d_tries` again: upload
   `v90d-ht802fw-bench.bin` on the stock upgrade page. Ours must start
   through the loader; `where` says `scheme loader`, the set, and
   `unconfirmed 1` until the minute is over. Way back: three unconfirmed
   starts, or `v90nv to-stock`, or the stock set simply being the other one.
8. Loader scheme: save settings, restart, they are there; upgrade through
   the Firmware page with the release file; pull the power in the middle of
   one upgrade on purpose (stock must come up); back to stock on the page.
9. The non-debug release file, last.
The loader started from the other set (B) cannot be tried on this unit as
it stands: the stock system runs from B and always installs into A.

## A service per port: Internet, Archive, Dreamcast (2026-11-15)

Asked for by the owner after the DreamPi research (`refs/dreampi-facts.md`):
the archive switch per port instead of for the whole box, and a setting for
a Sega Dreamcast in the same place. There is now one key, `service`, in
`[line]` and `[line2]` (Line page, "Service"): `internet`, `archive`,
`dreamcast`. `[network] archive` is gone from what we write; an older
settings file that has `archive = on` still reads and means both ports
(tested, and seen on the unit: its saved settings had it, and both ports came
up as Archive after the upgrade).

`dreamcast` keeps the port's number and the PPP login (the owner's choice;
the first version let any number and any login in, as DreamPi does). It adds
what the console cannot set for itself: no CCP, no V.92, the two name
servers of the game revival projects in IPCP, the hop count of the console's
packets set to 64 and a repeated SYN from the ports 3200 to 3205 passed on
once a second (`srv/console.c`, portable, integer only). Not built: the list
of dead server addresses to rewrite, port forwarding towards the console,
"Dreamcast Now", NetLink. **Nobody here has a Dreamcast: none of this has
been tried with a real console.** What a console's modem negotiates with
ours (V.90 or K56flex only, in which case V.34) is not known either.

Tests written first: `test_service` (cfg/test/test_config.c), the service
part of test_settings.c and test_status.c, `test_console_packets` and
`test_dreamcast_call` (srv/test/test_srv.c: a whole call on the simulated
line, with the same call on `internet` as the control), ns_call with
`service=archive`. Host `make check` and `check-ns`: 0 failures.

On the unit (image md5 ef510a6d..., flashed, start confirmed), the Conexant
modem with pppd on port 2:

| port 2 | port 1 | login | CCP | DNS given | name question to a made-up server | a packet sent with TTL 30 leaves the box with |
|---|---|---|---|---|---|---|
| dreamcast | archive | accepted (guest) | Protocol-Reject | 46.101.91.123, 209.50.50.129 | no answer | 63 |
| dreamcast | archive | unknown name: "Authentication failed" | | | | |
| archive | internet | accepted | Deflate | 9.9.9.9, 1.1.1.1 | 198.18.0.1 (the gateway answered) | 29 |
| internet | archive | accepted | Deflate | 9.9.9.9, 1.1.1.1 | no answer | 29 |

So the archive of one port does not catch the other port's traffic, and the
console's treatment is only on the port set to it. (63 is 64 less the box's
own hop.) Port 1 as the calling side could not be tried: one modem.

## One login at a time logged the owner out (2026-11-15)

The owner kept being logged out of the pages "with each call". The web
server had room for one session: every login of a test script ended the
browser's, and the other way round (seen when a script got 401 in the
middle of a run while the owner was on the pages). There is room for four
now (`HTTPD_SESSIONS`); a fifth takes the place of the one unused the
longest, logging out ends one session, a new admin password ends all.
test_httpd.c covers it; on the unit two logins were in at the same time.

## Saved settings this firmware cannot read: proven on the unit (2026-11-15)

`fw/rcS` checks the settings loaded from flash with `v90d -p` and starts on
the factory settings when the daemon would refuse them (an older firmware
meeting keys of a newer one; before this the daemon never started, no start
was confirmed and three restarts later the box was in the stock firmware).
On the unit, image ef510a6d...: a settings file with the line
`from_the_future = 1` written to flash with `v90nv conf-save`, then reboot:

    rcS: settings from flash
    rcS: the saved settings are not understood by this firmware: factory settings for now
    rcS: starting the daemon on dev:/dev/v90line and dev:/dev/v90line2
    rcS: start confirmed

Daemon running, `v90d_tries` back at 3. The file in flash is left as it is
until somebody saves on a settings page; two saves later `conf-load` gave a
file without the line, and a V.90 call with pings went through.

## Release image, on the unit: steps 1 to 4 (bench scheme) done (2026-11-15)

The numbered list under "Release image: system side". The unit starts the
bench way; image ef510a6d... was in flash.

1, 2. `v90nv where` on the running system: `scheme=old`, `board=9610003725A
   known`, `old_tries=3`, `settings=0x060000`, `image=0x2e0000`, `stock=B
   good`. Settings load and save through it (every save of this day). The
   Firmware page shows the same in words.
3. `v90nv upgrade` from the shell. The same image again: "this image is
   already there: nothing written" (2 s). The build before it (67082166...):
   "written: v90d ht802 0.1.0, 2216880 octets at 0x2e0000", 24.5 s, read
   back from flash with the same md5; restart; `rcS: start confirmed`,
   `old_tries=3`.
4. The Firmware page: upload of the current build (multipart, 2.2 MB) took
   2.8 s and answered "The file is good ... being written"; the box
   restarted by itself and answered again 49 s after the upload began;
   flash read back as ef510a6d..., `rcS: start confirmed`, `old_tries=3`.

Not done yet: 5 (back to stock from the page) to 9.

## The loader on the unit (2026-11-15)

Everything here was done from RAM out of the running stock system
(`fw/tools/ramboot.sh`), with set A staged by hand (our loader in core A,
our image behind a stock header in base A, stock prog A left as it was, so
`gsboot` keeps starting stock set B). No serial cable.

What the loader did on the first run, read back from a trace it left in its
state sector (build switch `LD_TRACE`): `a5 01 00 02 00 00 01 5a`, that is,
decision BOOT, set A, image from its own base, two starts left. So on real
hardware it recognises its core, reads the board number, programs the count
word, reads and checks the 2.2 MB image. Then the box reset.

| variant | result |
|---|---|
| loader as designed (watchdog 100 s, fed before the jump) | reset during the kernel's first seconds |
| same, instruction cache off at the jump | reset |
| same, 20 ms and 1 s between watchdog writes | reset |
| same, stock side not starting the watchdog first | reset |
| loader sits still after its feed, marking each second in flash | marks 00 01, dead 2 to 3 s after the feed |
| minimal stub: copy the image, jump (no watchdog) | our system up |
| loader without any watchdog write | our system up, `scheme=loader set=A`, confirmed at 66 s |

Decision: the loader does not touch the watchdog (DESIGN.md, HARDWARE.md).
What catches a hang before the kernel is the count: three power cycles and
the loader hands the box back to stock.

The real entry differs from the RAM one: `bootm` enters with the MMU and
both caches on, and a hang there could not be undone without a serial cable.
Two things were done about it before any real start:

- Read in the stock boot loader (facts in HARDWARE.md): after its copy it
  flushes the data cache only if the range ends on a 32-octet line, and it
  takes four times the length for the range. Our loader was 5644 octets, so
  it would not have been flushed. The loader image is now padded to whole
  lines (5664), with a test in `fw/tools/test_release.py` that failed first
  (`16 != 0`).
- A stub (bring-up only, not in the tree) builds the same state from RAM:
  flat 1:1 sections, RAM and flash window cached, MMU and both caches on,
  loader copied through the data cache, flushed by the boot loader's rule,
  entered with a device tree that says `bootpartition=0`. Through it:

| loader | flushed | result |
|---|---|---|
| 5644 octets | no | `scheme=loader set=A`, rcS "started: scheme loader, set A" |
| 5664 octets | yes | the same |

Not covered by this: the clocks and pins as U-Boot itself leaves them (the
stock kernel has been running in between). Our kernel has been started from
U-Boot by `bootm` since the bench scheme, so that half is known from there.

## Release file on the unit: stock to v90d and back, steps 7 to 9 (2026-11-15)

All through the web pages a user would use, no shell needed for the install.
Loader 5664 octets, no watchdog writes; `make -C fw test-release` clean
before the first upload (31 + 31 boot tests, 18, 11, loader 125 twice).

| step | what was done | result |
|---|---|---|
| 7 | bench release file on the stock upgrade page (`firmware_file` to `upload_firmware`) | page answers after 128 s, box restarts by itself; `scheme=loader set=A installed=whole`, "start confirmed" at 65 s |
| 8a | Line page: rings 2, service archive; restart | "settings from flash", both values there |
| 8b | a different bench release file on our Firmware page | accepted in 18 s, restart by itself about 30 s later, new image (kernel argument `loglevel=7` seen), settings kept, confirmed at 65 s |
| 8c | "Back to the stock firmware" on our page | core A magic zero, stock set B up 55 s later |
| 9 | non-debug release file (`v90d-ht802fw.bin`, md5 d05ab695...) on the stock page | our login page answers 130 s after the upload; Firmware page: started by the loader from set A, 0 starts not confirmed, stock in set B good; only TCP 80 open (no telnet, no ssh) |
| 9b | call into the non-debug release, Conexant modem on port 2, factory settings | PPP up after about 28 s, 3 of 3 pings to 10.64.64.1, HTTP 200 from example.com over the call |

Found on the way:
- The stock page refuses an upload in its first minute after a start with
  "Device already has another provision running!" and writes nothing (its
  own check at start holds the lock). The same upload a few minutes later
  went through. The flashing guide must say: wait two minutes after the box
  has started.
- Two waits of mine that looked like a hang were faults of the watching
  script, not of the box.

Not done: the power pulled in the middle of an upgrade on purpose (needs a
hand at the box; covered on the models for every step), the loader started
from set B (this unit installs into A), a second unit.


## V.92 on real hardware (2026-11-15, late): connects on the Conexant

Our own notes above said V.92 PCM upstream was simulation only and no real
modem had been called with it. Done now, on the bench unit running the
0.2.3 release, port 2 set `v92 = on`, `max_upstream = 48000`:

- Conexant CX93001 dialled with `AT+PIG=0` (PCM upstream enabled). The box
  answered and the call came up as **V.92** (status page `modulation` =
  v92), LAPM error control, PPP/IPCP up (caller got 10.64.64.3).
- Two calls: **52000 down / 32000 up** and **50666 / 30666**. The upstream
  settled around 31-32 kbit/s on this line, not the 48k ceiling. Downstream
  is the usual V.90-range PCM rate.
- Data path good: ping to the box's PPP end 3/3, ~121 ms.
- `v92` was put back to off (the default) and `max_upstream` to 33600 after
  the test.

So V.92 is proven to connect and carry PPP on real hardware; the headline
PCM-upstream rate on this line/modem was ~32k, not 48k. Still open: a
second modem brand, and whether another line/modem reaches a higher
upstream.

## Windows 98 dial-up re-verified on 0.2.3 (2026-11-16), driven headlessly

The HT802 bench unit on the 0.2.3 release, port 2 (Conexant on /dev/ttyACM0).
The Windows 98 SE VirtualBox VM was driven with no human at the keyboard:
`VBoxManage controlvm WIn98 keyboardputscancode/keyboardputstring` for input
and `screenshotpng` to read the screen. The modem reached the VM by wiring
its COM1 to the host tty (`modifyvm --uartmode1 /dev/ttyACM0`).

- The desktop's "Shortcut to Netstalgia" Dial-Up Networking connection dials
  6387825442. First attempt: Windows error **691** (bad password) - the DUN
  connection authenticates as user `netstalgia`, and the box was at its
  bench config with only `guest`. So the modem trained fine (box went
  ringing -> negotiating) and only PPP auth was refused.
- With the box's PPP auth set to none for the test, the redial **connected**:
  box line connected, **V.90, 52000 down / 28800 up, LAPM**, and the session
  held steady. So Windows 98 dials the box and brings a PPP link up.
- Afterwards: the VM was powered off, its COM1 put back to disconnected, and
  the box's auth restored to CHAP. Powering off the VM did not drop the
  Conexant's carrier; the modem was hung up from the host (drop DTR + ATH0)
  and the box went idle.

Note on the "691" path: it is an authentication result, not a modem or
line failure - the two modems negotiated a V.90 carrier with LAPM before
PPP auth ran. A real deployment just needs the DUN credentials to match a
box user.

## V.92 under Windows 98: did not negotiate (2026-11-16)

Follow-up to the Win98 dial-up test: with the box offering V.92 (port 2
v92 = on, max_upstream 48000) and `+PIG=0` added to the Windows "Standard
Modem" extra settings (Modems control panel -> Properties -> Connection ->
Advanced -> Extra settings), the Win98 DUN call still connected as **V.90**
(28800 upstream), not V.92.

So V.92 under Win98's generic Standard Modem driver did not engage, even
with the PCM-upstream init. This is a caller/modem-driver matter, not a box
one: the box's V.92 answering side is proven from Linux pppd, where the
exact chat string (`AT+PIG=0`) makes the Conexant request V.92 and the call
comes up V.92 (52000/32000). A real V.92-on-Windows test would need a
proper Conexant Win98 driver rather than the generic "Standard Modem".

## 1 800 BBS DIAL: bulletin boards from a terminal program (2026-11-16)

New: the box answers 1 800 227 3425 (with or without the 1) on every port,
whatever the port's service. That call carries octets instead of PPP, with
error control; the caller is asked `BBS:` and joined over telnet to the
board named. DESIGN.md 5b says how. The boards are those of the Telnet BBS
Guide: the box fetches the guide's archive once a day and makes its list
from it (`fw/bbsfetch.sh`, `fw/bbsguide.awk`); run on the PC against the
real site on 2026-11-16 the script took `ibbs111626.zip` and printed 1073
boards. A short list (`lists/bbs.txt`, 13 boards) is in the image.

Later the same day: every board has a telephone number of its own, 227 and
the login name on the keys of a telephone (seven digits, zeros behind a
short name, the next free number where two would share one: 116 of 1074
on that day's list), dialled on any port and joined to the board without a
prompt. 1 800 BBS DIAL stays as the operator and takes a name or a number.
A caller can no longer type an address and is no longer shown one. List
format `v90d-bbs 2`. `tel`: `?` in `also[]` is any digit (391 checks);
`srv`: 1094 checks; `ns_call`: 328 checks (a board's number with and
without the 1, a number no board has, an address typed at the prompt is
no board, no address on the caller's screen or on the page).

The BBS page in pages of 100 with a search (`ns_call` 337 checks,
`test_httpd` covers the address parsing). On the bench unit with the
guide's list: "Boards 1 to 100 of 1073", page 11 is 1001 to 1073 with no
Next, `q=tele` gives 9 boards, `q=2278353422` gives Telehack.

V.92 is on by default (`v92 = on` in the code, the image's first
configuration and the example); a saved configuration keeps its value.
On the unit, Conexant on port 2 from Linux (`ATDT2278353422`, V.92 on at
the port, the modem not asking for it): CONNECT, "v90d bulletin board
gateway", "Calling Telehack ...", Telehack's banner and command list, no
prompt in between. Not yet tried: a caller that asks for V.92 on a board
call, and the Windows VMs against a port with V.92 on.

Calls from SyncTERM 1.7 on Linux kept ending. Cause: busybox `nc -w 15`,
which the box used to reach a board, ends the second time 15 s pass
without an octet. Measured on the unit: a call to Telehack with one key
every 20 s ended after 42 s, "cleared locally". The box now connects with
its own code (`srv/bbsdial.c`, no limit on quiet time; `srv` 1123 checks).
With that build the same call with one key every 50 s was still up after
360 s when the test hung up, carrier detect steady throughout.

Tests on the PC:
- `dce`: octets on LAPM both ways (`test_call` case s).
- `tel`: numbers that answer beside the port's own (`test_also`).
- `srv`: the list's lines, the prompt's line editor, the telnet words
  (answers, refusals, split commands, CR NUL, no buffer overrun on 20000
  blocks of arbitrary octets), the number on a port of each service, a
  whole call with data both ways, every line of the shipped list.
- `ns_call` part 13, the real daemon: the number, a search, an unknown
  name, an address where nothing answers, a board (a script) reached by a
  name typed with a backspace, the board's telnet words kept from the
  caller, the line dropped when the board ends, the list fetched only after
  the first call and then in force on the page and at the prompt, the
  port's own number still PPP.
- `cfg`: the BBS page.
- `fw/tools/test_bbsfetch.sh`: the guide's directory as our list (a bare
  &, `][`, BBS inside a name, two boards of one name, a bad host, ssh
  entries left out), the archive names tried and their order, January, no
  clock, an error page instead of an archive.

On the box (HT802 V2.5A, this build installed through its Firmware page),
Conexant on PHONE 2, whose service is the plain internet:
- From Linux (pyserial as the terminal): `ATDT18002273425`, CONNECT, the
  greeting and `BBS:`; `?beer` listed 20forbeers; `telehack` gave "Calling
  Telehack (telehack.com:23) ..." and Telehack's banner and command list.
- From Windows XP (VirtualBox, modem by USB), HyperTerminal dialling
  1 800 2273425 through "Standard 56000 bps Modem": the box showed the call
  as V.90, 52000 down / 28800 up, LAPM. `?` listed the boards, `telehack`
  reached Telehack, readable in HyperTerminal's ANSI window. HyperTerminal
  did not show the greeting, which went out the moment the modems
  connected; the box now waits 1.5 s first.

- The list on the box: opened seconds after a restart, the BBS page showed
  the 13 boards of the image (the box had no clock yet and the fetch gave
  up; early failures are now tried again after a minute, not fifteen).
  With that build, 39 s after the restart the page showed 190 boards "and
  883 more": the guide's list of that day, 1073 boards, fetched and
  converted by the box itself.
- The owner then used the XP session by hand and was inside Dura-Europos
  (a board only the guide's list has) when the next firmware upload
  restarted the box and HyperTerminal showed NO CARRIER.

Not tried: a file transfer (Zmodem) through the bridge, a board that needs
8-bit binary, ANSI art at speed, a real vintage terminal, two such calls at
once, V.92 on such a call.

## 2026-11-16: the HT801, from flash dumps (no unit)

Two public HT801 flash dumps (crvallance/Grandstream-hax, hardware V1.0A
and V1.1C) were compared with this HT802's own backup: same partition map,
device trees equal except for the second SLIC, same SLIC description in the
factory data with one device instead of two (HARDWARE.md, BOARDS.md).

Changed, not yet in a release:
- The loader accepts part numbers 9611003610A and 9610003611C
  (`fw/loader/test`: 129 checks, 0 failures).
- `slic_present()` (`fw/drv`): a ProSLIC identity read twice and a register
  that keeps two written patterns (slic: 124 checks, 0 failures, host and
  ARM). The line driver asks it for port 2 before making `/dev/v90line2`;
  without that device the boot script already starts the daemon with one
  line.
- `make -C fw release` and `make -s -C fw test-release`: pass.

On the bench HT802 (this build through its Firmware page): both ports
present after the restart, and `ATDT2278353422` from the Conexant on PHONE 2
reached Telehack, V.90 52000 / 28800.

Not tested, because there is no HT801 here: everything on an HT801. Known
open points: the Ethernet PHY of V1.x boards (not named anywhere in a dump;
the boot loader sets it up and our kernel uses the generic driver, so it is
expected to work) and the SLIC's revision.

Because of the first: `v90nv where` now says `tried=yes` or `tried=no`, and
on a board that is not tried the boot script confirms a start only after
the network port has received a packet (`fw/startgood.sh`,
`tools/test_startgood.sh`: 7 checks, 0 failures, sh and busybox sh).

## 2026-11-16: a U.S. Robotics 5686, two calls at once, the echo delay

A second modem brand: U.S. Robotics 56K FAX EXT (model 5686, firmware
5.4.62), on PHONE 2, from Dial-Up Networking in Windows XP and from its AT
commands over a serial bridge.

- Windows XP, Dial-Up Networking: V.90 54666 down, LAPM, V.42bis, PPP up
  with MPPC both ways.
- Two calls at once (the first time): the Conexant on PHONE 1 from Linux
  pppd (V.90 53333 / 28800, PPP 10.64.64.2) while the USR call was up on
  PHONE 2 (V.90 56000 / 9600, PPP 10.64.64.3). Through the Conexant's link
  4 of 4 pings to the box and 3 of 3 to 1.1.1.1, 120 to 130 ms; no errors
  counted on either port.
- Earlier the same day this modem did not accept the dial tone and did not
  dial (`NO DIAL TONE` with X4, 18 s of nothing with X3), on both ports,
  while the Conexant worked on the same port. Later, with nothing changed
  on the box (daemon restarted, box restarted: the same), it accepted the
  dial tone in 2.3 s every time. Not explained; nothing in the box's
  recordings of the failed calls points at the box.

The upstream was only 9600 bit/s, the receiver's error -18 dB. From the
recording (`live/usr1.raw`, replayed with `live/replay`):

- The modem's own signal is clean: its probing tones stand 45 dB above the
  empty frequencies.
- The echo of TRN1d comes back 25 dB down (Conexant: 16 dB), 270 samples
  late on both modems, and from this modem with a flat tail of more than
  12 ms (low frequencies, the line transformer).
- The delay search took 308 for it, the canceller window began at 300 and
  took nothing away (Conexant: delay 261, 14 to 17 dB taken away). The
  same window offline gives the same 308; ten other windows give 270 or
  274.

Changed: the search correlates the change from sample to sample
(`modem/p34d.c`). The same eleven windows of the recording: 270 every time.
`test/test_p34.c` has the measured echo path as a line model, twelve noise
sequences: found on 0 of 12 before, 12 of 12 after (p34: 361 checks, 0
failures). `make check`: ALL HOST CHECKS PASS.

On the box (bench build), the same modem, same port: V.90 54666 / 26400.

### V.92 with the same modem

With V.92 on, this modem asks for PCM upstream by itself (`+PIG` is 0 from
the factory). The box's V.92 start-up gave up each time and the call fell
back to V.90, 39 s to CONNECT instead of 21. Four causes, found one behind
the other in recordings of the calls (`live/usr2.raw` on, replayed with
`live/replay`):

1. The V.92 engine has its own copy of the echo delay search, with the
   same fault: 300 taken for 270, nothing cancelled, the design gave up
   (error 6). Same change (`v92/v92_p34d.c`). After it: delay 262, 22 dB
   cancelled.
2. The modem waits 3.8 s into TRN2u for SUVd and then gives up. The box
   sent SUVd only when the CPd design was done, and the design, one step
   every four samples, took 4.5 to 7.5 s (9000 to 15000 steps). Now one
   step every two samples (2.2 to 3.7 s), and SUVd goes out 2.5 s into
   Phase 4 at the latest, the design done or not.
3. A first CPd that has the acknowledge bit set (CPu was in before the
   design was done) was answered with silence: 7 calls of 7. A first CPd
   without the bit: the modem went on, 9 of 9. The first CPd is now always
   sent without it.
4. In data mode what is left of our own low frequencies wanders under the
   caller's symbols: runs of tens of symbols 100 units off, the points 245
   apart, B1u with 66 to 213 wrong bits in four calls, and 71 symbols of
   4000 near a wrong decision in a call that went through. The receiver
   now follows the baseline with the error of each decision (an eighth).
   The four recordings replayed: B1u with 0 wrong bits; error 52 -> 17
   units RMS.

A wrong turn on the way: the refusals of point 3 looked like a limit on
the transmit power the design asks for (refused at 0.89 to 0.98 of the
limit, accepted at 0.58 to 0.84). With points 3 and 4 in, designs at 0.84
to 1.02 of the limit were accepted 6 of 6; the margin was taken out again.

Tests, each seen failing without its change (`test/test_v92call.c`, the
simulated caller and line in `sim/v92a.c`, `sim/v92chan.c`): 25b a weak
echo under its own slow tail (delay 349 and error 6 before, 263 after),
25c a caller that waits 3.8 s for SUVd and 3.5 s for CPd and answers a
first CPd with the acknowledge bit by silence, 25d a slow wander under the
data (47981 wrong bits of 308502 before, 0 after). v92call: 1054 checks,
0 failures.

On the box (bench build), the same modem on PHONE 2, `AT&F1`,
`ATX4DT6387825442`:

- upstream limit 33600 (the default): V.92 54666 / 33333, 5 calls of 5;
- upstream limit 48000: V.92 54666 / 40000, 5 calls of 5 (and 37333,
  37333, 38666 with the power margin that was taken out again);
- CONNECT after 22 to 25 s; the modem reports
  `CONNECT 54666/ARQ/V92/LAPM/V42BIS`;
- the line driver's counters for the port through a call of 34 s:
  pump_underruns 0, pump_overruns 0, stale_writes 0 (the design at two
  samples a step does not make the box late with one call up).

Later the same day, the Conexant attached again (PHONE 1, `AT+PIG=0`):

- V.92 on both ports at once, both orders. The USR in V.92 data on PHONE 2
  (54666 / 40000) while the Conexant started up and connected on PHONE 1
  (V.92 50666 / 28000); then the Conexant in a V.92 PPP session from Linux
  pppd (52000 / 26666, 3 of 3 pings through it) while the USR started up
  and connected (54666 / 40000). The line driver's counters of both ports,
  read twice a second through both: pump_underruns, pump_overruns and
  stale_writes stayed 0. The design at two samples a step does not make
  the box late with a call on the other port.
- The Conexant alone: V.92 49333 to 50666 down, 24000 to 26666 up, CONNECT
  after 18 to 20 s, 22 calls of 25. Of the other three, two fell back to
  V.90 (31 s) and one did not train; none of them was recorded. A loop of
  recorded calls then caught one fallback after six good calls
  (`live/cxfail.raw`): CPt is received, E1u is not, the channel estimate's
  residual is 717121 where a good call has about 5000, and the call
  retrains 2.2 s later, before Phase 4 begins. Echo delay (262) and
  canceller (22 to 32 dB) are the same as in the good calls
  (`live/cxa.raw`, `live/cxc.raw`). So it is not one of today's Phase 4
  changes. Looked at symbol by symbol (`V92_TRACE`): in that call the
  receiver's error on the second TRN1u never comes down (6.0e6 where a
  good call is at 3e5 after 0.5 s and 7e4 after 2 s), but it dips to
  2.6e6 and 2.9e6 at 2.20 s and 4.35 s into the state, and the two CPt
  that parse are received in the first dip. A receiver trained at the
  start that is right again every 2.15 s is one whose input slips a
  sample in that time: the caller's transmit clock is 58 ppm off ours in
  this call, where it is locked in the others. Starting the training up to
  3 samples earlier or later changes nothing. PCM upstream cannot be
  received from a caller that is not locked, and the retrain to V.90 is
  the right end of such a call; nothing changed in the box.

From Windows 98 SE Dial-Up Networking with the USR 5686: V.92 54666 /
40000, LAPM, PPP up as `guest` with MPPC both ways, a call of 66 s. (Two
calls before it connected at V.92, 40000 and 41333 up, and ended on a
wrong password; one before those dialled `16387825442`, Windows' long
distance 1, which the port's own number does not take.)

### A U.S. Robotics 56K of 1998 (x2 and V.90)

"U.S. Robotics 56K FAX EXT", product 00568603, flash 5.1.4 of 10/2/98,
options `V32bis,V.80,V.34+,x2,V.90`; on PHONE 2, driven over the serial
bridge. It names V.90 in its CM and goes into the V.90 start-up. Every
call ended after 28 s with our busy tone; the modem's `ATI6`:
"Disconnect Reason is Unable to Retrain". Recordings `live/x2a.raw`,
`live/x2b.raw`; `live/replay` now prints the DIL descriptor and the CPt.

1. 0.7 s into our MP the modem falls silent and sends Tone A (2400 Hz
   with its 1800 Hz guard tone, 80 % / 20 % of the power). `p34d` listened
   for Tone A in data mode and in a rate renegotiation only, went on
   sending MP and failed with "no CP" 15 s later. It now listens through
   Phase 4 as well. Test 17 in `test/test_p34.c`: before, state FAILED
   error 7, 14933 ms after the tone began; after, RETRAIN after 66 ms.
   On the box: the retrain is taken up and the call connects,
   `CONNECT 28800/ARQ/V34/LAPM/V42BIS`, 2 calls of 2 (and one more with
   `v92 = off`, the same).
2. Why it gave V.90 up: it asks for the same training as the 5686 (DIL
   N 197, H 10, CPt drn 16, Sr 1, ld 3, a1 64, nine points 30..89; only
   that it sends the six constellations one by one and a codec
   constellation equal to them), and through our TRN2d and MP it goes on
   repeating CPt (1788 bits each) and never sends CP. The 5686 does the
   same for 1.1 s and then sends CP. Our TRN2d was 400 data frames, 0.3 s
   (9.4.1: at least 2040 T). With 1200 frames, 0.9 s, the modem answers
   with CP. Test 18 (a caller that needs 0.8 s of TRN2d): red with 400,
   green with 1200. p34: 372 checks, 0 failures (two older checks moved
   with the later start of data mode: a bit count and the time of a
   renegotiation).
   On the box, 4 calls of 4 at V.90 with LAPM, CONNECT after 20.5 to
   21.2 s, 28800 up each. Downstream as the modem reports it: 54666,
   54666, 56000, 56000; as the box lists it: 54666, 50666, 56000, 54666.
   The two do not agree call by call. Four more calls, recorded
   (`live/x2r1.raw` to `x2r4.raw`): in three the modem's CONNECT, its CP
   (drn 21, 65 points) and the box's list all say 54666. In the fourth the
   CONNECT says 54666, and 6.7 s after its first CP the modem sends one
   with drn 18 (46 points), a rate renegotiation of its own; the box
   follows and lists 50666, and the modem's `ATI11` has "Speed Shifts
   Up/Down 0/1" for that call and 0/0 for the others. The CONNECT message
   is the rate at the start, the list has the rate at the end.

The Conexant against the longer TRN2d (PHONE 1): V.90 with `AT+PIG=1`,
3 calls of 3 (49333, 50666, 50666 / 28800); V.92 with `AT+PIG=0`, 3 of 3
(49333, 50666, 50666 / 25333).

The 5686 (flash 5.4.62) back on PHONE 2, the same build: V.90 with
`v92 = off`, 3 calls of 3, 54666 / 26400; V.92, 3 of 3, 54666 / 40000,
40000 and 38666.
x2 itself is not implemented: there is no published description of it.

The port's own number with a 1 in front: a ten-digit number is now taken
with the long distance 1 as well (`tel/tel.c`; Windows dials it so unless
told otherwise, and the call list showed `16387825442` as a wrong number
three times that day). `test_trunk_one`: red before (1 failure), tel: 396
checks, 0 failures after.

### 0.4.0

Host and ARM suites on the tree of the release: "ALL HOST CHECKS PASS",
"ALL ARM CHECKS PASS". `make -C fw release`, `make -C fw test-release`:
11 tests OK, loader: 129 checks, 0 failures. The image
(`v90d-ht802fw.bin`, SHA-256 922729a3...b022ce9c) uploaded on the unit's
Firmware page: back after about 100 s as 0.4.0, start confirmed, no
telnet. On it, the 1998 U.S. Robotics on PHONE 2: V.90 54666 / 28800
twice, once dialled as `16387825442`. The Conexant and the 5686 were not
attached for this last check; they were tested on the same sources in the
bench build (above).

## 2026-11-16: one copy of the echo delay measurement and of the tone watch

No change in what the modem does; the aim was that a correction is made
once.

- `modem/ecdly.[ch]`: the history of what was sent and the echo delay
  measurement, out of `modem/p34d.c`, `v92/v92_p34d.c` and `modem/v34dx.c`,
  which each had a copy (the 5686's correction earlier that day went into
  two of them by hand). `modem/tonedet.[ch]`: the retrain tone watch, out
  of the same three.
- Checked three ways. The engines' own tests as before (p34: 372 checks,
  v34dx: 690, v34call: 296, v92call: 1054, answer_v92: 231, all 0
  failures). A test of the two modules alone, `test_ecdly`: 1019 checks, 0
  failures; with the search's index and the tone threshold spoiled on
  purpose it gave 6 failures. And 13 recordings of real calls (Conexant,
  5686 and the 1998 U.S. Robotics; V.34, V.90 and V.92), each replayed
  with both port settings before and after: 26 outputs, all identical
  octet for octet.
- `test/test_p34.c`: the mid-call events (renegotiation, cleardown, the
  line getting worse) are now timed from the moment data mode is reached,
  not from the start of the run. With TRN2d made a second longer for a
  trial (2600 data frames, data mode at 4.00 s instead of 2.95) the test
  still gives 372 checks, 0 failures; the day before, 0.6 s more had
  broken three of its checks. Two thresholds went down by under 2 % with
  the run 50 ms shorter (270000 to 265000 and 300000 to 295000 bits).
- Bulletin board calls: an entry or a name that leads to one of the box's
  own addresses, to a link-local or a multicast address is not connected
  to (`bbsdial_is_self`; the point the review of 0.4.0 raised). Test first:
  6 failures (the three address classes, the own address, a listener on
  the own address that was reached), then srv: 1144 checks, 0 failures.


### 0.4.1

Host and ARM suites on these sources: "ALL HOST CHECKS PASS", "ALL ARM
CHECKS PASS" (run before the version number was set; only `fw/VERSION` and
this file changed after). `make -C fw release`, `make -C fw test-release`
as 0.4.1: 11 tests OK, loader: 129 checks, 0 failures. The image
(`v90d-ht802fw.bin`, SHA-256 4d7371d3...41a6ca68) uploaded on the unit's
Firmware page over 0.4.0: back after about 100 s as 0.4.1, starts not
confirmed 0, no telnet. On it, the 1998 U.S. Robotics on PHONE 2:

| call | result |
|---|---|
| as it comes | `CONNECT 54666/ARQ/V90`, the box's list: V.90, 56000 / 28800 |
| `ATS32=66` (no V.90, no x2) | `CONNECT 31200/ARQ/V34/LAPM/V42`, the list: V.34, 31200 / 31200 |
| 227 835 3422 | V.90, "Calling Telehack ...", Telehack's greeting and prompt |

After the release was published the 5686 (flash 5.4.62) went back on the
bridge, the same image on the box: V.92, 3 calls of 3, `CONNECT 54666`, the
box's list 54666 / 40000, 38666 and 38666. The Conexant was not tried on
this image.

## libv90modem: the modem as a library of its own, Phase 1 (2026-11-16)

The modem (`modem/`, `v32/`, `v34/`, `v90/`, `v92/`), V.42 (`v42/`), the line
simulator (`sim/`), the modem tests, the recorders and benches moved into
`libv90modem/` (`git mv`, the same relative paths), with a public API in one
header, `include/v90modem.h` (0.1.0, not stable before 1.0). v90d builds
against it (`libv90m.mk`): the daemon links `libv90modem.a`, in which only the
`v90m_*` functions are visible; the tests link the library's dev archive
(internals and simulator). Nothing was to change in what goes on the line.

Proof, before (819d38e, a clean worktree) and after (this branch), same PC:

| | before | after |
|---|---|---|
| `make check` | 107 programs, 2,835,154 checks, 0 failures, ALL HOST CHECKS PASS | the same 107 with the same counts, plus `api: 74 checks, 0 failures`; ALL HOST CHECKS PASS |
| `make check-arm` | 106 programs, 0 failures, ALL ARM CHECKS PASS | the same 106 with the same counts, plus the API test; ALL ARM CHECKS PASS |
| `make bench-v90` | transmit hash 8e3136e9 (host, qemu-arm) | 8e3136e9 by the internals (host, qemu-arm) and by the public API (static, shared, qemu-arm) |
| replays: 106 recordings of `live/` x 8 configurations | 848 outputs, 394 V.92 per-sample trace checksums, 371 bit dumps | the same 1613 files, byte for byte |
| `make -C fw test-release` | passes | passes; release image built against the library (`v90d-ht802fw.bin` 0.4.0, 2523144 octets, through the loader: 22 checks, 0 failures) |
| ARM daemon, stripped | 573248 octets | 577344 octets |
| ARM instructions per second of V.90 data mode (qemu) | 39,605,966 (`bench_v90`) | 40,170,131 through the API (`bench_api`): +1.4 %, about 70 per sample |

New with the library (no "before"): the API test (`test/api/test_api.c`:
configuration and its errors, memory of the caller's and full of rubbish,
G.711, a V.22bis call through the API, two modems in one process, blocks of
1, 7 and 160 samples giving the same samples on the line, V.42 alone), the
public-API bench, the example `examples/answer_file.c` (the Conexant V.92
call `cxa.raw` gives `CONNECT 50666/25333 V.92 at 24.805 s` on the host,
static and shared, and cross-built under qemu-arm), the replay tool
`v90m-replay` (modem and V.42 without telephony: the transmit matches the
recording to the bit until data mode on cxa, cxc, v34a, v21a, usr6, usr7,
x2r2, x2b), install / uninstall / pkg-config, a cross build with
`CC=arm-linux-gcc`.

Found on the way: the test of `dce_init` on a dce_t full of rubbish
(test_call (l)) failed when the modem was allocated by the first call; the
modem and V.42 memory are part of the dce_t again (`v90m_init` on it).

## main (0.4.1) merged; its modem changes in libv90modem (2026-11-17)

main since the split (819d38e..4799c4c: 0.4.0, one copy of the echo delay
measurement and of the tone watch, bulletin boards that lead back into the
box, 0.4.1) merged into this branch. The modem's part (`modem/ecdly`,
`modem/tonedet`, `p34d`, `v92_p34d`, `v34dx`, their tests) lives in
libv90modem now: there on its branch `sync-0.4.1` (8eee1b3), applied as
main has it. Here: `srv/bbsdial.[ch]` and its tests, `fw/VERSION`, DESIGN
and PROGRESS as main has them; `live/replay.c` and the top Makefile stay as
this branch has them (the trace is libv90modem's `tools/trace.c`, which
reads `ecd.delay` now, as does `test_srv`'s V.92 call).

Proof, before (main at 4799c4c, a clean worktree) and after (this merge
against libv90modem 8eee1b3), same PC:

| | before | after |
|---|---|---|
| `make check` | 108 programs, 2,836,310 checks, 0 failures, ALL HOST CHECKS PASS | every one of the 108 lines the same (`ecdly: 1019` among them, now in the library), except `tel: 396` -> `401` (this branch's `v90d -o`, aa91646; main did not touch `tel/`); plus 18 of the library's calling side and this branch (anstone, api 187, dce_orig, orig, orig_v32, orig_v34, orig_v90, p2call, p2v90, srv_orig, test_acdet, test_caller, v8orig, v90a, v90adec, v90arx, v90arx_robust, v90dil): 126 programs, 2,839,215 checks, 0 failures, ALL HOST CHECKS PASS |
| `make check-arm` | 107 programs, 0 failures, ALL ARM CHECKS PASS | the same 107 lines (tel as above), 125 programs, ALL ARM CHECKS PASS |
| `make bench-v90` | transmit hash 8e3136e9 (host, qemu-arm) | 8e3136e9 by the internals and by the public API (static, shared, qemu-arm) |
| replays: 106 recordings of `live/` x 8 configurations | 848 outputs, 394 V.92 trace checksums, 230 bit dumps, 48 + 93 data dumps: 1613 files | the same 1613 files, byte for byte |
| `make -C fw test-release` | passes (11 tests OK, loader: 129 checks, 0 failures) | the same; `v90d-ht802fw.bin` 0.4.1, 2552040 octets (2517992 before) |
| ARM daemon (`srv/build/v90d-arm`) | 569152 octets | 630592 octets: the calling side of the library and `v90d -o` |

The bench hash is the one of 0.3 / Phase 1: 0.4.x did not change what the
answerer sends in that simulated call. `V90M_MEM_SIZE` went from 100 to
112 KiB in the library (the larger echo history), so `dce_t` is 12 KiB
larger.

## 2026-11-17: Bell 103

A caller at 300 bit/s from North America is a Bell 103 modem more often
than a V.21 one (the 5686's default at 300, S27 bit 0 clear). On 0.4.1 such
a call failed: `AT&N1` on the 5686 said CONNECT, the box's list said "none,
negotiation failed" after 35 s. The caller took our signal for its carrier
and sent its mark, 1270 Hz; the box sat in the V.22bis wait and then gave up.

Now (`modem/answer.c`, DESIGN.md 4.1): a steady 1270 Hz from a caller
without V.8, heard behind a band-pass that keeps our own signal out
(`modem/bell103_bpf.h`, from `tools/gen_v21_bpf.py bell103`), switches to
300 bit/s FSK on Bell 103's tones with the V.21 modulator, demodulator and
start-stop path. On when V.21 is on for the port. Shown as "Bell 103"
(`bell103` in the JSON; status value 8, appended).

Test first, `test/test_answer.c` with a plain Bell 103 caller in it: 6
failures (the answering modem stuck in the V.22 phase, as on the box), then
0. A port without V.22: taken for V.21 at first (2 failures: the V.21
receiver took 1270 Hz for its carrier), then 0. answer: 46 checks, 0
failures. `test_status` wanted "unknown" for value 8: now for 9. Host suite:
"ALL HOST CHECKS PASS".

On the box (the image built from these sources, still numbered 0.4.1), the
5686 with `AT&B1` and a fixed rate, 227 835 3422:

| call | the modem | the box's list | received |
|---|---|---|---|
| `&N1` | `CONNECT` | Bell 103, 300 / 300 | Telehack's greeting, 709 octets as sent |
| `&N1S27=1` | `CONNECT` | V.21, 300 / 300 | the same, 711 octets |
| `&N2` | `CONNECT 1200/ARQ` | V.22, 1200 / 1200 | the same, 1204 octets |

Without `&B1` the 5686 follows the line rate on its serial port: at 300 the
bridge then reads rubbish and no command reaches the modem until the call
ends (the box was restarted to end one). A call that connects and sends no
PPP to the internet number ends as "error" in the list: PPP gave up, which
is what the earlier 1200 bit/s test call showed.

## 2026-11-17: Bell 212A

Looked up after the Bell 103 work: single-chip modems of the time detect
the answering modem in Bell 212A (and 103) originate mode by 2225 Hz within
10 Hz (TDK 73K222 data sheets), and manuals give the answer tone as the
difference between the two (`B0` 2100 Hz, `B1` 2225 Hz). Our V.22bis answer
sequence has 2100 Hz and then 2250 Hz in it: a caller that strict never
answers it.

Now (`modem/answer.c`, DESIGN.md 4.1): after 4 s of the V.22bis answer
sequence without an answer from a caller without V.8, 2225 Hz in place of
the unscrambled ones for the next 4 s, the V.22bis modem listening all the
while; then V.21 as before. A call that began during the tone is shown as
"Bell 212A" (`bell212a`, status value 9). On when V.22 or V.22bis is on.

Test first, `test/test_answer.c`: a 212A caller that takes nothing but
2225 Hz within some 10 Hz for an answering modem. 6 failures (it stayed
silent, nothing connected), then 0: connected at 10.96 s, 1200 bit/s, text
both ways. Not offered: no call, as before. answer: 54 checks, 0 failures;
test_status: 611 checks, 0 failures. Host suite: "ALL HOST CHECKS PASS".

On the box, the 5686, 227 835 3422, Telehack's greeting received as sent
in every call:

| image | the modem | the box's list |
|---|---|---|
| these sources | `&N1` | Bell 103, 300 / 300 |
| these sources | `&N1S27=1` | V.21, 300 / 300 (CONNECT after 21.6 s) |
| these sources | `&N2` | V.22, 1200 / 1200 |
| these sources | `&N2S32=0` (no V.8) | V.22, 1200 / 1200 |
| 2225 Hz from the start (`V22_BELL_MS` 0, a build for this test only) | `&N2`, twice | V.22: the 5686 does V.8 at 1200 too, the tone was never reached |
| the same | `&N2S32=0`, twice | Bell 212A, 1200 / 1200, `CONNECT 1200/ARQ` |
| the same | `&N1` | Bell 103, 300 / 300 |

So a real modem has started 1200 bit/s against our 2100 Hz and 2225 Hz
with no unscrambled ones ever sent, and the box's receiver has found its
scrambled ones under the echo of the tone. What that does not show: a
modem that calls as 212A and nothing else. The 5686 would have connected
to the V.22 sequence as well, and none of the other kind has been on the
line. The box was left on the image from these sources (4 s before the
tone).

ARM suite on these sources, after the commit: "ALL ARM CHECKS PASS"
(answer: 54 checks, 0 failures).


### 0.5.0

The sources of the two sections above, `fw/VERSION` 0.5.0. Host and ARM
suites passed on them before the version number was set (only `fw/VERSION`
and this file changed after). `make -C fw release`, `make -C fw
test-release`: exit 0 (32 tests OK with 1 skipped, twice; 18 tests OK;
loader: 129 checks, 0 failures). The image (`v90d-ht802fw.bin`, SHA-256
bdb07007...fe7a1de4) uploaded on the unit's Firmware page: back as 0.5.0,
no telnet. On it, the 5686 with `AT&B1`:

| call | the modem | the box's list |
|---|---|---|
| `&N1`, 227 835 3422 | `CONNECT`, Telehack's greeting | Bell 103, 300 / 300 |
| `&N1S27=1`, the same | `CONNECT`, the greeting | V.21, 300 / 300 |
| `&N2`, the same | `CONNECT 1200/ARQ`, the greeting | V.22, 1200 / 1200 |
| as it comes, the internet number | `CONNECT 54666/ARQ` | V.92, 54666 / 40000 |

Bell 212A was not called on this image: the 5686 answers the V.22 sequence
before the 2225 Hz tone begins. Its calls were made on the test build
described above. Not tried on this image: the 1998 U.S. Robotics, the
Conexant.

## main (0.5.0) merged; Bell through libv90modem's API (2026-11-17)

main 4799c4c..9484fb7: Bell 103 and Bell 212A answering. Its modem files
(`modem/answer.*`, `v21.h`, `bell103_bpf.h`, `test_answer`,
`tools/gen_v21_bpf.py`) are not in this tree: they are in libv90modem's
sync-0.5.0 (629ae89), with API 0.2.0. Here, main's dce / srv / cfg changes
reach Bell through the public API instead of `ANS_MODE_*`:
`srv` offers `V90M_MOD_BELL212A` with V.22 and `V90M_MOD_BELL103` with V.21
(the calling mode, `v90d -o`, masks both out as before); `mod_of` maps
`V90M_CONN_BELL103` / `_BELL212A` to the status page's; `dce` starts V.42
straight in start-stop for `V90M_CONN_BELL103` as for V.21. cfg, srv/main,
README, fw/VERSION 0.5.0 as main has them.

Proof, same PC, against main 9484fb7 built on its own (recordings copied
once and replayed from that copy on both sides):

| | main 9484fb7 | this branch + libv90modem 6036127 |
|---|---|---|
| `make check` | 108 programs, 2,836,351 checks, 0 failures | the same 108 lines (`answer: 54`, `test_status: 611` among them), except `tel: 396` -> `401` (this branch's `v90d -o`); plus the 18 of the calling side and this branch (`api` now 237): 126 programs, 2,839,306 checks, 0 failures |
| `make check-arm` | 107 programs, 547,858 checks, 0 failures | the same 107 lines but `tel`; 125 programs, 550,723 checks, 0 failures |
| `make bench-v90` | transmit hash 8e3136e9 | 8e3136e9 (internals and API) |
| replay, 106 `live/*.raw` x 8 `live/*.conf` | 1613 files | identical octet for octet (the two manifests compare equal) |
| `make -C fw release`, `test-release` | built, passes | built, passes, the same lines |

## main (0.5.1) merged; the modem pinned to libv90modem 0.3.0 (2026-11-20)

main 9484fb7..35df47b. The 0.5.1 changes were two V.92 answering fixes and
the Phase 2 speed-ups, all modem: they live in libv90modem 0.3.0, so main's
`modem/*` and `v92/*` edits are not taken here; the modem comes from the
library. Only the non-modem commits cross over: the HT802V2 board/firmware
notes (BOARDS.md, README.md, FLASHING.md) and fw/VERSION 0.5.1.

The library is now pinned, not a loose checkout next door. `fw/setup.sh`
fetches a released source tarball (a GitHub release asset, SHA-256
a9fef7ca..., so its bytes cannot change under us) and unpacks it at
`libv90modem/`, where `libv90m.mk` looks by default; a developer builds
against a working copy with `make LIBV90M=/path`. To move the modem, bump
`LIBV90M_VER` / `LIBV90M_SHA` in setup.sh.

Proof, built against the pinned libv90modem 0.3.0 (the daemon links its
public API only):

- `make check`: ALL HOST CHECKS PASS (the library's suite first, then
  v90d's), 0 failures.
- `make -C fw release`: built `v90d-ht802fw.bin` (v90d ht802 0.5.1); release
  through the loader 22 checks, 0 failures.
- `make bench-v90`: transmit hash 8e3136e9, the same as 0.5.0 and 0.5.1,
  reported both through the library's internals and its public API. The
  modem on the line is byte-for-byte what shipped.

Not re-run here, on purpose: `make check-arm` and `make -C fw test-release`.
The modem sources are byte-for-byte libv90modem 0.3.0, which went through
check-arm on its own and shipped in v90d 0.5.1; the loader and boot models
are unchanged by this restructure and were green in 0.5.1. The release
image was not put on the unit (the 0.5.1 image, the same modem, was).

## MS-CHAPv2 and MPPE encryption, per port (issue #6) (2026-11-22)

A caller that sets "require data encryption" (Windows dial-up) is now served.
The PPP stack gained MS-CHAPv2 (RFC 2759) and 128-bit stateless MPPE (RFC
3078/3079), clean-room from the RFCs, in `ppp/ppp_crypto.c` and
`ppp/ppp_mppe.c` with the negotiation and data path in `ppp/ppp.c`. Off by
default; a new `[ppp] encryption` switch turns it on.

What it does. On, the server offers CHAP algorithm 0x81 (MS-CHAPv2),
verifies the 49-octet response against MD4(password), derives the MPPE master
and session keys from the authentication, and negotiates CCP option 18 with
128-bit stateless MPPE in place of Deflate/MPPC. Each IP datagram then carries
a 2-octet MPPE header (FLUSHED, ENCRYPT, 12-bit coherency count) and RC4
ciphertext; the receiver rekeys forward across lost packets.

Fails closed (from the security review). Encryption is a requirement, not best
effort: if MPPE is not agreed in both directions the link ends
(PPP_END_ENCRYPT_REQUIRED) rather than falling back; no cleartext IP is sent
or accepted meanwhile; plain MPPC is not Ack'd when MPPE was asked for. MPPE is
keyed once per authentication - a CCP re-open after it has run ends the link
rather than restarting RC4 from the master key (which would reuse the
keystream), and a decrypt error drops the packet instead of renegotiating.

Per port. `encryption` is port 1 and `encryption2` is port 2, the only part of
`[ppp]` set per port (the users are shared). It is held in `cfg_port_t` and
carried through `port_take`/`port_give`/`cfg_for_port` like `ppp_peer2`, so the
settings page (a "PPP encryption" control on each port's Modem tab), the config
file, srv's per-port view, and `/status.json` (`ppp_encryption`, and a
per-direction "MPPE" reading) all became per port with no change to srv.c.
MPPE requires MS-CHAPv2, so a port that requires it needs a `user`.

Interop: NOT tested against a real client. Verified against the RFC 2759/3079
vectors and a modeled Windows peer only. The real pppd we use as a black box
covers Deflate/MPPC, not MPPE. The Netstalgia/nsdiald dial-up ISP (which builds
`ppp/` and `dce/` from a pinned copy) is adopting it at 6c1277d as
off/require, marked experimental, and will send back real Win9x/XP dial-in
results.

Proof (host, and ARM under qemu):

- `make check`: ALL HOST CHECKS PASS; `make check-arm` paths (ppp/cfg/srv
  test-arm): ALL ARM TESTS PASS, 0 failures.
- New vectors and paths: `ppp/test/test_crypto.c` 20 checks (NT-Response,
  authenticator, master/session keys), `test_mppe.c` 1815 (RFC RC4 sample,
  128/40/56-bit round trips, loss, plaintext refused), `test_ppp.c` 1825 (now
  with MS-CHAPv2), `test_ccp.c` 84786 (a full MPPE session, fail-closed both
  ways, a CCP re-open ending the link).
- Config/UI/status: `cfg` test_config 645, test_settings 769, test_status 623,
  0 failures - parse/write/validate of `encryption`/`encryption2`,
  `cfg_for_port`, per-port render and apply, the JSON/HTML.
- `make -C srv v90d` and `v90d-arm`: both built clean.
- Commits on main: 9e980da (crypto), 817d26d (MPPE engine), dd6bada
  (MS-CHAPv2), 622f205 (CCP integration, fail closed), 6c1277d (no re-key on
  CCP re-open), 1dfdda3 (config/settings/status, per port).

Not done: no real-client interop; stateful MPPE and 40/56-bit are not offered
(the engine does 40/56, the negotiation offers 128 only); no opportunistic
("allow") mode - the switch is off or require. Not put on a unit yet.

## 0.8.0: the encryption audited, and made to interoperate (2026-11-22, later)

Before the release, the MS-CHAPv2/MPPE work above was audited by two
independent reviewers (one on `ppp/` against RFC 2759/3078/3079 with a
hostile peer in mind, one on the config, settings and daemon wiring), and
in parallel the Netstalgia/nsdiald session, which builds `ppp/` from a
pinned copy, compared it with the Linux kernel's `ppp_mppe`. Both found the
same thing first, and it was serious: **the MPPE key schedule did not match
what real peers do**, so encryption would have authenticated and then
carried nothing. The self-consistent tests could not see it, since both ends
of every test used our own `ppp_mppe`. What the entry above calls the
session key was right only as the *initial* key.

Fixed (ppp/):

- Key schedule as the kernel (and so pppd and Windows) has it: initial key
  GetNewKeyFromSHA(start, start) with no RC4; every key change is
  GetNewKeyFromSHA(start, current) RC4-encrypted under itself (RFC 3078 7.3);
  stateless mode changes the key before every packet including the first
  (count starts at 4095). `ppp_mppe_new_key` is now
  `ppp_mppe_initial_key` and `ppp_mppe_change_key`.
- Receive window 2047 (the kernel's), replays dropped; was 256 and stuck for
  good after a longer loss.
- Fail closed everywhere, not only at CCP open: `comp` cleared when
  encrypting (CCP had asked for Deflate first); the link ends if the peer
  Protocol-Rejects CCP, if the login gave no keys, or if MPPE is not up
  within CCP's retry span; a refused MS-CHAPv2 ends the link at once when
  encrypting instead of falling back to PAP; `cw_mppe` no longer survives a
  refused MPPE request (could key MPPE toward a peer that never agreed);
  option 18 length checked on every copy; MPPE frames must fit the peer's
  MRU with their 4 octets; short MPPE frames dropped without a Reset-Request.
- A regression of 0.7.8's MS-CHAPv2 code, for unencrypted calls: a Nak
  toward MS-CHAP v1 now falls back to CHAP-MD5 before PAP.
- Failure carries `C=` (RFC 2759 6); passwords decoded from UTF-8 to UTF-16
  for the NT hash; unknown users hashed too (same timing); keys and
  intermediates wiped with a store the compiler cannot drop.
- Test client: MS-CHAPv2 responses were resent as 17 octets; its buffers are
  no longer shared between clients.

Fixed (daemon, config): a Dreamcast port with encryption on was asked for
MS-CHAPv2 (the console could not log in) -- one `full_options()` rule now
decides V.92, x2/K56flex, compression and encryption for every service; the
status said compression was on while encryption had turned it off; an
encrypting port's TUN MTU is 4 lower; settings errors on the Port 2 tab name
`encryption2`; the duplicated [modem]/[modem2] cross-checks are one function.

Proof:

- **Against a peer not written here:** `srv/test/ns_pppd.c` gained a sixth
  call: the daemon, switched to "PPP encryption: on" through its own
  settings page, answers pppd 2.5.1 with `require-mppe-128`. pppd accepts
  our MS-CHAPv2 authenticator response, both sides agree
  `<mppe +H -M +S -L -D -C>`, pppd logs "MPPE 128-bit stateless compression
  enabled", and 100000 octets down and 30000 up cross the kernel's
  `ppp_mppe` and ours over TCP, both files whole. ns_pppd 127 checks, 0
  failures; the five compression calls unchanged.
- Per-packet vectors from a separate Python reference (hashlib SHA-1, its
  own RC4) that first reproduces RFC 3079's own samples: test_crypto 28,
  test_mppe 1837 (window, replay), test_ppp 1839 (fallback order,
  encryption with a refused or keyless login, Failure format), test_ccp 84820
  (MRU, short frames, the stale request, Protocol-Reject, the deadline).
- srv 1180 (the Dreamcast-with-encryption call, shown to fail on the old
  code), cfg 645/773/623.

Still not done: a real Windows client (pppd + kernel stand in for it); no
opportunistic mode; 40/56-bit not offered.

## Issue #4 (HT802 V1.6A) audited, before 0.8.0 (2026-11-22, later)

The V1.6A work of 0.7.1-0.7.8 (board gate, flash chips and PHYs, the Si3226x
and both its channels) was audited by two independent reviewers, one on the
SLIC bring-up and line driver, one on the flash, loader gate and kernel
config. Each finding was checked against the code before anything changed.
Nobody here can run the V1.6A again, so the rule was: the success-path order
and timing on the chip stay exactly as the owner confirmed them; fixes go to
failure paths, to coordination, and to tests that pin the order down.

Fixed, SLIC and line driver:

- **The two ports interleaved SPI on the dual part.** An access is three
  octets, each its own chip-select frame; only an octet was atomic, and on
  the V1.6A both port threads talk to one chip on one select (with
  preemption on). A per-select lock now spans the three octets; never
  contended on the V2.5A.
- **Port 2 never recovered.** If its channel lost its settings, its thread
  went down and straight back up on the next pass without anything being
  started again, warning every second forever; and a channel 1 that failed
  to start was never retried unless channel 0 also reset. Port 2 now asks
  port 1's thread to start the whole part again (5 s then 60 s back-off, as
  a single-channel port retries itself); restarting channel 1 alone would be
  the order that trips the other converter. The owner never resets the part
  under the other port's call. A channel 0 failure no longer holds the part
  in reset when channel 1 is in service, so a bad port 1 line no longer
  takes port 2 with it.
- **The converter could be left starting.** After arming, four bus-error
  returns skipped turning it off and four writes (the RUN write among them)
  were unchecked; all now go through `converter_off`.
- Dual detection reads the family twice, up to three tries (one spoiled
  read made a dual board look like a one-port one).
- The dual order moved from the kernel into `fw/drv/slic.c`
  (`slic_dual_init`), so it is host-tested: two simulated channels on one
  clock and one log; the test fails on the naive order (shown) and checks
  each failure policy. Family decode shared; the stale "shared converter"
  comments (the first, wrong theory) corrected; `slicpoke`'s limit on the
  dual part documented.

Fixed, flash and loader:

- **A flash timeout during start-up gave a reset loop.** Only an unknown chip
  ID handed the box back to stock; a chip or controller that did not come
  ready (new with 0.7.2's quad-enable writes) returned with nothing erased,
  so the stock loader would start our core forever. Any start-up failure now
  hands back, and the raw hand-back first waits for a busy chip (it would
  have ignored the write enable). Test: a GD chip whose status write outlasts
  the driver's 4 s; fails on the old code (shown).
- Every erased sector is read back: a block-protected sector is not erased
  and the chip does not say so.
- The kernel config built the Motorcomm PHY driver for the V2.5A's YT8512H,
  which that driver does not match (the generic driver always ran it):
  dropped. Its comment, and the stated reason for dropping Micrel (a path
  that does not run without an "rmii-ref" clock), rewritten to what is known.
- BOARDS.md called the V1.6A "Tested", whose definition is a dial-up call on
  a unit; it is now "Confirmed by its owner", and the loader keeps it under
  the untried rule (start confirmed only once the network is heard).

Looked at and left, with reasons: the RAM pre-ready check (it is what
recovers after a lost write), finer sleeps, the double reset at probe,
overlapping calibrations (timing proven on the unit), merging the two
families' general tables (most values differ), GD's SR2 write (the vendor's
sequence), QPI across reset, 64 KiB erases, the 8-bit CRC table (small gain
in the loader), and N25Q, which no unit is known to carry and the simulator
does not model.

Proof: fw/drv slic 182 (dual order and failures, converter bus error), qspi
102 (protected erase), loader 144 (the reset loop), host and ARM; the
v90line driver compiled against the 6.6.63 tree with W=1, no warnings; the
release image built.

## 0.9.0: the Obihai OBi200 (2026-11-23)

v90d runs on a second maker's box. The OBi200 keeps the maker's Linux 2.6.30
kernel (Marvell's, never published) and the community firmware's system
(arrynrob13), and v90d replaces only the maker's application in the box's
`/obi` flash part. Everything is in `fw/obi/` and its README; the board
section is in BOARDS.md.

On the unit (OBi200 hardware 1.4): a U.S. Robotics 56K 5.4.62, reached over
the owner's serial-over-TCP bridge, called in at V.92 54 666 / 33 333 bit/s,
LAPM with V.42bis, no retrains; PPP up (PAP), pings to the box and to the
internet through its NAT, Deflate both ways. Installed through the OBi's own
update page; updated through v90d's Firmware page (checked, /obi written,
restarted by itself, the flash read back equal to the file); the way back
(the community firmware file on the same page) tested octet for octet on a
copy of the unit's flash and in `fw/obi/test_obinv.py`.

How (all read off the unit; the maker's binaries studied, none shipped in
our sources):

- `fw/obi/obiline.c`, the line in user space. The SLIC (Si3226x rev C,
  channel 0) is reached through the maker's module's register ioctls:
  `ps_bus` got `reg_rd`/`reg_wr` callbacks, so `fw/drv/slic.c` runs
  unchanged, patch-free, after a hardware reset through the module (the part
  is held in reset from boot). Audio is the module's `/dev/pcm`, 10 ms
  blocks of both channels, driven as the maker's application drives it
  (both channels started, poll, no prefill); the module stops a channel for
  good on an overrun, so the loop is SCHED_FIFO 80 and the threads that share
  its lock run at 70, above the daemon. The daemon gets the line over a
  packet socket: `seq:PATH` in srv/line.c, the `dev:` protocol unchanged.
- The kernel has no TUN driver: `srv/tun.c` falls back to the kernel's PPP
  unit over an async PPP channel on a pseudo-terminal, framed with
  `ppp/ppp_hdlc` (`tun_read`, `tun_write`, `tun_close`). No nftables either:
  `srv/nat.c` uses the system's iptables (1.4.4) on kernels before 3.13, the
  same plan as two chains put first in the nat table, all or nothing.
- `fw/obi/compat26.c`: `rename` and `accept4` for 2.6.30.
- `fw/obi/obi`, the launcher: the maker's modules, the LAN port (`eth1` is
  made with `unicfg -gwadd` and the unit's MAC from the factory block, as
  the maker's application does), the line, the daemon, the lights (power as
  the maker leaves it, read off its LED module; phone on while off hook),
  restarts.
- `fw/obi/mkobifw.py` builds the OBi firmware file from the owner's base
  file and rebuilds that base file octet for octet; the update page's keyed
  MD5 checks are made with a string found in the base file's own
  application, not kept here. A package must carry every part: the box keeps
  the last package's header at 0x470000 and its boot checks the flash
  against it (a one-part package sent the unit to recovery mode, once).
- `fw/obi/obinv.c`, the Firmware page's flash tool there: writes only parts
  that differ, only /obi, the system and the kernel; refuses a different
  boot loader or recovery image, so the recovery mode stays; locks itself in
  memory and restarts the box itself, since it may rewrite what it runs
  from. The page's upload limit is a build option (16 MiB for the OBi).

Audit before the release: two reviewers, one on `fw/obi`, one on the shared
daemon changes. Fixed from it: a priority inversion between the audio loop
and the threads sharing its lock; obinv rewriting the part it runs from
without locking itself in memory; the audio loop's stop conditions (separate
read and write errors, writes without reads); the header sector rewritten
whole instead of erased around 1 KiB; the old header read past its end;
duplicate parts accepted; the iptables fallback decided once from the
kernel version (it could hide errors on a kernel with nftables), our chains
first, all or nothing, an open port range from 0; one-sided bus callbacks.
The HT802 paths (TUN, nftables) are unchanged and their tests pass, the
namespace NAT test included.

Releases are now built by CI (`.github/workflows/release.yml`): publishing a
release builds and attaches `v90d-ht802fw.bin` (HT802/HT801),
`v90d-obi200.fw` and `SHA256SUMS`, after the host tests.

Open: the phone light not yet seen on the unit; the OBi202 (second port,
its LAN set-up) not done; no Windows MPPE client yet (issue #6).

## Issue #6 again: Windows dial-up callers could not encrypt (2026-11-23)

The first Windows tests of 0.8.0 (Windows 11 and XP, two winmodems and a
3Com CardBus modem, port 1 encrypting): "require encryption" ended in error
742 (XP, "the remote computer does not support the required data
encryption type"), 628 (Windows 11, after "Registering computer on
network") or 619 (the 3Com); "optional encryption" connected with no
encryption and dropped 10-20 s later, every time. The daemon log showed no
"PPP compression" line for any port 1 call: CCP never opened.

Cause: **a Windows client does stateful MPPE only on a modem (or PPPoE)
link**; stateless (the H bit) is for its VPN links. 0.8.0 offered and
accepted stateless only, the combination pppd uses. Windows asked for
stateful, we Nak'd that toward stateless; Windows Nak'd our stateless
request toward stateful, and we dropped MPPE from our request altogether.
So CCP could not open with MPPE: Windows gave up (742) or we ended the link
(628, PPP_END_ENCRYPT_REQUIRED), and with "optional" IPCP came up without
it until the encryption deadline (CCP's retry span) ended the call. The
same wall was hit by pppd + Windows XP/7 PPPoE clients (linux-kernel,
March 2011, "MPPE stateful mode problem and investigation"), which is also
where the Windows stateful details below come from.

Fixed (ppp/):

- Negotiation: 128-bit only still, either mode. We ask for stateless first
  (pppd keeps getting what it had), follow a Nak that names 128-bit in
  either mode, and once the peer has asked for stateful we ask for it too.
  A peer request with more bits (Windows "require": 0x60, "optional" with
  software compression: 0x61) is Nak'd to 0x40 in the mode it asked for.
  A Nak without the 128-bit bit still drops MPPE and the link ends.
- Stateful MPPE in `ppp_mppe.c` (RFC 3078 7.2/8.2), per direction: one RC4
  stream; key change on flag packets (count ending 0xFF); FLUSHED received
  means RC4 again from the current key (Windows sets it on its first packet
  and after a reset, not on flag packets); a lost packet makes us drop until
  a FLUSHED or flag packet and send one CCP Reset-Request (again only if
  undecryptable packets keep coming a Restart period later; never a CCP
  renegotiation). A Reset-Request to us skips our count to the next flag
  packet, a new key, instead of 8.2's RC4 restart on the current key, which
  would repeat a keystream. A Reset-Ack is sent until the peer Code-Rejects
  it (Windows does; the Linux kernel's receiver waits for one).
- Something decrypting to a protocol outside 0x21..0xFA counts as out of
  step.

Daemon: a call log line "PPP ended: ..." when PPP ended for a reason other
than a hang-up; for the encryption case it gives the caller's option 18
request and Nak in hex. The compression line says "MPPE stateful".

Proof: `ppp/test/test_ccp.c` has a Windows client modelled from RFC 3078
apart from ppp_mppe.c (stateful, no FLUSHED on flag packets, FLUSHED after
a reset, Code-Rejects Reset-Ack): on 0.8.0's code it reproduced the
failure (Nak toward stateless, then an empty request: 18 failures); now
"require", "maximum" and "optional" all open MPPE, 700 packets each way
cross two key changes, a lost packet (and 300 lost across a key change)
recover with one Reset-Request, and a Reset-Request to us resyncs the
client on a new key. `test_mppe.c`: the first stateful packet equals RFC
3079 3.5.3's own "test message" sample; 9000 packets round the counter
twice with losses, replays and resets; mixed modes. ns_pppd gained a
seventh call (pppd require-mppe-40 require-mppe-128 mppe-stateful: Nak'd
down to 128-bit, stateless). ppp `make test` and `test-arm`, srv `make
test`, `test-ns-pppd` (146 checks) all pass (one run of 3 had an unrelated
TCP flake in call 5, `noccp`; clean before and after).

Not verified: a real Windows client. The kernel's stateful MPPE differs
from Windows' (it rekeys on every FLUSHED), so pppd cannot stand in for
Windows here; the Windows behaviour is from RFC 3078 and that 2011 report.

## 0.10.0: the OBi200 on our own kernel (2026-11-23)

The OBi200 now runs v90d on Linux 6.6 with drivers of ours, not on the
maker's 2.6.30. No source was ever published for the board, so the drivers
were reconstructed from the maker's kernel (dumped from a running unit, with
its symbol table) and its modules (`fw/kernel/*-NOTES.md`, each fact marked
verified, inferred or guessed):

- board support: the FFxAV interrupt controller, timer, watchdog and restart;
  the CPU calls itself ARMv5TE (0x41159265), so the ARM926 entries of both the
  kernel and its decompressor match on part number (the decompressor's own
  table fell back to ARMv4 cache code, lost dirty lines, and the kernel ran
  into the maker's leftover code: crashes that moved with every build)
- Ethernet ("UniMAC AV", descriptor rings, a 2-byte Marvell header on every
  frame) and its switch (memory mapped, product 0xc00, not one mv88e6xxx
  knows): 20 Mbit/s down, 28 up, byte exact
- SPI controller (DesignWare SSI, chip-select held by the FIFO) with spi-nor
  for the W25Q128: reads match the flash dump; U-Boot, the factory block and
  the whole chip read only; a read-only frame is sent again after a receive
  overrun (one Firmware page write failed its check once, its data right)
- watchdog, LEDs (power on GPIO 5, phone forced through the switch PHYs), GPIO
- telephony with the maker's module's interface (/dev/fxsN ioctls,
  /dev/pcm), so obiline is unchanged: the SLIC on SPI chip select 2, and the
  audio DMA, which this kernel takes over as the maker's kernel left it
  running (its ring-off command is never acknowledged on this box): rings
  adopted in place, each descriptor rewritten before it is handed back

U-Boot checks checksums and has no fallback, so the kernel is not written to
the maker's kernel slot. The maker's kernel boots; `/obi/obi` starts ours
from RAM: `kjump` (no kexec on 2.6.30: a stub after the LED module's code,
run through /dev/led, caches and MMU off) unpacks `kernel.xz` with the
kernel's own XZ decoder while the maker's kernel still feeds the watchdog
(unpacking in the new kernel outlasts its 1.6 s), the shim copies the Image
into place. `oinit` mounts the box's own root file system and /obi; `kinit`
does the part of the maker's rc the box needs (no ssh, as on the HT80x). A
start count in RAM (`kjump --count`) keeps the box on the maker's kernel after
two starts that did not come up, until the power is cut. The Firmware page
writes through a "firmware" partition from 0x50000 under our kernel.

Proof, on one OBi200 (hardware 1.4, community firmware 8680EX-arrynrob13):
starts our kernel from flash by itself, also from a power cut (v90d ready
about 115 s after the power comes back); a U.S. Robotics 56K calls in at
V.92 54 666 / 33 333 bit/s, LAPM V.42bis, gets PPP and the internet through
nftables NAT (the same as on the maker's kernel); the settings from /scratch
(JFFS2 with 64 KiB erase blocks: 4 KiB SPI-NOR sectors misread it); several
updates through the Firmware page under our kernel and the way back to 0.9.1;
the lights, seen by the owner. obinv `make test` (27 checks), host tests and
the HT802 release build pass. /obi is 94 % full (2.23 of 2.36 MB).

## The HT801: a file of its own, and the first one on v90d (2026-11-24)

The HT802 file could not be installed on an HT801. Read from the stock HT801
1.0.65.3 file and its upload page (HARDWARE.md, "The HT801"), four things
stood in the way:

- the upload page checks the file's name in the browser: it must be
  `ht801fw.bin` (nothing is sent otherwise, and the box logs nothing);
- the upgrader only takes parts named for the model (`ht801core.bin` ...);
- every stock HT801 part carries model key 0xFD20 (the HT802's 0xFD21), and
  a part whose key differs from the partition it replaces is refused;
- the owner's V1.0A reports part number 9610003610A, not the dump's
  9611003610A, so the loader would not have started.

`make release` now also builds `v90d-ht801fw.bin` (HT801 names, key 0xFD20,
hardware bits FFFF as in the stock HT801 file), and the loader knows
9610003610A, under the untried rule. The system image is wrapped once per
model from the same kernel (`v90d ht802 VERSION`, `v90d ht801 VERSION`). The
loader's board list gives each part number its model; our own Firmware page
writes only an image named for the box's model ("this file is a v90d image
for the ht802; this box is a ht801: nothing is written"), so an HT801's
`base` never gets the HT802's model key, which the stock upgrader would
refuse to write over. `v90nv where` prints the model, and the update check
fetches that model's file.

Proof, on one HT801 V1.0A (part number 9610003610A; 25Q128 flash, DVF9918;
stock 1.0.65.3), its owner's report: `v90d-ht801fw.bin` (0.10.0), renamed
`ht801fw.bin`, installed from the stock page; the box came back on v90d,
and a call connected. Its Line status, as the owner pasted it:

```
State             connected
Modulation        V.90
Downstream        52000 bit/s
Upstream          28800 bit/s
Round-trip delay  33 ms
Receive level     -15.0 dBm0
Receive SNR       37.3 dB
V.42              LAPM
Compression       on
```

Proof, tests: `make check` 151 programs, 3,299,858 checks, 0 failures;
`make check-arm` (QEMU) and `make -C fw test-release` 156 programs, 1,011,136
checks, 0 failures (v90nv 26 and the boot model 33 tests on host and ARM,
mkgsfw 20, release 11, loader 144 on host and ARM); the same ARM binaries run
natively on a Raspberry Pi 4: 150 programs, 1,010,778 checks, 0 failures.
The release build made and checked both files. After the per-model images,
`make -C fw test-release` again (v90nv 26, boot model 33 with the model check,
loader 144, each on host and ARM) and the release build: both files, each
image named for its model.

## 0.11.0: IPv6, the terminal menu, the time service, the HT801 (2026-11-24)

Everything merged since 0.10.0 (#8, #10 to #20):

- **IPv6 (#7)**, in three steps:
  - IPv6 in the HT802 and OBi200 kernels, with SLAAC on the LAN port (#15);
  - the `[network] ipv6` setting, the router's name servers and NAT64 prefix from its advertisements, and outgoing connections over IPv6 (#16);
  - the callers' IPv4 over IPv6 through a NAT64 (an RFC 7915 translator, 464XLAT's CLAT side), and stand-in IPv4 addresses for names with IPv6 alone (#19).
- **The callers' name server** is a DNS relay on the box, answering IPv6 lookups with none, so callers with an IPv6 stack stop trying addresses the dial-up link cannot reach (#13, #14).
- **Netstalgia's terminal menu**, its signed services list, PROTOWEB archive calls and the reworked web interface (#8), with fixes from the first real calls (#10, #11).
- **A time service** in NIST's ACTS format on 303 494 4774 (#12).
- **The HT801**: a file of its own, keyed for its stock upgrade page (#17, by marrold, confirmed on a V1.0A with a V.90 call). A V1.1A boots v90d from flash (#18).
- **libv90modem 0.6.1** (#20): V.90 9.2.1.2.1 recovery (a caller restarting at INFO0a now connects) and the shared upstream receiver.

Proof on hardware for this release: the OBi200 on 0.6.1 answered a USR Sportster (V.34 31200) with PPP, ping and HTTP. With no IPv4 route out and only IPv6 to a NAT64, the same call reached 1.0.0.1 through the translator. The HT801 V1.1A heard a router advertisement and brought its translator up.

## WebTV, built in (2026-11-25)

A WebTV / MSN TV box reached WebTV Redialed through Netstalgia's dial-in
first (2026-11-25); the box now does the same on its own (design and plan
in `docs/superpowers/`). First built as three numbers answered on internet
and archive ports under `[services] webtv`; at the owner's word, WebTV is
now a service of its own per port instead, and that switch is gone.

- `[line] service = webtv` (and `[line2]`), on the Line page.
- On such a port every call is the box's, whatever number: PAP with any
  login, no encryption, no terminal menu; calls show as "WebTV (PPP)". The
  numbers 800-932-8800, 800-613-8199 (V.34 at most) and 484-820-1337 end
  the dialling at once (the telephony layer's `more` question now also ends
  dialling on a port that takes any number), any other after 3 s.
- For the call: 10.0.0.1 to WebTV Redialed and the box's fixed name server
  (157.57.221.51) to the port's, in both NAT back ends.
- WebTV Redialed's address looked up from community.webtv.zone every
  10 minutes while a port is a WebTV port; public IPv4 only; a failed
  lookup keeps the last one.
- On the way: the port's own number dialed with the 1 in front is now the
  port's own for the BBS, service, PROTOWEB and ACTS numbers as well.
- Tests: cfg (the service), tel (a known number ends dialling on an
  any-number port), test_srv (any number, the known ones, login,
  modulations, other services), test_webtv (the address picked), ns_nat
  (the rewrites, both ports), ns_call (whole calls through the daemon).
- Not yet tried with a real box on a v90d port.

## 0.12.0: WebTV and the HT802 V2.1A (2026-11-25)

Everything merged since 0.11.0 (#23 to #29):

- **WebTV** (#28, #29): a port whose service is WebTV takes a WebTV / MSN TV box to WebTV Redialed, whatever number it dials. The port's own number dialed with the 1 in front is now the port's own for every special number.
- **The HT802 V2.1A** (#27, issue #26), confirmed by its owner on the v0.11.1-rc1 pre-release.
- **1-800-BBS-DIAL shows the BBS directory** and keeps the typing (#23).
- **The libv90modem fetch** is its own script, `fw/setup-libv90m.sh` (#24 by marrold, #25).

Proof for this release: the host suites and the namespace suite (a whole WebTV call through the daemon); WebTV through Netstalgia's dial-in with a real box on 2026-11-25. Not yet tried: a real WebTV box on a v90d port.

## WebTV's other addresses (2026-11-25)

On v0.12.0, 10.0.1.129 went nowhere: a WebTV port sent only 10.0.0.1 to
WebTV Redialed, and the Dreamcast port's community list has no entry for
it. Now a WebTV call sends 10.0.0.1, 10.0.0.2 (the boxes' fallback),
10.0.128.1 (the daily builds') and 10.0.1.129 there, and a Dreamcast port
sends 10.0.1.129 (its names untouched); the lookup of WebTV Redialed runs
while a port is either. Tests: ns_nat (every address, the Dreamcast port's
one, both back ends' rules from one plan), ns_call (the rules of a WebTV
call and of a Dreamcast port, read back from the kernel).

Found on the way: the lookup's child process runs no program, so it kept
every descriptor of the daemon open, the TUN devices included; while it
ran, a port's ppp0 could not be set up again ("Device or resource busy",
callers getting PPP with no network until the next try). It now closes
them all but its pipe. On 0.12.0 this could hit a WebTV port; with the
lookup also running for Dreamcast ports, ns_call's console lease test
caught it.

## 0.12.1: WebTV's other addresses (2026-11-25)

- 10.0.0.2, 10.0.128.1 and 10.0.1.129 to WebTV Redialed for WebTV calls,
  10.0.1.129 for Dreamcast ports (above).
- The lookup's child no longer holds the daemon's TUN devices.

## 0.12.2: the WebTV server, and an audit of the WebTV work (2026-11-25)

- **`[services] webtv_server`** (Services page): WebTV Redialed (the
  default, followed by name; 31.97.100.150 until a lookup has worked),
  HackTV Public Minisrv (71.244.121.234), ZefieTV (216.126.232.171), or an
  address of the owner's (one on the home network is fine; 0/8, loopback,
  multicast and the boxes' own old addresses are refused). A WebTV port's
  box and a Dreamcast port's 10.0.1.129 go to it: 10.0.1.129 is WebTV for
  Dreamcast's service address (the WebTV Wiki), a WebTV service, not the
  console's games. The lookup runs only while that server is WebTV
  Redialed, and again a minute after a failure instead of ten.
- An audit of everything WebTV since 0.11.0 (a fresh reviewer, then one
  pass of fixes):
  - fewer NAT rules: the four addresses match any protocol, one nftables
    rule each instead of two, one iptables command for all four instead of
    eight; a Dreamcast port one rule;
  - on an internet port with no number of its own, a service's or
    Netstalgia's number no longer ends the dialling at once (only a WebTV
    port's own numbers do), so the list is not asked on every digit;
  - a change of the WebTV server reaches a Dreamcast port's rule at once;
  - the archive gateway is not touched when only a WebTV call began or
    ended; the lookup runs in the 1 second step; a lookup child that
    vanished is taken as failed, not killed every second;
  - stale wording from the first designs gone (code, pages, docs); the
    Services page and the log share the servers' names; a bad address for
    Other is reported as the server's (the key it is saved under); the
    status page's service a table.
  - The Services page's "worst case" (28585 of 28672 bytes) is no near
    miss: that test fills Netstalgia's list until the page is full, so it
    always ends close to the buffer's size.
  - Left as they were: the NAT layer's "no server" guards (the daemon
    always passes one now); DNS over TCP to the box's relay, which answers
    UDP only (the boxes ask over UDP).
- Tests: cfg (the setting, its refusals, the page), srv (any-number
  ports), ns_nat (both kinds of rule), ns_call (a call's log names the
  server, a Dreamcast port's rule follows the setting).

## 0.13.0: libv90modem 0.7.0, and the V.34 cap on WebTV ports (2026-11-26)

- **libv90modem 0.7.0** (from 0.6.1; a new ABI, v90d rebuilt against its
  header). What it brings to v90d's calls:
  - x2: a strict V.34 caller on a port with x2 on no longer fails at the
    start (the inverted JM only for the first 2 s);
  - V.34 Phase 3/4: the time limits allow for a late echo;
  - the modem's "still sending" signal holds until the closing flag is out,
    so a hang-up never cuts the last frame;
  - a USR Courier's S after Jd on tilted lines (libv90modem PR #5);
  - HST answering and MNP class 3: in the library, off; v90d does not
    offer them.
- **WebTV ports:** every call has the library's V.34 transmit cap
  (V90M_OPT_V34_TX_CAP): on V.34 the box is sent no faster than its own
  line probe projected, as a box's modem sometimes asks for more and then
  fails error control. After an answered V.34 call the log has the
  caller's projection, what it asked, and whether the cap held it.
- Tests: srv (a WebTV call has the cap, an internet port's does not); the
  host, ARM and namespace suites and the three images built against 0.7.0.

## 0.13.1: libv90modem 0.7.1, and a log line for every stop of the data in a call (2026-11-26)

- A WebTV box sometimes drops the call and shows its call-waiting message:
  its TellyScript turns on the modem's call-waiting detection, and the box
  takes a short stop of the data for a call-waiting interruption. On
  Netstalgia a box's modem renegotiated every ~22 s at the rate it had and
  hung up 8 to 10 s after one of those stops. v90d logged none of them.
- **libv90modem 0.7.1** (from 0.7.0; additive, the same ABI): it tells who
  began each retrain and rate renegotiation (v90m_get_restarts).
- Now each stop, as data moves again, is a log line: how long the data was
  stopped, how long after PPP came up, a retrain or a rate renegotiation,
  who began it (the caller, or us and why), and the rates before and
  after, for example `data stopped 0.36 s, 1.0 s after PPP came up: a rate
  renegotiation begun by the caller, 50666/28800 -> 50666/28800 bit/s`.
  dce keeps the last stop for this (dce_last_stop).
- The member-side fix for such a box: call-waiting detection off in its
  dialing settings.
- Tests: dce (a renegotiation and a retrain by the caller, and one of
  ours, are recorded with who began them; none before), ns_call (a V.90
  caller renegotiating at its rate on a WebTV port: the line is in the
  daemon's log).

## 0.13.2: libv90modem 0.7.2, and the K56flex choice in the log (2026-11-26)

- **libv90modem 0.7.2** (from 0.7.1; additive, the same ABI): K56flex's
  V.8bis CL offers versions 1 and 2 (it offered 2 alone). A version-2
  client gets the call it got before; a version-1 client (an early
  Rockwell modem, the WebTV Plus) is declined and goes on as V.90 or
  V.34, as before. v90m_get_k56 tells what was offered and chosen.
- After each call where K56flex was offered, the log says what the caller
  chose and whether we took it, for example `K56flex: we offered versions
  1 and 2; the caller chose version 1, declined: the call went on without
  K56flex` (srv_k56_text).
- README: a WebTV Plus renegotiating about every 22 s is its modem's
  automatic fall-forward (factory default); `AT%E1` in its TellyScript
  stops it (found by the libv90modem work).
- Tests: srv (the K56flex line for each case: nothing offered, offered and
  not chosen, version 2 agreed, version 1 declined, version-2-only and
  version-1-only offers, an undefined version, a short buffer, NULL).

## 0.14.0: PPP fixes from an audit and the ISP's Windows error 742 report (2026-11-26)

Two Windows dial-up members of the Netstalgia ISP (nsdiald, on v90d's PPP)
saw the PC end PPP right after its MS-CHAPv2 login, before IPCP, with
Windows error 742 ("does not support the data encryption requirements").
An audit of ppp/ (negotiation, authentication, framing, compression and
encryption; fuzzed under ASan and UBSan, crypto checked against reference
implementations) found the cause and three other bugs. No memory-safety
bug and no way past authentication were found.

- **40-bit and 56-bit MPPE.** CCP agreed 128-bit keys only, so a PC with
  shorter ones (Windows 9x/ME without the 128-bit upgrade) could never
  agree and ended with 742. Now one key length both ways: 128-bit, or the
  strongest of 56 and 40 a peer shows it has (by request, Nak or, as pppd
  does, Reject); the short keys as RFC 3079 3.3/3.4 and the kernel derive
  them. CCP is negotiated again, before anything is keyed, if the two
  directions were Acked at different lengths. A Nak never takes the length
  below the strongest of the peer's own request (a review found that one
  injected Nak could talk a 128-bit peer down to 40), and that floor only
  ever rises (a later request that claims less, one injected, say, changes
  nothing). `mppe_keybits` counts
  it; the daemon's compression line says "40-bit keys" and so on. A
  `require-mppe-40` call runs against the kernel's ppp_mppe (ns_pppd).
- **The caller's encryption request is always kept.** With encryption off
  its option-18 request was never recorded, so a log said "asked nothing".
  It is kept now, and the daemon logs "the caller asked for encryption
  (option 18 ...) and none was agreed" for such a call (srv_mppe_note),
  however the call ended.
- **Stateless MPPE after a long loss.** More than 512 packets lost in a row
  (the catch-up credit) and no packet ever decrypted again: the call stayed
  up carrying nothing. The receiver now steps as far as the credit goes on
  each packet and catches up over the next ones. Past the 2047-count window
  (either mode) the link now ends with `PPP_END_MPPE_LOST` after 32 such
  packets, instead of staying up dead.
- **Configure-Nak/Reject within the peer's MRU** (whole options, as many as
  fit).
- **A PPP trace** (`ppp_config_t.trace`, asked for by the ISP): every
  control packet each way with the time, the protocol and up to 64 octets,
  PAP and CHAP header only, also inside a Protocol-Reject (no secrets). The daemon logs it at debug level
  (srv_ppp_trace_text), for example `PPP +1.234 s in  CCP Configure-Request
  id 5, 10 octets: 12 06 00 00 00 21`.
- **Encryption on request, with no setting.** The PPP encryption switch is
  gone from the Modem page and the config file (an older file's
  `encryption` and `encryption2` lines are read and ignored). A caller that
  asks for MPPE gets it; one that does not is served in the clear, with the
  compression it asks for. Once a caller has asked, nothing goes in the
  clear either way. Windows' "optional encryption" asks for MPPC and MPPE
  in one request (0x61); we do not do both at once, so it keeps MPPC in
  the clear where the port compresses (text is far quicker that way), and
  only "require data encryption" (MPPE alone) is taken as asking. With the keys in hand CCP waits up to a Restart period
  for the caller's request before sending ours, so ours can be MPPE from
  the start (pppd with require-mppe gives up on any other first request). Both logins (CHAP and PAP) offer MS-CHAPv2 first, which
  MPPE needs, then CHAP-MD5, then (a PAP port) PAP: a caller moves on with
  a Nak, or now also a Configure-Reject (old stacks that Reject a method
  they do not know), and only a Reject of the last method ends the link.
  An older file with `encryption = on` (required) loads, and the daemon
  says at start that encryption is now given on request, so a caller that
  does not ask is served in the clear (a security review asked that the
  change not be silent).
  A port with a login carries IP 4 octets smaller (the MPPE overhead);
  status.json's `ppp_encryption` means "given on request" now. In the
  library `cfg.encrypt` 0 is on request, 1 still means required.
- **A race on ppp0.** The NAT child (re-applying the rules as a call begins
  or ends) and the STUN child ran on as themselves holding the daemon's TUN
  devices; when a port's ppp0 was set up again meanwhile (a console's lease
  running out), it was "busy" and callers got PPP with no network.
  ns_call's console lease test caught it now and then. Both children now
  close every descriptor, and the set-up waits for a NAT child first. That
  was not all of it: any child between its fork and its exec (the lease
  script, started as the lease runs out) holds the descriptor too, and
  close-on-exec only lets go at the exec. So the device is no longer made
  again when only its addresses or MTU change: it gets them in place
  (tun_readdress), and is made anew only if that fails. ns_call checks
  that the lease test never logs "cannot set up".
- Tests: mppe (long runs of loss caught up on, 513 to 2046; the late
  count; 40- and 56-bit packets against a reference written apart), ccp
  (the link ends past the window; 40- and 56-bit callers each way, a
  Windows-style Nak, a Reject after the peer's request, 128 when offered,
  no Nak below the peer's own request in either order, two lengths never
  keyed; the request logged with encryption off; on request: Windows-style
  (asked, Deflate Rejected, MPPE; no clear meanwhile), pppd-style (no
  MPPE asked: Deflate and the clear), a caller that Acks our Deflate and
  asks MPPE, no keys), ppp (MS-CHAPv2, CHAP-MD5 and PAP in turn on Rejects;
  a Reject cut to MRU
  68; the trace, in order, no password, none in a Protocol-Reject), srv
  (the encryption note, the trace line; a PAP caller on a port that offers
  MS-CHAPv2 first), cfg (old encryption keys read and not written; no
  field on the page), ns_pppd (require-mppe-128, -40 and both, with no
  setting; the plain calls unchanged), ns_call
  (the trace in a call's log).
- **libv90modem 0.7.4** (from 0.7.2; additive, the same ABI): V.92 Quick
  Connect on the answering side, on whenever V.92 is offered (a caller that
  asks for it gets the short start-up; one that does not complete it falls
  back to V.8 within 2 s), and V.8bis's ACK(1) fixed. 0.7.3's HST fixes come
  with it (HST stays off in v90d). After each call where Quick Connect was
  asked for, the log says what came of it (srv_qc_text, from v90m_get_qc):
  the caller's Ucode and LAPM bit, taken or not, the ANSpcm level, its TONEq
  and the short Phase 2, or the fall back to V.8. Tests: srv (each case of
  the line). Not yet tried with a real V.92 modem.
- **Van Jacobson TCP/IP header compression** (RFC 1144), asked for by a
  Netstalgia member (BinModem's author): Windows 95/98/ME ("Use IP header
  compression", on by default), classic Mac OS and BinModem offer it, and
  at modem speeds it makes interactive TCP much quicker (an echoed
  keystroke is a few octets instead of 41). IPCP option 2 (0x002d) each way
  on its own, 16 slots offered, the peer's Max-Slot-Id and Comp-Slot-Id
  honoured, other compression protocols Rejected; on by default, off with
  `ppp_config_t.no_vj` (and on console and WebTV ports, as CCP is). VJ
  works before CCP and MPPE; after a lost frame compressed packets are
  tossed until the slot is named or re-stated (RFC 1144 4.2). New module
  `ppp/ppp_vj.c` (build lists that name the PPP sources need it). Counters
  `vj_tx_comp`, `vj_tx_uncomp`, `vj_rx_comp`, `vj_rx_uncomp`, `vj_errors`,
  `vj_tx_slots`, `vj_rx_slots`; the daemon logs a line per call
  (srv_vj_text). Tests: vj (mixed streams rebuilt to the octet, plain IP
  cases, more connections than slots, slot number left out or not, tossing
  and the resend, malformed input, 200000 random packets; clean under ASan
  and UBSan), ppp (negotiation both forms, IPHC Rejected, off; data both
  ways, the peer's slot count and Comp-Slot-Id, a bad FCS and recovery,
  Protocol-Reject when not agreed), ccp (VJ under MPPE, no clear VJ), srv
  (the log line; off on console and WebTV ports), ns_pppd (every call with
  VJ against pppd and the kernel's slhc, and a novj call).
- dce's noisy-line test (V.22bis without error control, noise bursts) judged
  too early: a frame lost at the very end of the bursts showed as a gap only
  with the next packet, so the final four packets fell into the reset that
  followed (RFC 1979: dropped meanwhile). Offering VJ made IPCP six octets
  longer, the bursts landed later, and the quick run (under qemu) hit it.
  The test now sends a packet each way and lets a reset complete before it
  judges, and checks recovery in each direction that lost frames (which
  ways a burst hits is chance). No change to the PPP code: recovery worked.
