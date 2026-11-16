# v90d/v42: V.42 error control, V.42 bis and V.44 data compression

The layer between the modem's data mode bit stream and PPP: detection phase,
LAPM over HDLC framing, data compression, and the start-stop fallback for
callers without error control. `v42.h` describes the endpoint (`v42ec_*`)
and how it is driven; this file is about the compression half, V.44 above
all, and lists the readings taken where the Recommendations leave a choice.

Written from the ITU-T texts only (`../refs/itu/`: V.42 03/2002, V.42 bis
01/90, V.44 11/2000). No other modem or compression implementation was
opened or consulted.

Portable C99, integer only, no allocation, no OS calls; every object is a
caller-owned struct with arrays sized by compile-time maxima.

| file | what |
|---|---|
| `v42_hdlc.c`, `v42_detect.c` | HDLC framing, ODP/ADP detection, start-stop |
| `v42_lapm.c` | LAPM (V.42 clause 8) and the XID negotiation, compression included |
| `v42_comp.c` | V.42 bis |
| `v44_comp.c` | V.44, stream method (clauses 6 and 7) |
| `v42.c` | the endpoint: all of the above behind `v42ec_write()` / `v42ec_read()` |

## Build and test

    make test        host tests
    make test-arm    the same tests cross-built for the ARM926, under qemu-arm-static
    make clean

Last run (x86-64 and ARM, identical check counts; the V.44 and LAPM tests
also under clang with the address and undefined-behaviour sanitizers):

    test_hdlc: 989 checks, 0 failures
    test_detect: 1103 checks, 0 failures
    test_comp: 761 checks, 0 failures
    test_v44: 149078 checks, 0 failures
    test_lapm: 13964 checks, 0 failures
    test_v42: 5203 checks, 0 failures

## V.44

`v44_comp.c` holds one encoder and one decoder type; a connection uses one
of each, with separate parameters per direction (N2 codewords, N7 maximum
string length, N8 history length; Table 11).

Implemented: the encoder dictionary as root array, node-tree and history
(6.2.1) and the decoder's string collection and history (6.2.2); string
matching and string extension (6.3, 6.4); ordinals, codewords, control codes
and string-extension lengths with their prefixes, packed as in 6.6; STEPUP
for both the codeword and the ordinal size (7.11.1, 7.11.2); transparent
mode with ETM, ECM, EID and the cycling ESCAPE (6.5, 7.14); FLUSH (7.13);
dictionary reinitialization with REINIT (7.12); the data compressibility
test (7.11.5); C-ERROR (7.15).

Not implemented: the packet and multi-packet methods of Annex B (they are
for packet networks, not for V.42) and the negotiation of parameters after
link establishment (7.4.2, the EPM command).

Maxima: 2048 codewords, strings of 255, history 4096
(`V44COMP_MAX_CODEWORDS`, `V44COMP_MAX_HISTORY`). With them an encoder is
19152 octets and a decoder 10336 (19116 and 10320 on ARM). In `v42ec_t` the
V.44 and V.42 bis objects share their storage, as only one procedure runs on
a connection: the endpoint grew from about 45 kB to 49752 octets.

Cost, counted under qemu on the ARM build (2048 / 255 / 4096, HTML and
English text): about 200 instructions per character in the encoder and 75
to 85 in the decoder. At 15000 characters a second that is 3 million
instructions a second, on the order of one percent of the 400 MHz ARM926.

### The tests (`test/test_v44.c`)

- Table 6 of 6.6.4 and the worked examples of Appendix II.1 and II.2, bit
  for bit.
- Hand-derived vectors for the matching rules, STEPUP, reinitialization,
  history-full, transparent mode and the ESCAPE cycle.
- Round trips of text, HTML, random, binary, runs, all 256 values and a mix,
  from one character up, through every parameter corner (256, 512, 1024,
  2048 codewords; strings 32 and 255; history 512 to 4096), with a flush
  every 1 to 200 characters, output buffers of one octet, and forced mode
  switches.
- The decoder on garbage: it stops with a C-ERROR or carries on, and never
  writes outside its object.
