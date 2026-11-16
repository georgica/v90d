# v32: V.32bis / V.32 answering modem

A duplex, echo-cancelling 2400 symbol/s data modem for the answering side of
v90d, written from scratch from ITU-T Recommendation V.32bis (02/91,
`../refs/itu/T-REC-V.32bis-199102-I.txt` and `.pdf`). No other modem
implementation was read or copied. The signal path is C99, integer only, has
no allocation, and runs one 8000 Hz 16-bit sample in, one out per call, like
`../modem/v22bis`.

Status in one paragraph: the whole answer-side start-up, data at all five
rates, retrains and the hand-over from V.8 work in simulation against a
calling test peer that was written separately (double precision, straight
from the Recommendation's tables). Nothing here has talked to a real modem,
and the test peer was written by the same hand as the modem, so a shared
misreading of the Recommendation would not show (see "Not done / not
verified").

## What is implemented

| Recommendation | Where | Notes |
|---|---|---|
| 2.1, 2.2 carrier 1800 Hz, 2400 symbols/s, spectrum | `v32_tx.c`, `tools/gen_tabs.py` | root raised cosine, 25 % excess bandwidth, 12 symbols, ten phases (a symbol is 10/3 samples); 600 and 3000 Hz are 3.2 to 4.5 dB down (2.2 asks 4.5 +- 2.5) |
| 2.3 signal element coding, 14400 / 12000 / 9600 / 7200 trellis coded, 4800 uncoded | `v32_code.c`, `v32_tabs.c` | differential encoder (Table 1), convolutional encoder (Figure 1), constellations of Figures 2-1 to 2-5, Table 2 for 4800 |
| 4 scramblers GPC (call) and GPA (answer) | `v32_code.c` | |
| 5.2 S, S-bar, TRN; 5.3 rate signals R1 R2 R3, E | `v32_tx.c`, `v32_rx.c` | |
| 6.2 answer mode start-up, Figure 3 | `v32.c` | AC / CA / AC with the 64 T turn-round, round trip MT, TRN extension, R1, wait MT, receiver training, R2, second conditioning signal, R3, E, B1, data |
| echo cancellation (near and far, bulk delay from MT) | `v32_ec.c` | 96 + 96 taps at 8 kHz, joint NLMS, far window up to 2 s back |
| receiver | `v32_rx.c` | fractional-delay band-pass filter bank, band-edge timing recovery, T/2 32-tap adaptive equaliser, carrier loop, 8-state Viterbi decoder (`v32_viterbi.c`) |
| 7.2 retrain, either end | `v32.c` | `v32_retrain()`, the caller's AA in data mode, and by itself when reception stays poor |
| Table 5 cleardown | `v32.c` | no common rate: R3 without a rate for 64 T, then silence |
| A.2.2 answering automode: AA during the answer tone | `v32_rx.c` (`v32_aadet_*`) | detector only; the sequencing belongs to `modem/answer.c` (see below) |

The calling side exists only as the test peer `../sim/v32call.c` (double
precision, not for the target).

## Files

    v32.h            public API (everything below)
    v32_priv.h       tables and helpers shared by the sources
    v32_code.c       scramblers, differential + trellis encoder, rate words
    v32_viterbi.c    8-state decoder, differential decoder
    v32_tx.c         modulator, all transmit signals
    v32_ec.c         echo canceller
    v32_rx.c         receiver, tone / S detectors, AA detector
    v32.c            answer-mode state machine
    v32_tabs.c       generated: constellations, pulse shapes, receive filter bank
    tools/gen_tabs.py  generator (constellations are read out of the PDF's figures)
    test/            test_code test_viterbi test_tx test_ec test_rx test_startup
                     test_call test_v8 test_bench, calltest.h
    ../sim/v32spec.h   Tables 1, 2, 4 and the constellation rows, typed in by hand
    ../sim/v32call.*   calling modem test peer (double)
    ../sim/v32line.*   2-wire line between the two: hybrids, echoes, delay,
                       loss, noise, carrier offset, clock offset, G.711 (via sim/channel)

`make test` (host), `make test-arm` (cross-built, under qemu-arm-static),
`make check-fixed` (no floating point or allocator symbol in the modem's
objects), `make cost-arm` (instructions per sample), `make tables`.

Size on the ARM target: 14.7 KB of code, 7.9 KB of tables in this directory
(plus the sine table of `../modem/sintab.h`, 2 KB); `sizeof(v32_t)` is 36264
bytes, of which 32 KB are the far echo canceller's 2 s of transmit history.

## API

    #include "v32.h"

    v32_cfg cfg;
    cfg.level  = 3000;            /* transmit RMS, 16-bit sample units */
    cfg.rates  = V32_RALL;        /* V32_R4800 | ... | V32_R14400 */
    cfg.getbit = my_getbit;       /* int  (*)(void *user): next data bit to send */
    cfg.putbit = my_putbit;       /* void (*)(void *user, int bit): received bit */
    cfg.user   = my_ctx;
    cfg.echo_delay = 0;           /* samples of I/O buffering, see below */
    cfg.no_auto_retrain = 0;
    v32_init(&m, &cfg);           /* starts sending AC at once */

    for every sample:  tx = v32_tick(&m, rx);

* `m.connected` becomes 1 when both directions are in data mode;
  `m.bitrate` is then 4800 ... 14400 (the same both ways). `getbit` is
  called for every transmitted bit from the end of our own B1
  (`m.tx_ready`), `putbit` for every received bit from the end of the
  caller's B1 (`m.rx_ready`, circuit 104 unclamped); `connected` is both,
  and the two are a few tens of symbols apart, so a few bits may be asked
  for just before `connected` rises. They are sent as data. During the
  start-up and during a retrain neither function is called.
* `m.retraining` is 1 from the start of a retrain until data mode is back;
  `connected` stays 1 through it and `bitrate` may come back different.
  `m.retrains` counts them.
* `m.state == V32S_FAILED` ends the call, with `m.error`: `V32_ERR_NO_TONE`
  (no AA in 20 s, or no reversal), `V32_ERR_TRAIN` (10 s without the
  caller's S, R2 or E), `V32_ERR_NO_RATE` (nothing in common or the caller
  asked for a cleardown), `V32_ERR_CARRIER` (in data mode: no line signal
  for 400 ms, or reception still too poor after three retrains of our own).
  After that `v32_tick` returns silence.
* `v32_retrain(&m)` starts a retrain from data mode (0, or -1 when not in
  data mode).
* `cfg.echo_delay`: samples between handing a sample to the driver and the
  earliest moment its echo can come back in the received samples (transmit
  plus receive buffering). The near canceller's 12 ms window starts there,
  and the 64 T turn-round of 6.2 is advanced by it so that it holds at the
  line. Up to about 150 samples the turn-round can be met; beyond that it is
  late by the excess (it still connects with the test peer at 160).
* Diagnostics: `m.mt` (round trip as measured, samples, includes the
  caller's 64 T), `m.erle_db10` (echo attenuation at the end of TRN, 0.1 dB),
  `m.rx.mse` (decision error, 4096 = one signal-space unit squared),
  `v32rx_level(&m.rx)` (received RMS), `m.seg_len[]` (symbols sent in each
  state), `m.carrier`.

The building blocks (`v32_scramble`, `v32_encode`, `v32_vit_*`, `v32tx_*`,
`v32ec_*`, `v32rx_*`) are public for the tests; an integration needs only
the calls above and, for callers without V.8, `v32_aadet_init/tick`.

## How it works, briefly

* Transmitter: symbols are rotated by the carrier (1800 Hz is 3/4 cycle per
  symbol, so the rotation is a power of -j) and put through one of ten
  polyphase pulse tables. A second table makes the echo canceller's
  reference from the same symbols (below).
* Echo canceller: two FIR models at the sample rate, the near one on the
  last 12 ms, the far one on a 12 ms window placed `MT - 64 T` back, adapted
  together by NLMS on the common residual during the first TRN (the caller
  is silent then), and frozen afterwards. A canceller driven by the transmit
  samples themselves converged to only about 27 dB in 1280 symbols: the
  transmit spectrum is nearly empty at the band edges, where the echo path
  still has to be modelled, and those modes adapt slowly. So the model is
  driven by a "flat" version of the same symbol stream (wider pulse, no
  roll-off inside the band) and the line signal is delayed six symbols
  behind that reference so that the echo is causal with respect to it. That
  gives about 50 dB in 1280 symbols on a linear path (`test_ec`), 44 to 46 dB
  in a whole call on a linear line and 32 to 33 dB behind mu-law, where the
  codec's own quantising noise is the floor.
* TRN is extended beyond 1280 symbols by the round trip (the far echo of TRN
  is back only then), and then in blocks of 256 until the echo is 40 dB down
  or a block gains less than 1 dB; never beyond 8192.
* Receiver: a bank of 48 fractionally delayed complex band-pass filters
  produces T/2 samples at instants chosen by a band-edge (600 / 3000 Hz)
  timing loop with a second-order filter, so a sample clock offset is
  tracked without a resampler. A 32-tap T/2 equaliser trains on S-bar and the
  known TRN sequence (1024 symbols against the reference, then on 4-point
  decisions), a second-order carrier loop follows frequency offset, and the
  8-state Viterbi decoder (24 symbols deep) decodes the trellis rates.
* Rate choice: R3 takes the highest rate, within R2 and our own set, whose
  threshold the equaliser's error at the end of our TRN2 is under (that
  error includes what is left of our own echo). In test_call's noise runs
  this picks 14400 down to 24 dB of SNR, 9600 at 20, 7200 at 16 and 4800 at
  12, each with no or next to no errors.

## Tests

| Test | What it checks |
|---|---|
| `test_code` | scrambler sequences against vectors worked by hand from the polynomials; Table 1 (as mod-4 addition) and Table 2 against the hand-typed tables; every constellation point against the hand-typed rows of Figures 2-1 to 2-5; point counts, symmetry, mean power; the trellis encoder's state table; rate words of Table 5 |
| `test_viterbi` | decoder against the encoder at each trellis rate: no noise, all four 90 degree rotations, and Gaussian noise at a level where plain decisions make about 3.5 % symbol errors |
| `test_tx` | line level, spectrum at 600 / 3000 Hz and out of band, and every transmit signal demodulated by an ideal receiver and compared with the Recommendation's states: AC, CA, AA, CC, S, S-bar, TRN (both scramblers, 5.2.3 and Table 4), rate sequences, data at 14400 |
| `test_ec` | echo canceller alone on synthetic hybrids: convergence in 1280 symbols, far echo with the bulk delay right and wrong, long delay, behaviour in double talk |
| `test_rx` | receiver alone against a double-precision signal built from the tables: S, S-bar, TRN, R, E, data at every rate, levels from 160 to 8000 RMS, a long TRN, noise |
| `test_startup` | whole calls against the test peer with every segment's length asserted: AC >= 128 T and even, the 64 +- 2 T turn-round at the line (both ends), MT and NT against the line's true round trip, 16 T of silence, S 256 T, S-bar 16 T, TRN within 1280..8192, silence >= MT after the caller's S, second S / S-bar / TRN, R3 in whole sequences, E 8 T, B1 128 T, the contents of R1 R2 R3 E; one-way delays of 8, 53 and 303 ms; restricted rate sets at either end |
| `test_call` | end-to-end bit error counts, both directions, each rate: clean line, G.711 mu-law (through `sim/channel`), hybrid echo, -30 dBm0, +-100 ppm clock offset, +2 / -7 Hz carrier offset, white noise from 32 down to 9 dB SNR; retrain from each end; retrain by itself when the line turns noisy; driver buffering; carrier loss; no common rate |
| `test_v8` | V.8 (`modem/v8ans` against `sim/v8call`) handing over to V.32bis; the AA detector; a caller without V.8 |
| `test_bench` | not a test: records a call for `make cost-arm` and proves the ARM build produces the host's transmit samples bit for bit |

In `test_call` and `test_v8`, "c->a" is caller to answerer (this modem's
receiver), "a->c" answerer to caller (this modem's transmitter into the
peer's receiver). "echo" is the echo attenuation the answerer measured at
the end of its TRN (it includes line noise, so it falls with SNR), "mse" the
answerer's decision error at the end of the run (4096 = one unit of the
signal space squared). SNR is signal power over white noise power in the
whole 4 kHz band at each receiver; the SNR in the signal's own band is
about 2 dB better. Rows marked "reported only" are not asserted.

### Host: `make test` and `make check-fixed`

```
$ make test
== running host tests ==
   4800 bit/s:   4 points, mean power 40.00
   7200 bit/s:  16 points, mean power 40.00
   9600 bit/s:  32 points, mean power 40.00
  12000 bit/s:  64 points, mean power 42.00
  14400 bit/s: 128 points, mean power 41.00
test_code: 5750 checks, 0 failures
   7200 bit/s, sigma 0.870: plain decisions 626 symbol errors in 20000, decoder 0 bit errors in 60000 bits
   9600 bit/s, sigma 0.615: plain decisions 701 symbol errors in 20000, decoder 0 bit errors in 80000 bits
  12000 bit/s, sigma 0.435: plain decisions 735 symbol errors in 20000, decoder 0 bit errors in 100000 bits
  14400 bit/s, sigma 0.307: plain decisions 756 symbol errors in 20000, decoder 0 bit errors in 120000 bits
test_viterbi: 484 checks, 0 failures
   4800 bit/s scrambled ones: line RMS 2997 for level 3000
   7200 bit/s scrambled ones: line RMS 3005 for level 3000
   9600 bit/s scrambled ones: line RMS 2998 for level 3000
  12000 bit/s scrambled ones: line RMS 3073 for level 3000
  14400 bit/s scrambled ones: line RMS 3023 for level 3000
   4800 bit/s: 600 Hz -3.7 dB, 3000 Hz -3.7 dB, 200 Hz -42.8 dB, 3500 Hz -46.9 dB re maximum
   7200 bit/s: 600 Hz -4.4 dB, 3000 Hz -4.4 dB, 200 Hz -43.8 dB, 3500 Hz -47.0 dB re maximum
   9600 bit/s: 600 Hz -3.2 dB, 3000 Hz -3.2 dB, 200 Hz -43.9 dB, 3500 Hz -46.9 dB re maximum
  12000 bit/s: 600 Hz -3.8 dB, 3000 Hz -3.7 dB, 200 Hz -42.9 dB, 3500 Hz -46.7 dB re maximum
  14400 bit/s: 600 Hz -4.5 dB, 3000 Hz -4.5 dB, 200 Hz -42.2 dB, 3500 Hz -46.5 dB re maximum
  14400 bit/s: worst deviation of a demodulated point 0.130 units (points are 1.41 apart)
test_tx: 2237 checks, 0 failures
  near echo, 1280 symbols of TRN: 50.7 dB
  near echo, 3000 symbols of TRN: 52.6 dB
  near + far echo at 40 ms, bulk delay set: 54.5 dB
  near + far echo at 40 ms, bulk delay wrong: 0.6 dB
  near + far echo at 600 ms: 55.3 dB
  then 16 s of double talk, frozen: 54.5 dB before, 55.9 dB after
  then 16 s of double talk at step 2^-15: 54.5 dB before, 52.5 dB after
  then 16 s of double talk at step 2^-11: 32.5 dB after (too fast)
  double talk at the training step 2^-2: 2.9 dB after
test_ec: 9 checks, 0 failures
   4800 bit/s level  2000 snr  0: S at 472, S-bar at 1398, R at 5871 (word 0ff9 x17), E fd91; mse after TRN 0, in data 0; timing -52; 11948 bits, 0 errors
   7200 bit/s level  2000 snr  0: S at 472, S-bar at 1398, R at 5871 (word 0ff9 x17), E f9d1; mse after TRN 0, in data 0; timing 19; 17853 bits, 0 errors
   9600 bit/s level  2000 snr  0: S at 472, S-bar at 1398, R at 5871 (word 0ff9 x17), E fb91; mse after TRN 0, in data 0; timing 117; 23804 bits, 0 errors
  12000 bit/s level  2000 snr  0: S at 472, S-bar at 1398, R at 5871 (word 0ff9 x17), E f9b1; mse after TRN 0, in data 0; timing 10; 29755 bits, 0 errors
  14400 bit/s level  2000 snr  0: S at 472, S-bar at 1398, R at 5871 (word 0ff9 x17), E f999; mse after TRN 0, in data 0; timing 377; 35706 bits, 0 errors
  14400 bit/s level   160 snr  0: S at 484, S-bar at 1398, R at 5871 (word 0ff9 x17), E f999; mse after TRN 1, in data 0; timing 378; 35706 bits, 0 errors
  14400 bit/s level  8000 snr  0: S at 472, S-bar at 1398, R at 5871 (word 0ff9 x17), E f999; mse after TRN 0, in data 0; timing 378; 35706 bits, 0 errors
  14400 bit/s level  2000 snr  0: S at 472, S-bar at 1398, R at 14938 (word 0ff9 x17), E f999; mse after TRN 0, in data 0; timing 92; 35706 bits, 0 errors
  14400 bit/s level  2000 snr 30: S at 472, S-bar at 1398, R at 5871 (word 0ff9 x17), E f999; mse after TRN 101, in data 75; timing 26; 119706 bits, 0 errors
   9600 bit/s level  2000 snr 24: S at 472, S-bar at 1398, R at 5871 (word 0ff9 x17), E fb91; mse after TRN 430, in data 361; timing 18; 79804 bits, 0 errors
   4800 bit/s level  2000 snr 15: S at 475, S-bar at 1398, R at 5871 (word 0ff9 x17), E fd91; mse after TRN 3545, in data 2649; timing 116; 39948 bits, 0 errors
test_rx: 115 checks, 0 failures
  one-way delay 64 samples (8.0 ms), rates offered 1f / 1f
  turn-round at the line: answerer 64.50 T, caller 63.90 T (64 +- 2)
  round trip 39.6 T: MT 103.5 T, NT 103.8 T (each less the other's turn-round: 39.6, 39.3)
  caller saw: S-bar -> R1 1892.7 T (TRN 1831), S-bar -> R3 1334.1 T (TRN 1280)
  echo attenuation after TRN: answerer 45.0 dB, caller 31.5 dB; connected at 14400 bit/s after 2.61 s
  3 s of data: caller -> answerer 42721 bits 0 errors, answerer -> caller 43141 bits 0 errors
  one-way delay 424 samples (53.0 ms), rates offered 1f / 1f
  turn-round at the line: answerer 64.50 T, caller 63.90 T (64 +- 2)
  round trip 255.6 T: MT 319.5 T, NT 319.8 T (each less the other's turn-round: 255.6, 255.3)
  caller saw: S-bar -> R1 1852.8 T (TRN 1791), S-bar -> R3 1333.8 T (TRN 1280)
  echo attenuation after TRN: answerer 44.0 dB, caller 37.4 dB; connected at 14400 bit/s after 3.22 s
  3 s of data: caller -> answerer 42073 bits 0 errors, answerer -> caller 43135 bits 0 errors
  one-way delay 2424 samples (303.0 ms), rates offered 1f / 07
  turn-round at the line: answerer 64.50 T, caller 63.90 T (64 +- 2)
  round trip 1455.6 T: MT 1519.5 T, NT 1519.8 T (each less the other's turn-round: 1455.6, 1455.3)
  caller saw: S-bar -> R1 2796.9 T (TRN 2735), S-bar -> R3 1334.1 T (TRN 1280)
  echo attenuation after TRN: answerer 46.0 dB, caller 48.8 dB; connected at 9600 bit/s after 7.11 s
  3 s of data: caller -> answerer 25627 bits 0 errors, answerer -> caller 28739 bits 0 errors
  one-way delay 32 samples (4.0 ms), rates offered 03 / 1f
  turn-round at the line: answerer 63.90 T, caller 63.60 T (64 +- 2)
  round trip 20.4 T: MT 84.0 T, NT 84.3 T (each less the other's turn-round: 20.4, 20.4)
  caller saw: S-bar -> R1 1873.8 T (TRN 1812), S-bar -> R3 1334.1 T (TRN 1280)
  echo attenuation after TRN: answerer 45.0 dB, caller 31.2 dB; connected at 7200 bit/s after 2.55 s
  3 s of data: caller -> answerer 21358 bits 0 errors, answerer -> caller 21535 bits 0 errors
test_startup: 140 checks, 0 failures
 clean line (6 dB loss, no echo, linear)
  clean, 14400                       14400 bit/s  c->a 114721 bits     0 errors   a->c 115141 bits     0 errors   echo  0 dB  mse    0
  clean, 12000                       12000 bit/s  c->a  95590 bits     0 errors   a->c  95940 bits     0 errors   echo  0 dB  mse    0
  clean, 9600                         9600 bit/s  c->a  76459 bits     0 errors   a->c  76739 bits     0 errors   echo  0 dB  mse    0
  clean, 7200                         7200 bit/s  c->a  57328 bits     0 errors   a->c  57538 bits     0 errors   echo  0 dB  mse    0
  clean, 4800                         4800 bit/s  c->a  38243 bits     0 errors   a->c  38337 bits     0 errors   echo  0 dB  mse    0
 G.711 mu-law at the answerer's interface (sim/channel)
  mu-law, 14400                      14400 bit/s  c->a 114721 bits     0 errors   a->c 115141 bits     0 errors   echo  0 dB  mse   12
  mu-law, 12000                      12000 bit/s  c->a  95590 bits     0 errors   a->c  95940 bits     0 errors   echo  0 dB  mse    6
  mu-law, 9600                        9600 bit/s  c->a  76459 bits     0 errors   a->c  76739 bits     0 errors   echo  0 dB  mse   18
  mu-law, 7200                        7200 bit/s  c->a  57328 bits     0 errors   a->c  57538 bits     0 errors   echo  0 dB  mse   13
  mu-law, 4800                        4800 bit/s  c->a  38243 bits     0 errors   a->c  38337 bits     0 errors   echo  0 dB  mse    5
 hybrid echo: near 15 dB down at both ends, far echo 20 dB down, 25 ms each way
  echo, 14400                        14400 bit/s  c->a 114433 bits     0 errors   a->c 115141 bits     0 errors   echo 33 dB  mse   12
  echo, 12000                        12000 bit/s  c->a  95350 bits     0 errors   a->c  95940 bits     0 errors   echo 33 dB  mse   18
  echo, 9600                          9600 bit/s  c->a  76267 bits     0 errors   a->c  76739 bits     0 errors   echo 33 dB  mse   18
  echo, 7200                          7200 bit/s  c->a  57184 bits     0 errors   a->c  57538 bits     0 errors   echo 33 dB  mse    6
  echo, 4800                          4800 bit/s  c->a  38147 bits     0 errors   a->c  38337 bits     0 errors   echo 33 dB  mse   28
 low level: -30 dBm0 received at both ends, with the same echo (near echo 0 dB re signal)
  -30 dBm0, 14400                    14400 bit/s  c->a 114433 bits     0 errors   a->c 115141 bits     0 errors   echo 32 dB  mse   79
  -30 dBm0, 12000                    12000 bit/s  c->a  95350 bits     0 errors   a->c  95940 bits     0 errors   echo 32 dB  mse   70
  -30 dBm0, 9600                      9600 bit/s  c->a  76267 bits     0 errors   a->c  76739 bits     0 errors   echo 32 dB  mse   65
  -30 dBm0, 7200                      7200 bit/s  c->a  57184 bits     0 errors   a->c  57538 bits     0 errors   echo 32 dB  mse   77
  -30 dBm0, 4800                      4800 bit/s  c->a  38147 bits     0 errors   a->c  38337 bits     0 errors   echo 32 dB  mse   76
 sample clock offset (caller against network), with echo
  +100 ppm, 14400                    14400 bit/s  c->a 114445 bits     0 errors   a->c 115135 bits     0 errors   echo 33 dB  mse   27
  +100 ppm, 12000                    12000 bit/s  c->a  95360 bits     0 errors   a->c  95935 bits     0 errors   echo 33 dB  mse    6
  +100 ppm, 9600                      9600 bit/s  c->a  76275 bits     0 errors   a->c  76735 bits     0 errors   echo 33 dB  mse   17
  +100 ppm, 7200                      7200 bit/s  c->a  57190 bits     0 errors   a->c  57535 bits     0 errors   echo 33 dB  mse    6
  +100 ppm, 4800                      4800 bit/s  c->a  38151 bits     0 errors   a->c  38335 bits     0 errors   echo 33 dB  mse   21
  -100 ppm, 14400                    14400 bit/s  c->a 114421 bits     0 errors   a->c 115141 bits     0 errors   echo 33 dB  mse   14
  -100 ppm, 12000                    12000 bit/s  c->a  95340 bits     0 errors   a->c  95940 bits     0 errors   echo 33 dB  mse    2
  -100 ppm, 9600                      9600 bit/s  c->a  76259 bits     0 errors   a->c  76739 bits     0 errors   echo 33 dB  mse   19
  -100 ppm, 7200                      7200 bit/s  c->a  57178 bits     0 errors   a->c  57538 bits     0 errors   echo 33 dB  mse   21
  -100 ppm, 4800                      4800 bit/s  c->a  38143 bits     0 errors   a->c  38337 bits     0 errors   echo 33 dB  mse   14
 carrier offset (each way), near echo only
  +2 Hz, 14400                       14400 bit/s  c->a 114379 bits     0 errors   a->c 115135 bits     0 errors   echo 33 dB  mse   13
  +2 Hz, 12000                       12000 bit/s  c->a  95305 bits     0 errors   a->c  95935 bits     0 errors   echo 33 dB  mse   22
  +2 Hz, 9600                         9600 bit/s  c->a  76231 bits     0 errors   a->c  76735 bits     0 errors   echo 33 dB  mse    8
  +2 Hz, 7200                         7200 bit/s  c->a  57157 bits     0 errors   a->c  57535 bits     0 errors   echo 33 dB  mse   18
  +2 Hz, 4800                         4800 bit/s  c->a  38129 bits     0 errors   a->c  38335 bits     0 errors   echo 33 dB  mse   19
  -7 Hz, 14400                       14400 bit/s  c->a 114379 bits     0 errors   a->c 115135 bits     0 errors   echo 33 dB  mse   17
  -7 Hz, 12000                       12000 bit/s  c->a  95305 bits     0 errors   a->c  95935 bits     0 errors   echo 33 dB  mse   16
  -7 Hz, 9600                         9600 bit/s  c->a  76231 bits     0 errors   a->c  76735 bits     0 errors   echo 33 dB  mse   10
  -7 Hz, 7200                         7200 bit/s  c->a  57157 bits     0 errors   a->c  57535 bits     0 errors   echo 33 dB  mse   16
  -7 Hz, 4800                         4800 bit/s  c->a  38129 bits     0 errors   a->c  38335 bits     0 errors   echo 33 dB  mse   12
 carrier offset with the far echo as well (20 dB down; its phase roll is not tracked): reported only
  0.2 Hz + far echo, any rate         9600 bit/s  c->a  76231 bits     0 errors   a->c  76735 bits     0 errors   echo 28 dB  mse  140
  1.0 Hz + far echo, any rate        12000 bit/s  c->a  95305 bits     0 errors   a->c  95935 bits     0 errors   echo 19 dB  mse  916
  2.0 Hz + far echo, any rate        12000 bit/s  c->a  95305 bits     0 errors   a->c  95935 bits     0 errors   echo 17 dB  mse  452
  7.0 Hz + far echo, any rate        14400 bit/s  c->a 114379 bits     0 errors   a->c 115135 bits     0 errors   echo 16 dB  mse  358
 white noise at both receivers (signal -20.4 dBm0), mu-law, echo; one rate offered
  SNR 32 dB, 14400                   14400 bit/s  c->a 114433 bits     0 errors   a->c 115135 bits     0 errors   echo 22 dB  mse  112
  SNR 26 dB, 14400                   14400 bit/s  c->a 114433 bits     0 errors   a->c 115135 bits     0 errors   echo 16 dB  mse  331
  SNR 22 dB, 14400                   14400 bit/s  c->a 114433 bits     0 errors   a->c 115135 bits   112 errors   echo 12 dB  mse  685
  SNR 20 dB, 14400                   14400 bit/s  c->a 114433 bits  1230 errors   a->c 115135 bits  2844 errors   echo 10 dB  mse 1068
  SNR 28 dB, 12000                   12000 bit/s  c->a  95350 bits     0 errors   a->c  95940 bits     0 errors   echo 18 dB  mse  224
  SNR 23 dB, 12000                   12000 bit/s  c->a  95350 bits     0 errors   a->c  95940 bits     0 errors   echo 13 dB  mse  667
  SNR 19 dB, 12000                   12000 bit/s  c->a  95350 bits     0 errors   a->c  95940 bits     0 errors   echo  9 dB  mse 1829
  SNR 24 dB, 9600                     9600 bit/s  c->a  76267 bits     0 errors   a->c  76739 bits     0 errors   echo 14 dB  mse  511
  SNR 20 dB, 9600                     9600 bit/s  c->a  76267 bits     0 errors   a->c  76739 bits     0 errors   echo 10 dB  mse 1222
  SNR 16 dB, 9600                     9600 bit/s  c->a  76267 bits     0 errors   a->c  76739 bits     0 errors   echo  7 dB  mse 2863
  SNR 20 dB, 7200                     7200 bit/s  c->a  57184 bits     0 errors   a->c  57538 bits     0 errors   echo 10 dB  mse 1138
  SNR 17 dB, 7200                     7200 bit/s  c->a  57184 bits     0 errors   a->c  57538 bits     0 errors   echo  8 dB  mse 2300
  SNR 13 dB, 7200                     7200 bit/s  c->a  57184 bits     0 errors   a->c  57538 bits     0 errors   echo  4 dB  mse 5388
  SNR 16 dB, 4800                     4800 bit/s  c->a  38147 bits     0 errors   a->c  38335 bits     0 errors   echo  7 dB  mse 3041
  SNR 13 dB, 4800                     4800 bit/s  c->a  38147 bits     0 errors   a->c  38337 bits     0 errors   echo  4 dB  mse 6625
  SNR  9 dB, 4800                     4800 bit/s  c->a  38147 bits   120 errors   a->c  38337 bits   174 errors   echo  2 dB  mse 15770
 the same, all rates offered: the answerer picks from its receiver's error
  SNR 40 dB, any rate                14400 bit/s  c->a 114433 bits     0 errors   a->c 115141 bits     0 errors   echo 28 dB  mse   36
  SNR 32 dB, any rate                14400 bit/s  c->a 114433 bits     0 errors   a->c 115135 bits     0 errors   echo 22 dB  mse  112
  SNR 28 dB, any rate                14400 bit/s  c->a 114433 bits     0 errors   a->c 115135 bits     0 errors   echo 18 dB  mse  210
  SNR 24 dB, any rate                14400 bit/s  c->a 114433 bits     0 errors   a->c 115135 bits     0 errors   echo 14 dB  mse  503
  SNR 20 dB, any rate                 9600 bit/s  c->a  76267 bits     0 errors   a->c  76739 bits     0 errors   echo 10 dB  mse 1379
  SNR 16 dB, any rate                 7200 bit/s  c->a  57184 bits     0 errors   a->c  57538 bits     0 errors   echo  7 dB  mse 3042
  SNR 12 dB, any rate                 4800 bit/s  c->a  38147 bits     6 errors   a->c  38337 bits     0 errors   echo  4 dB  mse 8060
 retrain (7.2), mu-law, echo
  asked for by the caller            both ends retraining 96 ms after the request; data again after 2.90 s at 14400 bit/s; then c->a 114433 bits 0 errors, a->c 115135 bits 0 errors
  asked for by the answerer          both ends retraining 101 ms after the request; data again after 3.02 s at 14400 bit/s; then c->a 114433 bits 0 errors, a->c 115135 bits 0 errors
 driver buffering between the modem and the line (mu-law, echo), echo_delay set to match
  40 samples (5.0 ms)                14400 bit/s  c->a 114433 bits     0 errors   a->c 115141 bits     0 errors   echo 33 dB  mse   18
                                     turn-round at the line 64.5 T
  100 samples (12.5 ms)              14400 bit/s  c->a 114433 bits     0 errors   a->c 115141 bits     0 errors   echo 33 dB  mse   27
                                     turn-round at the line 64.5 T
  160 samples (20.0 ms)              14400 bit/s  c->a 114433 bits     0 errors   a->c 115141 bits     0 errors   echo 31 dB  mse   17
                                     turn-round at the line 68.4 T
 the line turns noisy in data mode (linear, echo; SNR 40 dB, then 19 dB)
  14400 bit/s; noise up: 316 errors in 58993 bits, retrain begun after 2.15 s; back at 9600 bit/s: c->a 76267 bits 0 errors, a->c 76739 bits 0 errors
 the caller disappears in data mode (near echo 15 dB down stays)
  answerer: error 4 (carrier lost) 450 ms after the caller went silent
 no rate in common (answerer 14400 only, caller 7200 only)
  answerer gave up after 5.16 s with error 3 (no rate), then silent
test_call: 358 checks, 0 failures
 V.8, then V.32bis (mu-law, near echo 15 dB, far echo 20 dB down)
  V.8 done at 2.56 s (selected 3), V.32bis data at 5.47 s, 14400 bit/s; c->a 71233 bits 0 errors, a->c 71935 bits 0 errors
 AA detector (1800 Hz during the answer tone)
  AA -20 dBm0, answer tone echo -10 dBm0: detected 104 ms after it starts
  AA -30 dBm0, answer tone echo -10 dBm0: detected 108 ms after it starts
  AA -40 dBm0, answer tone echo -10 dBm0: detected 127 ms after it starts
  1793 Hz and 1807 Hz at -30 dBm0: detected
  answer tone echo alone at -6 dBm0, 5 s: nothing
  1800 Hz at -48 dBm0 (below the -43 dBm0 threshold): nothing
  1650 Hz at -20 dBm0, V.21 CM tones (980 / 1180 Hz) at -12 dBm0: nothing
  white noise at -15 dBm0, 10 s: nothing
 a V.32bis caller without V.8: AA during the answer tone
  AA sent from 1.40 s, detected at 1.54 s; V.32bis data at 4.53 s, 14400 bit/s; c->a 71233 bits 0 errors, a->c 71935 bits 0 errors
test_v8: 28 checks, 0 failures
ALL TESTS PASS
$ make check-fixed
no floating point, no allocation in v32_tabs.c v32_code.c v32_viterbi.c v32_tx.c v32_ec.c v32_rx.c v32.c
```

Reading the noise rows: every rate runs clean down to some SNR and then
falls off steeply, which is what a coded modem should do. In these runs
14400 is clean at 26 dB and has errors at 22 dB (a->c only) and 20 dB;
12000 is still clean at 19 dB, 9600 at 16 dB, 7200 at 13 dB; 4800 is clean
at 13 dB and has errors at 9 dB. The runs are 8 s long, so "0 errors" means
a bit error rate under about 1e-5, no more.

### ARM (armv5 uClibc toolchain, qemu-arm-static): `make test-arm`

The end-to-end tests run a subset and 2 s of data per call under qemu,
because the double-precision peer is slow in soft float. The modem itself
is integer only and `test_bench check` (run by `make cost-arm`) confirms
that the ARM build's transmit samples over a whole recorded call are
identical to the host's.

```
$ make test-arm
== running ARM tests under qemu-arm-static ==
[arm] test_code
   4800 bit/s:   4 points, mean power 40.00
   7200 bit/s:  16 points, mean power 40.00
   9600 bit/s:  32 points, mean power 40.00
  12000 bit/s:  64 points, mean power 42.00
  14400 bit/s: 128 points, mean power 41.00
test_code: 5750 checks, 0 failures
[arm] test_viterbi
   7200 bit/s, sigma 0.870: plain decisions 613 symbol errors in 20000, decoder 0 bit errors in 60000 bits
   9600 bit/s, sigma 0.615: plain decisions 706 symbol errors in 20000, decoder 0 bit errors in 80000 bits
  12000 bit/s, sigma 0.435: plain decisions 749 symbol errors in 20000, decoder 8 bit errors in 100000 bits
  14400 bit/s, sigma 0.307: plain decisions 760 symbol errors in 20000, decoder 0 bit errors in 120000 bits
test_viterbi: 484 checks, 0 failures
[arm] test_tx
   4800 bit/s scrambled ones: line RMS 2997 for level 3000
   7200 bit/s scrambled ones: line RMS 3005 for level 3000
   9600 bit/s scrambled ones: line RMS 2998 for level 3000
  12000 bit/s scrambled ones: line RMS 3073 for level 3000
  14400 bit/s scrambled ones: line RMS 3023 for level 3000
   4800 bit/s: 600 Hz -3.7 dB, 3000 Hz -3.7 dB, 200 Hz -42.8 dB, 3500 Hz -46.9 dB re maximum
   7200 bit/s: 600 Hz -4.4 dB, 3000 Hz -4.4 dB, 200 Hz -43.8 dB, 3500 Hz -47.0 dB re maximum
   9600 bit/s: 600 Hz -3.2 dB, 3000 Hz -3.2 dB, 200 Hz -43.9 dB, 3500 Hz -46.9 dB re maximum
  12000 bit/s: 600 Hz -3.8 dB, 3000 Hz -3.7 dB, 200 Hz -42.9 dB, 3500 Hz -46.7 dB re maximum
  14400 bit/s: 600 Hz -4.5 dB, 3000 Hz -4.5 dB, 200 Hz -42.2 dB, 3500 Hz -46.5 dB re maximum
  14400 bit/s: worst deviation of a demodulated point 0.130 units (points are 1.41 apart)
test_tx: 2237 checks, 0 failures
[arm] test_ec
  near echo, 1280 symbols of TRN: 50.7 dB
  near echo, 3000 symbols of TRN: 52.6 dB
  near + far echo at 40 ms, bulk delay set: 54.5 dB
  near + far echo at 40 ms, bulk delay wrong: 0.6 dB
  near + far echo at 600 ms: 55.3 dB
  then 16 s of double talk, frozen: 54.5 dB before, 55.9 dB after
  then 16 s of double talk at step 2^-15: 54.5 dB before, 52.5 dB after
  then 16 s of double talk at step 2^-11: 32.5 dB after (too fast)
  double talk at the training step 2^-2: 2.9 dB after
test_ec: 9 checks, 0 failures
[arm] test_rx
   4800 bit/s level  2000 snr  0: S at 472, S-bar at 1398, R at 5871 (word 0ff9 x17), E fd91; mse after TRN 0, in data 0; timing -52; 11948 bits, 0 errors
   7200 bit/s level  2000 snr  0: S at 472, S-bar at 1398, R at 5871 (word 0ff9 x17), E f9d1; mse after TRN 0, in data 0; timing 19; 17853 bits, 0 errors
   9600 bit/s level  2000 snr  0: S at 472, S-bar at 1398, R at 5871 (word 0ff9 x17), E fb91; mse after TRN 0, in data 0; timing 117; 23804 bits, 0 errors
  12000 bit/s level  2000 snr  0: S at 472, S-bar at 1398, R at 5871 (word 0ff9 x17), E f9b1; mse after TRN 0, in data 0; timing 10; 29755 bits, 0 errors
  14400 bit/s level  2000 snr  0: S at 472, S-bar at 1398, R at 5871 (word 0ff9 x17), E f999; mse after TRN 0, in data 0; timing 377; 35706 bits, 0 errors
  14400 bit/s level   160 snr  0: S at 484, S-bar at 1398, R at 5871 (word 0ff9 x17), E f999; mse after TRN 1, in data 0; timing 378; 35706 bits, 0 errors
  14400 bit/s level  8000 snr  0: S at 472, S-bar at 1398, R at 5871 (word 0ff9 x17), E f999; mse after TRN 0, in data 0; timing 378; 35706 bits, 0 errors
  14400 bit/s level  2000 snr  0: S at 472, S-bar at 1398, R at 14938 (word 0ff9 x17), E f999; mse after TRN 0, in data 0; timing 92; 35706 bits, 0 errors
  14400 bit/s level  2000 snr 30: S at 472, S-bar at 1398, R at 5871 (word 0ff9 x17), E f999; mse after TRN 101, in data 75; timing 26; 119706 bits, 0 errors
   9600 bit/s level  2000 snr 24: S at 472, S-bar at 1398, R at 5871 (word 0ff9 x17), E fb91; mse after TRN 430, in data 361; timing 18; 79804 bits, 0 errors
   4800 bit/s level  2000 snr 15: S at 475, S-bar at 1398, R at 5871 (word 0ff9 x17), E fd91; mse after TRN 3545, in data 2649; timing 116; 39948 bits, 0 errors
test_rx: 115 checks, 0 failures
[arm] test_startup
  one-way delay 64 samples (8.0 ms), rates offered 1f / 1f
  turn-round at the line: answerer 64.50 T, caller 63.90 T (64 +- 2)
  round trip 39.6 T: MT 103.5 T, NT 103.8 T (each less the other's turn-round: 39.6, 39.3)
  caller saw: S-bar -> R1 1892.7 T (TRN 1831), S-bar -> R3 1334.1 T (TRN 1280)
  echo attenuation after TRN: answerer 45.0 dB, caller 31.5 dB; connected at 14400 bit/s after 2.61 s
  3 s of data: caller -> answerer 42721 bits 0 errors, answerer -> caller 43141 bits 0 errors
test_startup: 35 checks, 0 failures
[arm] test_call
 hybrid echo: near 15 dB down at both ends, far echo 20 dB down, 25 ms each way
  echo, 14400                        14400 bit/s  c->a  28033 bits     0 errors   a->c  28741 bits     0 errors   echo 33 dB  mse   17
  echo, 12000                        12000 bit/s  c->a  23350 bits     0 errors   a->c  23940 bits     0 errors   echo 33 dB  mse   16
  echo, 9600                          9600 bit/s  c->a  18667 bits     0 errors   a->c  19139 bits     0 errors   echo 33 dB  mse   22
  echo, 7200                          7200 bit/s  c->a  13984 bits     0 errors   a->c  14338 bits     0 errors   echo 33 dB  mse   23
  echo, 4800                          4800 bit/s  c->a   9347 bits     0 errors   a->c   9537 bits     0 errors   echo 33 dB  mse   23
 low level: -30 dBm0 received at both ends, with the same echo (near echo 0 dB re signal)
  -30 dBm0, any rate                 14400 bit/s  c->a  28033 bits     0 errors   a->c  28741 bits     0 errors   echo 32 dB  mse   91
 white noise at both receivers (signal -20.4 dBm0), mu-law, echo; one rate offered
  SNR 26 dB, 14400                   14400 bit/s  c->a  28033 bits     0 errors   a->c  28741 bits     0 errors   echo 16 dB  mse  345
  SNR 13 dB, 4800                     4800 bit/s  c->a   9347 bits     0 errors   a->c   9537 bits     0 errors   echo  4 dB  mse 7184
 the same, all rates offered: the answerer picks from its receiver's error
  SNR 20 dB, any rate                 9600 bit/s  c->a  18667 bits     0 errors   a->c  19139 bits     0 errors   echo 10 dB  mse 1508
 retrain (7.2), mu-law, echo
  asked for by the caller            both ends retraining 96 ms after the request; data again after 2.90 s at 14400 bit/s; then c->a 28033 bits 0 errors, a->c 28741 bits 0 errors
 the caller disappears in data mode (near echo 15 dB down stays)
  answerer: error 4 (carrier lost) 450 ms after the caller went silent
 no rate in common (answerer 14400 only, caller 7200 only)
  answerer gave up after 5.16 s with error 3 (no rate), then silent
test_call: 70 checks, 0 failures
[arm] test_v8
 V.8, then V.32bis (mu-law, near echo 15 dB, far echo 20 dB down)
  V.8 done at 2.56 s (selected 3), V.32bis data at 5.47 s, 14400 bit/s; c->a 28033 bits 0 errors, a->c 28735 bits 0 errors
 AA detector (1800 Hz during the answer tone)
  AA -20 dBm0, answer tone echo -10 dBm0: detected 104 ms after it starts
  AA -30 dBm0, answer tone echo -10 dBm0: detected 108 ms after it starts
  AA -40 dBm0, answer tone echo -10 dBm0: detected 127 ms after it starts
  1793 Hz and 1807 Hz at -30 dBm0: detected
  answer tone echo alone at -6 dBm0, 5 s: nothing
  1800 Hz at -48 dBm0 (below the -43 dBm0 threshold): nothing
  1650 Hz at -20 dBm0, V.21 CM tones (980 / 1180 Hz) at -12 dBm0: nothing
  white noise at -15 dBm0, 10 s: nothing
 a V.32bis caller without V.8: AA during the answer tone
  AA sent from 1.40 s, detected at 1.54 s; V.32bis data at 4.53 s, 14400 bit/s; c->a 28033 bits 0 errors, a->c 28735 bits 0 errors
test_v8: 28 checks, 0 failures
ALL ARM TESTS PASS
```

## CPU cost

`make cost-arm`: the host records what the answerer receives during a whole
call (mu-law line, echo, noise at -60 dBm0); qemu replays those samples into
the ARM build of the modem alone with `-one-insn-per-tb -d exec,nochain`,
and the instructions executed between two sample counts inside one stretch
of the call are divided by the number of samples.

```
$ make cost-arm
== ARM instructions per sample (qemu -one-insn-per-tb), 8000 samples/s ==
  reference FIR: 6.09 instructions per MAC
  replay of 46183 samples: transmit checksum b1d5acf1 (recorded b1d5acf1), connected at 14400 bit/s: same as the host run
  rate 4, TRN1 (echo canceller training): 5813 instructions per sample = 954 MACs, about 30.0 % of the box
  rate 4, receiver training (caller's TRN): 2477 instructions per sample = 406 MACs, about 12.7 % of the box
  rate 4, data, both ways: 3855 instructions per sample = 633 MACs, about 19.9 % of the box
  replay of 46135 samples: transmit checksum 0388c0cc (recorded 0388c0cc), connected at 4800 bit/s: same as the host run
  rate 0, data, both ways: 2584 instructions per sample = 424 MACs, about 13.3 % of the box
```

| Stretch | Instructions per sample | per second | Reference FIR MACs per sample | Share of the box (estimate) |
|---|---|---|---|---|
| TRN1, echo canceller adapting (about 0.8 s, more on long round trips) | 5813 | 46.5 M | 954 | 30.0 % |
| receiver training on the caller's TRN | 2477 | 19.8 M | 406 | 12.7 % |
| data at 14400 bit/s, both ways | 3855 | 30.8 M | 633 | 19.9 % |
| data at 4800 bit/s, both ways | 2584 | 20.7 M | 424 | 13.3 % |

The last column is an estimate, not a measurement: it uses the project's
reference, the FIR kernel of `../stage0/bench.c`, which ran at 25.4 MMAC/s
on the real 400 MHz ARM926 and takes 6.09 instructions per MAC in the same
qemu count, and assumes this code needs as many cycles per instruction as
that loop. It has more 64-bit arithmetic, branches and table lookups, so
the true figure is probably higher. Nothing was timed on the box.

Where it goes: the difference between the 14400 and 4800 rows is the
trellis decoder, about 4200 instructions per symbol (1270 per sample), most
of it a plain search for the nearest point of each of the eight subsets
among 128 points, which a table-driven slicer would cut several times over.
The difference between TRN1 and the rest is the echo canceller's adaptation
(192 coefficient updates and one 64-bit division per sample). The remainder
was not broken down by measurement; by tap count the two frozen 96-tap echo
filters with 64-bit accumulation should be the largest part of it, then the
equaliser (32 complex taps, filter and update per symbol) and the receive
filter bank. None of it has been optimised.

## Not done / not verified

* **No real modem has been called.** Everything above is this modem against
  `sim/v32call`, which the same author wrote from the same Recommendation.
  The two share no code (the peer is double precision, has its own filters,
  its own gate-level trellis encoder and Viterbi decoder, and reads Tables 1
  and 2 from the hand-typed `sim/v32spec.h`), and `test_code` / `test_tx`
  check the transmitted signals against the Recommendation's tables rather
  than against the receiver. A wrong reading of the Recommendation that both
  sides share would still pass. The points most at risk are listed under
  "Spec readings chosen".
* **Rate renegotiation (section 8) is not implemented**, in either
  direction, although the Recommendation requires a modem to respond to it
  ("shall be conditioned to detect an incoming preamble at any time while
  receiving data"). A rate change is only possible through a full retrain.
  A caller that starts the procedure (its preamble is AA for 56 T and CC for
  8 T, then R4) is not understood: 56 T of AA is too short to be taken for a
  retrain request, R4 decodes as garbage data, and if the caller persists
  the answerer retrains by itself after 2 s of poor reception. Real V.32bis
  modems do use this procedure to fall back and forward, so this is the
  first thing to add before trusting an interop result.
* **V.32 (non-bis) interworking is not complete.** This directory has the
  V.32bis text only. A caller whose rate signal has B4 or B8 clear (i.e. not
  a V.32bis rate signal) is offered 4800 bit/s only, which is uncoded and
  the same in both Recommendations as far as V.32bis describes it. V.32's
  9600 bit/s modes and the meaning of its rate signal bits were not
  implemented because their definition needs the V.32 text. The trellis
  9600 constellation here is V.32bis Figure 2-3, which is expected to be
  V.32's, but that was not checked.
* **Far echo with frequency offset is not tracked.** The far canceller has
  no phase roll compensation. With a carrier offset on the connection the
  far echo comes back shifted and is not cancelled; `test_call` shows what
  that costs (far echo 20 dB down: the rate falls to 9600 or 12000, still
  error free, because R3 is chosen from the error actually seen). With no
  offset, or a weak far echo, nothing is lost. Near echo is unaffected.
* **The echo canceller is frozen after training.** A hybrid that drifts
  during a long call is only corrected by a retrain. (A crawl at step 2^-15
  in double talk was measured as harmless in `test_ec` but is not switched
  on.)
* **Annex A automode sequencing** (USB1, timers Ta / Tc, falling back to
  V.22bis) is not in this directory; only the AA detector is. See the
  wiring notes.
* **Calling mode** exists only as the double-precision test peer.
* **Echo path longer than 12 ms** at either end (near) or a far echo spread
  over more than 12 ms is not covered by the two windows.
* **Not measured on the box.** CPU cost is instruction counts under qemu;
  the share of the CPU is an estimate (see "CPU cost").
* **I/O delay over about 150 samples** makes the 64 T turn-round late by the
  excess (see `cfg.echo_delay`); a strict caller may then misplace its far
  echo canceller.
* Test-first was not followed strictly: for most blocks the test and the
  code were written together and the test was run against the code as it
  grew. Every figure quoted in this file is from an actual run.
* The tests' channel is clean in one respect that real lines are not: no
  impulse noise, no phase jitter, no amplitude or delay distortion beyond
  the short hybrid responses and what `sim/channel` does for mu-law.

## Spec readings chosen

Where the Recommendation leaves room, this is what was implemented. Each is
a candidate for a mistake that the tests cannot see.

1. **Convolutional encoder, Figure 1.** The figure was read from the PDF
   image as: with delay elements a, b, c (c is the output Y0),
   `a' = c`, `b' = a ^ Y1 ^ Y2 ^ (c & (b ^ Y2))`, `c' = b ^ Y2 ^ (Y1 & c)`.
   The check available is internal: with this encoder the minimum squared
   distance between allowed sequences is that of the 8-state code (the
   decoder corrects what plain decisions get wrong at 3.5 % symbol errors,
   `test_viterbi`), and a 90 degree rotation of the whole signal decodes to
   the same data. A different wiring with the same properties would not be
   caught.
2. **Signal points, Figures 2-1 to 2-5.** The coordinates were extracted
   from the PDF by the positions of the printed labels and cross-checked
   against a hand transcription of the rows (`sim/v32spec.h`). Relative
   scale between rates: all on the grid of Figure 2-1, 9600 / 7200 / 4800
   doubled, which gives mean powers 41, 42, 40, 40, 40 for 14400 down to
   4800; the transmitter uses one gain for all of them, so the line power
   differs between rates by at most 0.2 dB.
3. **Differential encoder start.** The first data symbol's differential
   encoding continues from the Y1 Y2 of the last E symbol (4800 coding),
   and the trellis delay elements start at zero (6.2 says the latter; the
   former is our reading of "no break").
4. **Rate sequence bit order.** B0 is sent first and is the most
   significant bit of the 16-bit word here; dibits are B0B1, B2B3, ... each
   coded with Table 2.
5. **Detecting a rate signal.** "At least two consecutive identical 16-bit
   rate sequences": the receiver finds the frame from the synchronising
   bits (B0-B3 = 0, B7 = B11 = B15 = 1), and accepts a word when two
   consecutive frames are identical and a third has valid synchronising
   bits.
6. **R3 names a single rate** (the highest the receiver can take within
   R2), not a set.
7. **R1 is cut mid-sequence** when the caller's S arrives ("on detection of
   an incoming S sequence, the modem shall cease transmitting"); R3 and E
   are whole sequences.
8. **Waiting MT.** After detecting S we stay silent for MT plus our own
   transmit filter delay, counted from the detection, then train on S if it
   is still there or when it returns.
9. **TRN extension** (first conditioning signal): 1280 symbols plus the
   round trip, then blocks of 256 while the echo keeps falling, up to 8192.
   No separate echo cancellation sequence (Note 3) is used.
10. **Second TRN** is exactly 1280 symbols.
11. **Circuit 104 unclamped 128 symbols after E** is received (the caller's
    B1), by discarding that many decoded symbols.
12. **The turn-round** is 64 T measured between pulse centres at the line,
    with CA an even number of symbols; the nearest even count is used, so
    the result is within one symbol of 64 T (63.9 to 64.5 T in `test_startup`).
13. **AA in data mode** for more than 128 T, as the only thing on the line,
    is the caller's retrain request (7.2); the answerer's own request is
    AC, which is simply the start-up again.
14. **Carrier detect levels.** The Recommendation gives none. Tones and S
    are detected from -43 dBm0; in data mode the line signal is taken as
    lost below about -48 dBm0 for 400 ms.
15. **After AA is detected during the answer tone** (A.2.2: "continue at
    the second paragraph of 6.2"), the test leaves 75 ms of silence between
    the answer tone and AC, as V.25 has after an answer tone. A.2.2 does not
    say whether the silence is kept.
16. **No common rate / V.32 caller**: see "Not done".
17. **When to give a rate up**: reception worse than a per-rate threshold
    for 2 s starts a retrain; a fourth such retrain without a good minute
    in between ends the call. This is ours, not the Recommendation's (7.2
    only says a modem may retrain when it detects unsatisfactory
    reception).

## Wiring into modem/answer.c

Not done here (the file is outside this directory's remit); `test/test_v8.c`
does the same steps with `modem/v8ans` and is the model.

**V.8 bit.** V.32bis and V.32 are item 3 of Table 4/V.8: `V8_MOD_V32` in
`modem/v8msg.h`. Offer it by setting that bit in `answer_cfg.modes`;
`v8_selected()` then returns 3 when V.32 is the lowest-numbered common
modulation (it comes before V.22, item 4, so a caller offering both gets
V.32bis). V.34 (item 1) and V.90 keep their precedence.

**Hand-over.** `v8ans_tick` returns `V8A_DONE` after CJ and its 75 ms of
silence. On that sample, with `a->v8.selected == 3`:

    v32_cfg c;
    c.level = a->cfg.level; c.rates = V32_RALL;
    c.getbit = a->cfg.getbit; c.putbit = a->cfg.putbit; c.user = a->cfg.user;
    c.echo_delay = a->echo_delay;        /* same meaning as answer_v90_line's */
    c.no_auto_retrain = 0;
    v32_init(&a->v32, &c);
    a->phase = ANS_V32; a->t = 0;

and from the next sample `tx = v32_tick(&a->v32, rx)`. No further delay is
needed: `v32_init` starts AC, which is what the caller waits for, and the
caller's AA starts within its own 75 ms. (`getbit` / `putbit` have the same
types as `v22_getbit_fn` / `v22_putbit_fn`.) Measured in `test_v8`: V.8 done
at 2.56 s, data at 5.47 s.

**Per sample, in the new `case ANS_V32:`**

    tx = v32_tick(&a->v32, rx);
    if (a->v32.state == V32S_FAILED) { fail(a); break; }
    if (a->v32.connected && !a->connected) {
        a->connected = 1; a->modulation = ANS_MOD_V32;   /* new constant */
    }
    if (a->v32.connected) {
        a->bitrate = a->rx_bitrate = a->v32.bitrate;     /* may change at a retrain */
        a->retraining = a->v32.retraining;
        a->retrains = a->v32.retrains;
    }

`connected` and `bitrate` are valid as soon as `v32.connected` is 1; the bit
pump is not called before that, nor during a retrain. Loss of carrier and a
hopeless line both end in `V32S_FAILED` (`V32_ERR_CARRIER`), so answer.c
needs no carrier timer of its own for this phase. A start-up that never
completes ends in `V32S_FAILED` after at most 20 s (no AA) or 10 s (training).

`answer_t` grows by `sizeof(v32_t)`, 36 KB. If that matters, the 32 KB
transmit history can share storage with other phases' buffers, since only
one modulation runs at a time; nothing here does that.

**A caller without V.8 (V.32bis 6.1, A.2.2).** Such a caller answers the
answer tone with AA (1800 Hz) after hearing it for a second, and sends no
CM. While `a->v8.state == V8A_ANSAM`, also run

    if (v32_aadet_tick(&a->aadet, rx)) { ... }      /* v32_aadet_init at answer_init */

and when it fires (1800 Hz at -43 dBm0 or more, 12 dB above what there is
at 2400 Hz, for 100 ms), abandon V.8: send 75 ms of silence, then `v32_init`
as above. The detector must only be fed during ANSam: our own JM has a
tone at 1850 Hz, and a V.21 CM does not set it off (tested), but our JM's
echo could. `test_v8` runs this path: AA from 1.40 s, detected at 1.54 s,
data at 4.53 s. Under the echo of our own answer tone at -10 dBm0 the
detector fires 104 to 127 ms after AA at -20 to -40 dBm0 starts, and not at
all on that echo alone, on 1650 Hz, on V.21 tones or on white noise at
-15 dBm0.

If no AA and no CM arrive during ANSam, answer.c today goes to the V.22bis
answer sequence. A.2.2 adds one step for a V.32 caller that did not get its
AA through during the tone: if neither S1 nor SB1 of V.22bis is detected
within Ta = 3000 +- 50 ms of USB1, continue with AC (i.e. `v32_init`). That
timer belongs in answer.c's ANS_V22 legacy branch, next to the existing
fall-back to V.21, and is not implemented or tested here.
