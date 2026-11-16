# V.34 answering (full duplex, primary channel): report

State of the working copy `a separate working copy`. Everything below was
run in simulation only. **No real modem has ever called this code**, and the
calling peer used in the tests is built from the same blocks by the same
author (see "Not done, not verified").

## 1. What exists

A V.34 answer modem from the end of V.8 to data mode and back out of it:

    V.8 (modem/v8ans, unchanged)
      -> Phase 2, answer role          modem/p2ans     INFO0a, Tone A, reversals, RTD, L1/L2, INFO1a
      -> Phases 3 and 4, data          modem/v34dx     S, S-bar, PP, TRN, J / J', MP, MP', E, B1, data
         rate renegotiation, cleardown                 (role parameter: answer or call)
      -> retrain: 70 ms silence, Phase 2 again         modem/v34ans    the session; this is what answer.c calls

Transmit side: `v34/` (bits to points, now with the non-linear encoder),
`modem/v34mp` (J, J', MP, MP' contents), `modem/v34mod` (points to 8 kHz
samples: pulse shaping, carrier, pre-emphasis, power). Receive side:
`modem/ec` (near and far echo), `modem/qamrx`, `modem/v34train`, `v34/`.

Symbol rates 2400, 3000 and 3200 with both carriers each (clause 5 makes
exactly these three mandatory); 2743, 2800 and 3429 are declined in INFO0a.
One symbol rate for both directions (INFO0a bits 21:23 = 0). Data rates
2400 to 31200 bit/s, asymmetric when both MPs allow it. No auxiliary
channel, no half duplex, no MD of our own.

### Files added

| file | what |
|---|---|
| `modem/v34mp.h`, `.c` | J, J' patterns (Tables 18, 19), MP / MP' Type 0 and 1 build and parse with CRC (Tables 20, 21), the rate decision of 11.4.1.x.3, cleardown test |
| `modem/p2ans.h`, `.c` | Phase 2, answer role (11.2.1.2), recovery (11.2.2.2), Phase 2 of a retrain (11.5.2); symbol rate / carrier / projected rate decision; `v34_symbol_rate()`, `v34_carrier()` (Tables 1, 2) |
| `modem/v34mod.h`, `.c`, `modem/v34mod_tabs.h` | QAM modulator, fixed-point; the tables are generated |
| `tools/gen_v34mod_tabs.py` | generator of those tables (pure Python): root-raised-cosine polyphase pulse, the ten pre-emphasis filters per symbol rate, power-restoring gains |
| `modem/v34dx.h`, `.c` | Phases 3 and 4, data mode, rate renegotiation, cleardown, retrain detection; answer role and call role |
| `modem/v34ans.h`, `.c` | the answer session (p2ans, v34dx, retrain loop); `v34_call_cfg_from_v90()` for the V.90 fallback |
| `v34/v34_nl.c`, `v34/test/test_nl.c` | non-linear encoder (9.7) and its inverse |
| `sim/p2c.h`, `.c` | test peer: Phase 2, call role |
| `sim/v34peer.h`, `.c` | test peer: V.34 calling modem (sim/v8call with a CM without PCM, p2c, v34dx in the call role, retrains) |
| `test/test_v34mp.c`, `test_p2ans.c`, `test_v34mod.c`, `test_v34dx.c`, `test_v34call.c` | tests, all in `TESTS` of the top Makefile |
| `test/dxline.h`, `test/line34.h` | line models and bit sources / sinks for those tests |
| `tools/rec_v34.c`, `rig/bench_v34.c`, `rig/v34glue.h` | record a simulated call, replay it on host and ARM (bit-exactness, CPU cost) |

### Existing files modified (all backward compatible)