- Figures: the table below. The two files are optional local samples; the
  tests do not depend on them.

`test_lapm.c` has the XID side (the recorded XID of a Conexant CX93001 among
the cases), `test_v42.c` runs two endpoints against each other with V.44 in
both directions through bit errors and bursts, flow control, breaks and the
interactive case.

### Compression ratios

300000 octets of each kind, the encoder left to its compressibility test.
V.42 bis: codewords / string length. V.44: codewords / string / history.

    data                 | V.42bis 512/32 2048/32 | V.44 512/32/1024 1024/255/3072 2048/32/4096 2048/255/4096
    text (generated)     |   2.04    2.89 |   2.05    2.40    2.48    2.48
    html (generated)     |   1.66    2.92 |   1.93    2.51    2.66    2.67
    random (generated)   |   0.99    0.99 |   0.99    0.99    0.99    0.99
    binary (generated)   |   2.43    2.21 |   2.24    2.36    2.38    2.38
    runs (generated)     |   8.94    9.23 |  20.04   39.22   21.56   39.60
    all256 (generated)   |   0.98    1.49 |   0.98    0.98    0.98    0.98
    mixed (generated)    |   1.54    1.60 |   1.49    1.55    1.59    1.60
    html.bin (file)      |   1.71    2.34 |   2.00    2.41    2.47    2.49
    text.bin (file)      |   1.82    2.41 |   1.98    2.33    2.41    2.41

