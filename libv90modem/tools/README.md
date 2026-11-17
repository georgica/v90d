# tools

| | |
|---|---|
| `v90m_replay.c` (`make replay`, installed as `v90m-replay`) | plays a recorded call into the answering modem and reports what it made of it |
| `trace.c`, `trace.h` | the replay's report: the trace hooks of the modem (built with `-DP34D_TRACE`) and the lines printed per phase; v90d's server replay prints the same |
| `rec_v90.c`, `rec_v34.c` | record a simulated call (`make bench-v90`, `make bench-v34`) |
| `ber_v90.c` | bit error rate of V.90 over the simulated line |
| `capinfo.py`, `spec.py`, `hdlc.py` | a recording's level per 200 ms; its strongest spectral lines; the HDLC frames in a bit dump |
| `gen_*.py` | generators of the constant tables in `modem/` |

## The call recording format

A recording holds, for every 10 ms (80 samples) of a call, what the codec
received from the line and what it sent, in that order:

- **companded**: 80 octets heard, then 80 octets sent: G.711 codewords
  (mu-law or A-law, as the line was). 160 octets per block.
- **linear receive**: the file starts with the 8 octets `V90DLIN1`; then per
  block 80 heard samples as little-endian signed 16-bit values on the scale
  of V.90 Table 1 (what `v90m_g711_decode` gives for a codeword), then 80
  codewords sent. 240 octets per block.

The recording starts before the call is answered (v90d records from
off-hook); the replay finds the answer tone in what was sent (the first
sound after 200 ms of silence) and starts the modem so that its own answer
tone begins at that moment.

For the replay to follow the call, the modem must be set up as the recording
modem was (level, modulations offered, rate ceilings, V.92): what it hears
contains the echo of what that modem sent. The replay's last line counts the
samples where its transmit differs from the recording's; 0 until data mode
(where the data were the recording modem's own) means it was set up right.