| file | change | why |
|---|---|---|
| `Makefile` | new sources in `MODEM_SRC` / `SIM_SRC`, five tests in `TESTS`, targets `bench-v34`, `cost-v34` | build and run the new code |
| `modem/ec.h`, `ec.c` | new function `ec_adapt()` (the NLMS step alone); `ec_tick()` untouched | near and far canceller in a row must both learn from the error left after both |
| `modem/v34train.h`, `.c` | `sdet_t` gets a field `div`, set to 8 by `sdet_init()` (the old constant); `sdet_tick()` uses it | on a loop that tilts the three lines of S the old threshold (carrier line > 1/4 of the power) never fires; v34dx sets 16 in Phase 3 only. V.90 behaviour is unchanged (tests `v34train`, `sres`, `p34`, `answer_v90` pass) |
| `v34/v34.h` | declares `v34_nl_encode()`, `v34_nl_decode()`; comment of `v34_params_init()` | non-linear encoder |
| `v34/v34_params.c` | no longer returns -3 for `cfg.nonlinear` | the caller's MP may ask for it |
| `v34/v34_tx.c` | `v34_tx_symbol()` passes x through `v34_nl_encode()` (identity when `nonlinear` = 0) | same |
| `v34/test/test_params.c` | line 78 expected -3, now 0 | same |
| `v34/Makefile`, `v34/README.md` | `v34_nl.c`, `test_nl`; the README paragraph on the non-linear encoder | same |

`modem/answer.c` and `answer.h` were not touched.

## 2. What works, with the evidence

Host: `make test`

    g711: 19 checks, 0 failures; noise: 8; filt: 14; resamp: 10; channel: 13; v21: 9; ansam: 7; v8msg: 41;
    v8rx: 19; v8ans: 21; v22tx: 25; v22rx: 28; v22bis: 28; v14: 15; answer: 25; modemterm: 6; info: 131;
    dpsk: 277; probe: 121; p2: 106; qamrx: 18; ec: 504; v34train: 48; sres: 56; answer_v90: 32; p34: 348;
    pcmenc: 8                                        (the 27 existing tests, all 0 failures)
    v34mp: 126 checks, 0 failures
    p2ans: 171 checks, 0 failures
    v34mod: 248 checks, 0 failures
    v34dx: 676 checks, 0 failures
    v34call: 296 checks, 0 failures
    ALL TESTS PASS

ARM (uClibc toolchain, qemu-arm-static): `make test-arm`

    @@ARM_TOP@@

`make -C v34 test` and `make -C v34 test-arm`

    @@V34_LIB@@

Bit-exactness host / ARM on a whole recorded call (`make bench-v34`):

    recorded 16.0 s; data mode from 8.10 s; 3200 sym/s, 28800 bit/s out, 28800 bit/s in; bits in 227448 (0 bad), far end 227520 (0 bad)
    transmit hash 0793a29c                                       (host, while recording)
    replayed 16.0 s; transmit hash 0793a29c; ... 28800 bit/s out, 28800 bit/s in; bits from line 227448 (0 bad)    (host replay)
    replayed 16.0 s; transmit hash 0793a29c; ... 28800 bit/s out, 28800 bit/s in; bits from line 227448 (0 bad)    (ARM under qemu)

### Per block

* **MP / J (`test_v34mp`)**: bit strings written out from Tables 18 to 21, an
  independent model of the CRC of Figure 14, the rate decision cases.
* **Phase 2 (`test_p2ans`)**, against `sim/p2c` over a path with delay, loss,
  echo, noise, band limits: INFO0a contents, round-trip delay within 1 ms at
  5.5 ms and 105 ms (also with 20 ms of driver delay compensated), L1
  exactly 160 ms and 6 dB above L2, L2 no longer than 550 ms + RTD, Tone A
  for 50 ms before the second reversal, INFO1a contents, symbol rate chosen (3200 / 3000 / 2400 as the
  band narrows), the recoveries of 11.2.2.2 (INFO0c lost, INFO0a lost, a
  reversal lost, INFO1c lost: own retrain), retrain entry, dead line.
* **Modulator (`test_v34mod`)**: power within 0.04 dB of nominal for 3 symbol
  rates x 2 carriers x 11 pre-emphasis settings; power reduction 1..7 dB;
  the pre-emphasis templates of Figures 1 and 2 met within 0.11 dB (1 dB
  allowed) over carrier +-0.45 S; symbol clock exact; the receiver core
  trains on it to -48 dB at 2400, 3000 and 3200.
* **Non-linear encoder (`v34/test/test_nl`)**: (9-33) to (9-35) against a
  double model within 1/128 unit, inverse exact, encoder output power +0.64 dB.
