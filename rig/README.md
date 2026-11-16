# Real-modem test rig (stock firmware SIP/RTP path)

Purpose: let a hardware modem on FXS port 1 call the v90d answering modem
*before* our own SLIC driver exists. The stock firmware keeps the line; its
port-1 SIP account is pointed at `sipmodem` running on the box itself, which
feeds the RTP audio (G.711 mu-law, 20 ms frames) through `modem/answer.c`.
This is a test rig only. Nothing here goes into the shipped image.

What it does on a call: answers, runs V.8 (ANSam, CM/JM/CJ), then V.22bis /
V.22 / V.21 (or the pre-V.8 answer sequence), sends
`v90d CONNECT <rate> <modulation>` and echoes every character back. It logs
`MODEM CONNECT ...`, each received line (`MODEM rx: ...`), and RTP timing.

## Build and load (nothing is written to flash; /tmp is RAM)

    make rig                                   # build/sipmodem.arm (static, ~137 KB)
    box:  cd /tmp && nc -l -p 9001 > sipmodem.arm      # root shell: telnet BOX 2323
    PC:   nc -w 5 BOX 9001 < build/sipmodem.arm
    box:  chmod +x sipmodem.arm && ./sipmodem.arm -p 5070 -r 40000

Verified so far (2026-11-14): the binary runs on the box and binds its ports;
the modem code itself was timed on the box at 2.1 % of the CPU (V.22bis) and
2.0 % (V.8). NOT yet done: an actual call from a hardware modem (needs someone
at the modem, and the port-1 settings below).

## Port 1 settings (note the old values first, to put them back)

Same table as `stage0/RUNBOOK.md` 0b: Primary SIP Server `127.0.0.1:5070`,
no registration, outgoing calls without registration, off-hook auto-dial,
PCMU only, 20 ms frames, VAD off, line echo canceller off, network echo
suppressor off, fax mode pass-through, no re-INVITE on fax tone, fixed low
jitter buffer, TX/RX gain 0 dB, SRTP off.

## The test

1. Start `sipmodem.arm` on the box, leave the telnet session open to watch the log.
2. On the PC with the hardware modem: open the serial port (e.g. `minicom`),
   `ATZ`, then `ATDT1` (any digit string; off-hook auto-dial sends the INVITE).
   To force a mode: `AT+MS=V22B` (USR: `ATS27=...`/`AT&N3` for 2400) or
   `AT&N1` for 300 bit/s; default lets V.8 choose.
3. Expected: modem reports `CONNECT 2400` (or 1200 / 300), the banner appears,
   typed characters echo. The box log shows `MODEM CONNECT 2400 (V.22bis)`.
4. Things worth recording if it fails: the box log (RTP gaps, `ts_jump`,
   `badpt`), the modem's `ATI6`/`ATI11` diagnostics, and whether the firmware
   re-INVITEd (T.38) when it heard ANSam.

Options: `-g <rms>` transmit level (default 3000, about -17.6 dBm0),
`-M v21|v22|all` modes offered, `-m echo|silence|file` the stage-0 tools,
`-w file` dump received audio (raw mu-law) for offline analysis.

## A whole dial-up call: `-m ppp`

`sipmodem -m ppp` puts the complete call path behind the same RTP stream:
answering modem, V.42 / V.42bis (or start-stop for a caller without error
control), and the PPP server. There is no network behind it; the rig itself
is the far end of the link at 10.64.64.1 and answers pings, which is enough to
see a dial-up connection come up and carry packets. The caller gets
10.64.64.2.

    box:  ./sipmodem.arm -p 5070 -r 40000 -m ppp                  # V.22bis / V.21
    box:  ./sipmodem.arm -p 5070 -r 40000 -m ppp -u alice:secret  # ask for a login (CHAP or PAP)
    box:  ./sipmodem.arm -p 5070 -r 40000 -m ppp -M v90           # offer V.90 as well (see below)

On the PC, dial with any PPP client instead of a terminal:

    Linux:    pppd /dev/ttyUSB0 115200 connect "chat -v '' ATZ OK ATDT1 CONNECT" \
                   noauth nodetach debug defaultroute        (add: user alice, and the secret in chap-secrets)
    Windows:  a dial-up connection on the modem, number 1, any user name if -u is not given

then `ping 10.64.64.1`. The rig logs one line per change, for example

    MODEM CONNECT V.22bis, 2400 bit/s to the caller, 2400 bit/s from it, 4.1 s after the answer
    MODEM online: V.42 LAPM + V.42bis, PPP network, IP up, user alice
    MODEM call over after 61.2 s: carrier lost; 12 pings answered, 3 other IP packets, LAPM 0 frames resent, 0 bad

This path is tested in simulation (`make -C srv test`, test_rig: 20 ms frames,
V.22bis and V.90 callers, 26 checks) and NOT yet against a hardware modem.

### V.90 through the stock firmware: an experiment, not a promise

`-M v90` hands the RTP payload octets to the V.90 modem as they are. That can
only work if the stock firmware (a) plays our octets to the SLIC unchanged,
with no gain, no packet loss concealment, no comfort noise and no sample
slipped or repeated by its jitter buffer, and (b) does the same in the other
direction. Port settings above (gains 0 dB, VAD off, echo canceller off, fixed
jitter buffer) aim at that, but whether the firmware really is transparent to
the octet is unknown; `-m echo` and the stage 0 file tools can show it. The
echo of our own signal comes back late on this path (packetisation and the
jitter buffer); the modem measures that delay in each call, up to 160 ms.
If V.90 fails here it says little about V.90 on our own firmware, where the
octets go straight to the codec; if it works, it says a lot. A caller that
gets no V.90 falls back to V.22bis on its own.

Worth recording from a V.90 attempt: the modem's `ATI6` and `ATI11` (USR:
rates, retrains, the constellation it chose), the rig log, and with
`-w rx.ulaw` the received audio.