What limits V.44 here is the history: the dictionary is reinitialized every
N8 characters of input (7.12), so with 4096 at most it never learns more
than one screenful. Against a caller that proposes 512 / 32 / 1024 (the
CX93001's defaults) the gain over V.42 bis with 512 codewords is about 17 %
on HTML (2.00 against 1.71); with the caller raised to 2048 / 255 / 4096 it
is 2.49. V.42 bis with 2048 codewords is about as good, where a caller
offers it.

## Negotiation (V.42 12.2, V.44 7.3, 7.4, Annex A)

The V.44 parameter set travels in the user data subfield of the XID frame
(group identifier 0xFF, no length, to the end of the frame): set identifier
0x40 "V44", C0 0x41, P0 0x42, P1T / P1R 0x43 / 0x44, P2T / P2R 0x45 / 0x46,
P3T / P3R 0x47 / 0x48. The V.42 bis set stays where it was, in the private
parameter subfield.

- As initiator both sets are sent, V.42 bis first.
- As responder, if the command has a V.44 set, V.44 is enabled here and at
  least one direction can be agreed, V.44 is taken: the answer carries the
  V.44 set and no V.42 bis set. Otherwise V.42 bis is handled as before.
- Each value in force is the lesser of the two ends' values, per direction.
- `v42lapm_cfg_t` has `v44_tx`, `v44_rx` and `v44_p1_*`, `v44_p2_*`,
  `v44_p3_*` per direction. `v42lapm_cfg_default()` leaves V.44 off, like
  V.42 bis; `v42ec_cfg_default()` turns both on, V.44 with 2048 / 255 / 4096.
- `v42ec_status_t.comp_alg` says which procedure is in use; `comp_tx` and
  `comp_rx` mean "compression in this direction" for either.
- The server's `v42bis = off` setting switches off both procedures.

## Readings and choices

Where the text leaves something open, is unclear, or had to be completed.

**V.44 encoder and decoder**

1. Table 2 NOTE, a string that would exceed N7. Nothing is created and no
   codeword number is used up, at both ends.
2. 7.11.3 against Appendix I.1, the full node-tree. 7.11.3 speaks of the
   encoder being "unable to create" a node; the appendix reinitializes when
   the last codeword has been assigned. Here the encoder reinitializes (and
   sends REINIT) as soon as codeword N2 - 1 has been created. Characters
   taken but not yet coded are moved to the start of the new history. A
   decoder whose collection is full creates no more strings and reports
   nothing: the encoder's REINIT is what resets it.
3. 6.3.1, several string-segments at one level that agree with the input.
   The longest one that matches completely is taken. Where only part of a
   longer segment matches and none matches completely, the match ends at
   the node above and the rest goes into a string-extension length (6.3.1
   NOTE 1).
4. 7.13, the character after a flush. It is always appended as a new node
   to what was matched before the flush, even where that makes a second
   segment with the same content, because the decoder creates a string
   there in any case (Table 2).
5. 7.13, when FLUSH is sent. Always when anything was coded since the last
   flush, also when the code stream happens to end on an octet boundary;
   never when nothing was coded.
6. 7.11.1 / 7.11.2, which size a STEPUP raises. The text ties it to the code
   that follows; the decoder looks at the next prefix bit: 0, an ordinal
   follows and C5 goes up; 1, a codeword follows and C2 goes up.
7. 7.15, C-ERROR. Beyond the three listed conditions the decoder also
   reports: a codeword equal to C1 with no previous code to build it from, a
   string-extension length that takes the string past N7, more characters
   than the history holds without a REINIT, and the EPM or a reserved
   command in transparent mode.
8. 7.11.5, the compressibility test, is left to the implementer. Here: in
   compressed mode the code bits of each 512 characters are compared with
   8 bits a character, and more means transparent mode; in transparent mode
   each 512 characters are coded on the side with a fresh dictionary, and
   less than 7/8 of the raw size means compressed mode. `v44comp_enc_policy()`
   can pin either mode. The initial mode is compressed (7.5).
9. 7.5 lists link establishment as the occasion for C-INIT and says nothing
   of breaks. A destructive break (V.42 8.13) discards data in transit, so
   it also runs C-INIT on both functions here, as V.42 bis 5.6 has it.

**V.44 in XID**

10. 7.4.2, C0. Negotiation after link establishment is not implemented. Our
    sets carry C0 = 0. A command with N = 1 is accepted for V.44 with the
    values of that XID and answered with N = 0 (V.42 Table 11c NOTE 3 calls
    the bit a capability).
11. Parameters left out of a V.44 set stand for the defaults of Table 10:
    P0 both directions, P1 1024, P2 255, P3 three times P1 (the P1 of the
    same set and direction, before the minimum is taken).
12. P0, which bit is which direction. V.44 7.4 and Table 10 make bit 1 the
    sender's transmit direction and say the best answer to 01 is 10; V.42
    Table 11c NOTE 4 reads as initiator-relative. The V.44 reading is used:
    T and R are always the sender's own. For 11 and 00 the two agree.
13. Set identifier. "V44" is sent (V.44 Table A.1); 0x2A '4' '4' is taken
    as well (V.42 Table 11c NOTE 2).
14. 7.3 NOTE, "at most one" procedure. V.44 is preferred. The answer that
    takes V.44 leaves the V.42 bis set out altogether (rather than sending
    it with P0 = 0). A response that grants both is a negotiation failure.
    V.44 agreed in one direction only still excludes V.42 bis in the other.
15. A command with a V.44 set and no V.42 bis set, to an end with V.44
    switched off: the answer has no user data subfield (rather than a V.44
    set with P0 = 0).
16. 7.4, illegal values. P1 below 256, P2 outside 32 to 255 or P3 below 512
    in a command or a response ends the connection attempt
    (`V42LAPM_R_NEGOTIATION`), as V.42 bis values out of range already did.
    A response that grants a direction that was not proposed does the same.
17. The user data subfield is parsed as PI, PL, PV triplets to the end of
    the frame. Parameters before the V.44 set identifier or under another
    identifier (the manufacturer ID of V.42 Table 11c) are passed over; a
    subfield cut short is used as far as it is whole.
18. An XID command on an established connection changes nothing and is
    answered with the values in force, as before.

**The endpoint**

19. C-ERROR. V.42 bis 5.8 asks for "appropriate recovery action, including
    re-establishment"; V.44 7.15 likewise leaves it to the control function.
    Here the connection is released (`V42EC_R_COMPRESSION`) for either.

## Not verified

V.44 has not been run against a real modem yet; the XID test vector is a
recording, everything after it is this implementation against itself and
against the worked example of the Recommendation. Readings 2, 4, 5, 6 and 12
are the ones a real peer could contradict.