* **Engine (`test_v34dx`)**, answer role against call role, direct path:

      clean, answer on the low/high carrier   3200 sym/s  A->C 31200  C->A 31200   124800 bits each way, 0 errors
                                               3000 sym/s  A->C 28800  C->A 28800   0 errors
                                               2400 sym/s  A->C 21600  C->A 21600   0 errors
      noise 38 dB below the signal   28800 / 28800   230400 bits each way, 0 errors
      noise 32 dB                    24000 / 24000   0 errors
      noise 26 dB                    16800 / 16800   0 errors
      noise 20 dB                     9600 /  9600   0 errors
      noise 14 dB                     4800 /  4800   0 errors
      16-point TRN / MP / E; 32-state; 64-state; non-linear encoder; expanded shaping; precoder (MP Type 1);
      pre-emphasis 7 with 3 dB power reduction; all together; all asked of the caller too:
                                     31200 / 31200, 0 errors in each of the nine runs
      echo delay found: 154 samples (true 160)                        31200 / 31200, 0 errors
      far echo 80 ms late, 16 dB down: echo down 59 dB                31200 / 31200, 0 errors
      asymmetric: ceilings 19200 / 26400                              A->C 19200, C->A 26400, 0 errors
      renegotiation took 371 ms; begun by the answer modem            0 errors after it
      renegotiation took 313 ms; begun by both at once                0 errors after it
      renegotiation took 371 ms; answer modem, new ceiling 14400      C->A 14400, 0 errors
      noise rises to -27 dB in data mode: 1 renegotiation of our own  C->A 31200 -> 16800, 0 errors after it
      Tone B in data mode: retrain after 61 ms
      caller's signal lost in data mode: carrier lost after 522 ms
      as call modem after V.90 Phase 2 (configuration from INFO1a)    31200 / 28800, 0 errors

  The same test asserts the segment lengths and limits of 11.3 and 11.4:
  S 128 T, S-bar 16 T, PP 288 T, TRN >= 512 T and <= 2000 ms, J in whole
  repetitions, exactly one J' from the call modem, the caller's S within
  600 ms of the start of our J, J' within 100 ms + RTD of our S-bar, Phase 4
  TRN >= 512 T and <= 2000 ms, MP in whole sequences, E 20 bits (10 or 5
  symbols), data within 2500 ms + 3 RTD of the start of Phase 4; and the
  time-outs that must end in a retrain (no S after J: at 600 ms + RTD; no J;
  no E in Phase 4), cleardown both ways, a configuration that does not exist.
