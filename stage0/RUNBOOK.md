# Stage 0 runbook: what gets run on the HT802, in order

Nothing here writes to flash. Binaries go to /tmp (RAM) and vanish on reboot.
Settings changes are P-values through the web UI or `nvram set`; note the old
values first so they can be put back.

`BOX` = the HT802's IP. Root: `ssh admin@BOX`, type `gssu`, answer the challenge
with `python3 ~/v90d/refs/ht802/tools/SuperUser/GSSU.py ADMIN_PASSWORD CHALLENGE`.

## 0a facts and hello

```
uname -a; cat /proc/cpuinfo; cat /proc/meminfo | head -5; free
cat /proc/mtd; df -h; nvram get 68 2>/dev/null   # version string location to be confirmed
ps; top -b -n 1 | head -15                        # gs_ata CPU at idle
```
Copy binaries (the box has no scp server; use netcat or tftp/wget from the PC):
```
PC:   cd ~/v90d/stage0 && python3 -m http.server 8000
box:  cd /tmp && for f in hello.arm bench.arm sipecho.arm pcmpat.arm; do wget http://PC:8000/$f; done; chmod +x *.arm
box:  ./hello.arm
```

## 0d CPU benchmark (run before any call, then again during a call)

```
./bench.arm; ./bench.arm; ./bench.arm
```

## 0b loopback

Settings for FXS port 1 (P-values read from the 1.0.10.6 web pages; verify on the unit):

| Setting | P | Value for the test |
|---|---|---|
| Primary SIP Server | 47 | 127.0.0.1:5062 |
| SIP Registration | 31 | 0 (No) |
| Outgoing Call without Registration | 109 | 1 |
| Offhook Auto-Dial / delay | 71 / 4045 | 100 / 0 |
| Preferred vocoder 1..8 | 57,58,59,60,61,62,46,98 | all 0 (PCMU) |
| Voice Frames per TX | 37 | 2 (20 ms) |
| VAD | 50 | 0 |
| Disable LEC | 824 | 1 |
| Disable Network Echo Suppressor | 4441 | 1 |
| Fax Mode | 228 | 1 (pass-through) |
| Re-INVITE After Fax Tone | 4416 | 0 |
| Jitter Buffer Type / Length | 133 / 132 | 0 (fixed) / 0 (low) |
| TX gain / RX gain | 247 / 249 | 0 / 0 (0 dB; RX default is -6 dB) |
| SRTP | 183 | 0 |

Port 2 keeps SIP port 5062 by default in some profiles. If `./sipecho.arm`
fails to bind, use 5070 in both places.

```
box:  ./sipecho.arm -p 5062 -r 40000 -m echo -l 1000 | tee /tmp/0b.log
```
Take port 1 off-hook (phone or the USR with ATH1). Leave it 5 minutes. Pass
criteria: every 1 s line shows 50.0/s, lost=0, ts_jump=0, badpt=0, badlen=0, and
the histogram has everything in the 19-21 ms bucket (a few in the neighbours is
tolerable; anything >=40 ms is not). The log also prints the firmware's full
INVITE, which records its codec list, ptime and any fax/modem attributes.
Repeat once with each of LEC, jitter buffer and gain at factory values to see
which settings actually change the stream.

Voice DSP parameters as the firmware set them, during the call:
```
box:  /app/bin/audiotune -h; /app/bin/audiotune -n -o /tmp/dsp_params.txt   # option meaning to be confirmed with -h first
```

## 0c transparency

```
box:  ./pcmpat.arm /tmp/pattern.ulaw
box:  ./sipecho.arm -p 5062 -r 40000 -f /tmp/pattern.ulaw -w /tmp/rx.ulaw | tee /tmp/0c.log
PC:   arecord -D <device> -f S16_LE -r 48000 -c 1 capture.wav     # start first, then go off-hook
PC:   ~/v90d/stage0/pcman capture.wav
```
The pattern starts with the first RTP frame of the call and lasts 39.2 s.
Capture needs a DC-blocking, isolating coupler between the line and the sound
card input, and the port needs a DC load to stay off-hook. Set the input gain so
`pcman` reports a peak between -12 and -3 dBFS and "capture quality good".
Run three times; slips are intermittent by nature.

### 0c without a coupler: echo capture (current plan)

No sound card. A phone holds port 1 off-hook; with the firmware echo canceller
off (P824=1, P4441=1), part of the pattern reflects at the 2-wire side and
returns in upstream RTP, which `sipecho -w` already records.

```
box:  ./sipecho.arm -p 5062 -r 40000 -f /tmp/pattern.ulaw -w /tmp/rx.ulaw | tee /tmp/0c.log
      (lift the handset, keep the room silent or cover the mouthpiece, wait 45 s, hang up)
PC:   fetch /tmp/rx.ulaw, then  ~/v90d/stage0/pcman rx.ulaw
```
What it can show: slips, dropped/repeated/concealed samples, gain stages and
level remapping (in either direction; it cannot say which). What it cannot
show: A-law style changes to the lowest codes, or level errors under about
0.15 dB, because the return path is itself mu-law quantised (fit SNR tops out
near 37 dB). "INCONCLUSIVE" means room noise or a weak echo; repeat quieter.
If the clock offset is not 0.0 ppm, something in the path resamples.

Then the ANSam question: repeat with a pattern file that begins with 3 s of
2100 Hz with phase reversals (to be generated) and check in the SIP log whether
gs_ata reacts (re-INVITE, codec change) and whether `pcman` results change.

## Go / no-go inputs

| Result | Meaning |
|---|---|
| 0b clean, 0c "bit-exact, slip-free" | go for V.90 |
| 0c levels BAD (scaled) but timing OK | V.90 at reduced rate at best; find the gain stage first |
| 0c or 0b shows slips or concealment that settings cannot remove | no-go for V.90 and for V.34; path is not usable for any modem |
| 0b clean, 0c altered only in levels/filtering | V.34 fallback is viable |
| bench load estimate above about 50 % during a call | no-go on CPU, or reduce receiver size |