* **Whole calls (`test_v34call`)**: `modem/v8ans`, then `modem/v34ans`,
  driven the way `answer.c` will (the glue is in the test and in
  `rig/v34glue.h`), against `sim/v34peer` starting with its V.8 CM, over
  `test/line34.h`. Base line: mu-law codec at our side, loss 6 dB, both
  hybrids 0.3, round trip 5.5 ms.

      no codec (linear)              3200 sym/s  A->C 31200 bit/s  156000 bits   0 err   C->A 31200 bit/s  156000 bits   0 err  (data at 7.9 s)
      G.711 mu-law                   3200 sym/s  A->C 28800 bit/s  144000 bits   0 err   C->A 28800 bit/s  144000 bits   0 err  (data at 8.1 s)
      silence between INFO1a and S: 69.9 ms; round trip measured 45 samples (true 44)
      G.711 A-law                    3200 sym/s  A->C 28800 bit/s  144000 bits   0 err   C->A 28800 bit/s  144000 bits   0 err  (data at 7.9 s)
      mild loop                      3200 sym/s  A->C 28800 bit/s  144000 bits   0 err   C->A 26400 bit/s  132000 bits   0 err  (data at 8.1 s)
      long loop                      3000 sym/s  A->C 12000 bit/s   60000 bits   0 err   C->A 12000 bit/s   60000 bits   0 err  (data at 8.2 s)
      loop past anything real        3200 sym/s  A->C  4800 bit/s   24000 bits   0 err   C->A  4800 bit/s   24000 bits   0 err  (data at 7.9 s)
      strong echo, own delay unknown 3200 sym/s  A->C 28800 bit/s  144000 bits   0 err   C->A 21600 bit/s  108000 bits   0 err  (data at 8.4 s)
      far echo 55 ms late, same      3200 sym/s  A->C 28800 bit/s  144000 bits   0 err   C->A 28800 bit/s  144000 bits   0 err  (data at 8.7 s)
      noise 34 dB down               3200 sym/s  A->C 24000 bit/s  192000 bits   0 err   C->A 21600 bit/s  172800 bits   0 err  (data at 8.1 s)
      noise 28 dB down               3200 sym/s  A->C 19200 bit/s  153600 bits   0 err   C->A 19200 bit/s  153600 bits   0 err  (data at 8.1 s)
      noise 22 dB down               3200 sym/s  A->C 12000 bit/s   96000 bits   0 err   C->A 12000 bit/s   96000 bits   0 err  (data at 8.1 s)
      caller's clock +100 ppm        3200 sym/s  A->C 28800 bit/s  230400 bits   0 err   C->A 26400 bit/s  211200 bits   0 err  (data at 8.1 s)
      caller's clock -100 ppm        3200 sym/s  A->C 28800 bit/s  230400 bits   0 err   C->A 28800 bit/s  230400 bits   0 err  (data at 7.9 s)
      frequency offset +-1.5 Hz      3200 sym/s  A->C 28800 bit/s  230400 bits   0 err   C->A 26400 bit/s  211200 bits   0 err  (data at 8.1 s)
      caller asks for everything     3200 sym/s  A->C 28800 bit/s  144000 bits   0 err   C->A 28800 bit/s  144000 bits   0 err  (data at 8.2 s)
      caller without 3200            3000 sym/s  A->C 28800 bit/s  115200 bits   0 err   C->A 26400 bit/s  105600 bits   0 err  (data at 8.2 s)
      2400 symbols/s only            2400 sym/s  A->C 21600 bit/s   86400 bits   0 err   C->A 19200 bit/s   76800 bits   0 err  (data at 8.4 s)
      retrain: data again 5.6 s later;  after our retrain              28800 / 28800   115200 bits each way, 0 err
      retrain: data again 5.6 s later;  after the caller's retrain     28800 / 28800   0 err
      retrain: data again 8.8 s later;  caller lost in Phase 3         28800 / 28800   0 err
      after two renegotiations       A->C 28800, C->A 16800 (ceiling set by us), 0 err; then cleardown both ways

  "data at" is the time from the start of ANSam. "caller asks for
  everything": INFO1c with pre-emphasis filter 6, 2 dB power reduction and
  the low carrier; J asking 16 points; MP asking 64 states, the non-linear
  encoder, expanded shaping and a precoder; the test checks each reached our
  transmitter. "strong echo, own delay unknown": hybrid 0.8, 12 ms of driver
  delay not told to the modem (`io_delay = -1`). "long loop": -13 dB at
  3 kHz against low frequencies. Also asserted: V.8 selection for a V.34-only
  and for a V.90 caller, S 70 +-5 ms after INFO1a, failures (no INFO0c: ends
  with `V34A_ERR_PHASE2`; caller gone during training: retrains, then ends).

## 3. CPU cost

Method: `make cost-v34`. A simulated call is recorded on the host
(`tools/rec_v34`), replayed through the answering modem on ARM under
`qemu -one-insn-per-tb -d exec,nochain`, and the executed instructions are
counted for two lengths of the recording, both in data mode; the difference
is one second of data mode. The V.90 recording of `make bench-v90` is
counted the same way. V.34 here: 3200 symbols/s, 28800 bit/s both ways,
16-state code both ways, mu-law, near canceller only.

    V.34 data mode: 41116270 ARM instructions per second of line (second 10 to 11 of the call)
    V.90 data mode: 39497113 ARM instructions per second of line (second 10 to 11 of the call)
    ratio V.34 / V.90: 104 %

V.90 data mode costs 32 % of the CPU on the real box (the project's
figure; the timed replay in PROGRESS.md shows 28.8 % for the data-mode
blocks). By instruction count V.34 data mode is 104 % of V.90's, so
about 33 % of the CPU, if instructions cost the same in both (both are dominated by the same kernels: the T/3 equaliser,
the 96-tap canceller, the Viterbi decoder; V.34 adds the modulator, about
45 multiply-adds a sample). This is an estimate from counts, not a
measurement on the target. Not in the figure: a far echo canceller (96
more taps a sample when the round trip exceeds 7 ms), a 32 or 64-state code
if the caller asks for one in OUR transmitter costs nothing (encoder only);
our receiver always asks for 16 states.

## 4. Not done, not verified

* **No real modem, no independent implementation.** The calling peer is
  `modem/v34dx` in its call role plus `sim/p2c`, a rearrangement of
  `modem/p2d`. A misreading of the Recommendation that is the same in both
  roles passes every test here. The contents of MP, J, J', INFO, the
  pre-emphasis templates and the non-linear encoder are checked against
  values taken from the tables and formulas, the procedures only against
  the peer.
* **TDD**: every test was written before the code it tests. `test_v34mp`
  was first run against a stub (132 of 146 checks failed). The others were
  first run against the finished first draft, not against a stub, and what
  they caught is what was fixed afterwards (L1 one sample short, the
  carrier table of the modulator at 2400, the far canceller not converging,
  the Phase 4 time limit restarting per state, the S detector on tilted
  loops, the Phase 2 projection blind to codec noise).
* **Symbol rates 2743, 2800, 3429**: not implemented, declined in INFO0a.
  3429 would take: one more pulse table (a/b = 3/7), pre-emphasis tables,
  the receiver core running above its input rate (T/3 is 10287 Hz), and a
  band up to 3.8 kHz, which a G.711 path does not carry; not worth it here.
* **Different symbol rates in the two directions**, auxiliary channel, MD
  of our own, half duplex, the control channel of Annex A, CME: no. A
  caller's MD is waited out (`md_len`), which no test exercises.
* **Recovery by INFOMARKS** (11.3.2, 11.4.2): not implemented. Every such
  time-out makes us begin a retrain (11.5.2.1), which the same clauses
  allow. The J' limit (100 ms + RTD) is not acted on by itself; the Phase 4
  limit covers it.
* **What we ask of the caller's transmitter in INFO1a is minimal**: no
  pre-emphasis, no power reduction, never a precoder. On a tilted loop a
  real V.34 pair would do better than the 12000 bit/s of the "long loop"
  run. Choosing a pre-emphasis filter from the L1 / L2 levels is the first
  thing to add.
* **Phase 2 cannot see noise that follows the signal** (codec
  quantisation): `modem/probe` counts what repeats from block to block as
  tone. p2ans therefore assumes a floor 36 dB below the received signal
  (`P2N_FLOOR_DB10`). It is an assumption, not a measurement; the final rate
  is set in Phase 4 from the receiver's error and does not depend on it,
  the choice of the symbol rate does.
* **2400 symbols/s**: works, but the echo canceller learns less from the
  narrow TRN (echo down 30 dB against 57 dB at 3200 in `test_v34dx`), so
  with a strong echo the last rate step may be missing.
* **Far echo canceller**: placed by the Phase 2 round-trip delay, 96 taps,
  no frequency-offset (phase-roll) compensation. Tested with a far echo 55
  and 80 ms late without offset. A far echo that has gone through a carrier
  system with offset will not cancel.
* **Echo cancellers are frozen after training.** A change of the echo path
  in data mode shows as a worse receiver error, then a rate renegotiation
  of our own, then a retrain.
* **Our own rate renegotiation only goes down** (receiver error above what
  the rate needs for a second). Nothing raises the rate again by itself;
  `v34ans_renegotiate()` exists for the owner to do it.
* **Loss of the caller's signal in data mode ends the call**
  (`V34A_ERR_CARRIER`, after about 0.5 s) instead of retraining, as p34d does.
* **V.90 Phase 2 followed by V.34** (V.90 9.2.1.1.8): the call-role engine
  and the configuration from INFO1a exist and are tested together
  (`v34_call_cfg_from_v90`, last block of `test_v34dx`), but nothing runs
  `modem/p2d` into it, and `answer.c` fails that call today. See 6.7.
* **Lines in the tests** are delays, short FIR "loops", white noise and a
  G.711 codec. No measured loop, no impulse noise, no non-linear distortion,
  no codec at the caller's end, no robbed-bit signalling.
* **CPU cost** is an instruction-count ratio under qemu, not a timing on
  the box.

## 5. Spec readings chosen

1. **Pre-emphasis templates (5.4, Figures 1 and 2, Tables 3 and 4).**
   Filters 1 to 5: gain in dB = alpha x f/S, alpha = 2, 4, 6, 8, 10 dB
   (straight line, 0 dB at 0, alpha at f = S). Filters 6 to 10: 0 dB up to
   f/S = 0.4, beta at 0.8, beta + gamma at 1.2 (beta = 0.5..2.5, gamma =
   1..5 dB), straight lines between; the figure does not label the stretch
   from 0.4 to 0.8. Total power is kept at nominal whatever the filter.
2. **Pulse shape**: not specified; root raised cosine with 10 % excess
   band, so that the spectrum is flat over carrier +-0.45 S as 5.4 measures it.
3. **Non-linear encoder (9.7)**: "the average energy of x(n)" is the
   nominal mean energy of the data constellation; the transmitter scales the
   encoded signal back to the nominal level (`energy_tx_q8`), as a real modem
   does (found with a Conexant CX93001, see PROGRESS.md).
4. **INFO0a**: bits 12:14 = 0 (no 2743, 2800, 3429), 15:18 = 1 (both
   carriers at 3000 and 3200), 19 = 0, 20 = 1 (we can reduce power), 21:23
   = 0 (one symbol rate), 24 = 0 (no CME), 25 = 1 (1664 points).
5. **INFO1a**: one symbol rate for both directions, the one with the
   highest sum of the two projected rates (ours from the probing, the
   caller's from INFO1c), on a tie the one with more in hand; the high
   carrier only when it is 1 dB better; projected rate = 3 dB per bit per
   symbol above 9 dB; rates above 28800 only if the caller's INFO0c bit 25
   is set. The power reduction applied to our transmitter is INFO1c bits
   12:14; the "additional power reduction" field is not used.
6. **Phase 2 recovery**: 11.2.2.2.1 to .3 return to Tone A and the
   reversal (up to four times); 11.2.2.2.4 (no INFO1c in 2000 ms + 2 RTD)
   starts a retrain of our own, up to two.
7. **70 ms of silence before S (11.3.1.2.1)** is counted from the end of
   the INFO1a carrier to the start of the first S pulse.
8. **J and J'** are sent in whole 16-bit repetitions: the answer modem
   finishes the current J before going silent, the call modem before J'.
9. **TRN of Phase 4** starts with scrambler and differential encoder at
   zero (10.1.3.8). Our receiver reads it through the differential decoder
   and does not use it as a known sequence, so the first MP after it is
   normally lost and the second is the first one read.
10. **An MP is believed when two in a row agree** (the acknowledge bit
    aside): what TRN looks like through the decoder passes the 16-bit CRC
    now and then. E is 20 ones after an accepted MP.
11. **Phase 4 limit**: 2500 ms + 3 RTD from the start of Phase 4 until both
    directions carry data, in either role (the Recommendation gives the
    call modem 2 RTD and counts to E).
12. **Rate renegotiation (11.6)**: S 128 T, S-bar 16 T, TRN for 100 ms, MP,
    all with four points; the responder starts its S on the S-to-S-bar
    transition, or 300 ms after S if it missed it; 6 s without data again
    means retrain.
13. **Cleardown (11.7)**: a rate renegotiation whose MP has both rate
    fields zero. The initiator stops when it has sent MP' and received MP',
    or after six MPs; the responder answers with a zero MP' and stops.
14. **MP of ours**: Type 0 (Type 1 only if a precoder is configured, which
    the answer modem never does), 16 states, no non-linear encoder, minimum
    shaping, bit 50 = 1 (asymmetric allowed), every rate of Table 8 for the
    symbol rate enabled; maximum receive rate from the receiver's error at
    the end of our Phase 4 TRN (9 dB + 3 dB per bit per symbol).
15. **Retrain (11.5.2)**: initiating and responding are the same for us:
    70 ms of silence, Tone A, and p2ans goes on at 11.2.1.2.3 with the
    INFO0c of the first Phase 2. The far modem's tone counts as detected
    after 60 ms holding more than half of the received power.

## 6. Integration into modem/answer.c

1. **V.8 menu.** Offer V.34 as a data mode: `local.modes |= V8_MOD_V34`
   and `local.pcm = V8_PCM_DIGITAL` without `V8_PCM_NO_V34_DATA` (that flag
   is what withdraws V.34 from JM today when PCM is not selected).
2. **What the two callers look like.** V.90-capable caller: CM carries the
   PCM category with the analogue-modem bit and V.34; JM answers with PCM
   digital; `v8.selected == V8_SEL_V90`; nothing changes. V.34-only caller:
   CM has V.34 and no PCM category (or no analogue bit); JM keeps V.34 and
   has no PCM; `v8.selected == 1`. Both cases are asserted in `test_v34call`.
3. **Hand-over.** When `v8ans_tick()` returns `V8A_DONE` (its 75 ms of
   silence after JM / CJ are over) and `selected == 1`: call
   `v34ans_init(&a->v34, &cfg)` and from the next sample on
   `tx = v34ans_tick(&a->v34, rx)`. No extra delay: INFO0a must start 75
   +-5 ms after JM ended (11.2.1.2.1). `rig/v34glue.h` is this in 15 lines.
4. **Configuration** (`v34ans_cfg`): `level` (RMS, as `answer_cfg.level`);
   `law` = 1 for mu-law, 2 for A-law when the samples pass a G.711 codec
   (the modem then only emits codec values, `pcm_encode()` of them loses
   nothing, and the echo reference is exact), 0 only for a truly linear
   path; `io_delay` = samples from handing out a sample until its echo
   starts to return, or -1 when the driver's delay is not known (found
   during training, up to 160 ms); `max_rx_rate`, `max_tx_rate` in bit/s or
   0; `getbit` / `putbit` / `user`.
5. **Per sample**: one linear 16-bit sample in, one out. With the octet
   interface: `rx = v90_octet_linear(alaw, octet_in)`, `octet_out =
   pcm_encode(alaw, tx)`.
6. **What it reports.**
   * `state == V34A_DATA`: connected. `v34ans_tx_rate()` is answer to call
     (what `answer_t.bitrate` holds for V.90), `v34ans_rx_rate()` call to
     answer. `connects` counts how often data mode was reached.
   * `V34A_RENEG`: data interrupted by a rate renegotiation (about 0.4 s);
     the rates may differ afterwards. `dx.n_reneg`, `dx.rate_drops` count.
   * `V34A_RETRAIN`, then `V34A_PHASE2` / `V34A_TRAINING` after a
     connection: retraining (about 5.6 s). `retrains`, `retrains_ours`.
   * `V34A_ENDED` with `error`: `V34A_ERR_CLEARDOWN` (the caller cleared
     down), `V34A_ERR_HANGUP` (we did), `V34A_ERR_CARRIER`,
     `V34A_ERR_PHASE2` (`p2.error` says why), `V34A_ERR_TRAINING`
     (`dx.error`), `V34A_ERR_RETRAINS` (more than four in a row).
   * `getbit` is only called for data; `putbit` only gets data (B1 is
     checked and dropped). Bits in flight when a renegotiation or retrain
     starts are lost; the error-correcting layer above must cope.
   * To hang up: `v34ans_cleardown()`, then wait for `V34A_ENDED` (well
     under a second). `v34ans_retrain()` and `v34ans_renegotiate(max_rx)`
     are there for the owner's own policy.
7. **V.90 Phase 2 has started but the caller wants V.34.** It can happen:
   V.90 9.2.1.1.8 says that when INFO1a bits 37:39 are 0..5 the digital
   modem "shall proceed in accordance with 11.3.1.1/V.34 assuming the role
   of a call modem" and that later retrains use Phase 2 of V.90.
   `info1a_parse()` reports it as `info1a.v90 == 0`, and `start_p3()` in
   answer.c fails the call on that today. To support it: on `P2D_DONE` with
   `info1a.v90 == 0` call `v34_call_cfg_from_v90(&p2.info1a,
   (p2cfg.up_hi_carrier >> info1a.up_sr) & 1, p2.info0a.c1664, p2.rtd,
   &cfg, &dxcfg)`, then `v34dx_init()` and `v34dx_tick()` per sample (the
   engine is silent at first and waits for the caller's S). On
   `DX_RETRAIN`: 70 ms of silence, `p2d_init_retrain()`, and decide again
   from the new INFO1a. Limits: the helper refuses different symbol rates
   in the two directions and anything but 2400 / 3000 / 3200; what p2d
   puts into INFO1d (no pre-emphasis, rates capped at 28800) was not
   reviewed for this use; none of this was run end to end.
8. **Memory**: `sizeof(v34ans_t)` is 27368 bytes on the host (p2ans 4256,
   v34dx 23008: two cancellers, an 8 kB delay line, the Viterbi history);
   static state only, no allocation.
