# ppp/: the PPP server

HDLC framing (`ppp_hdlc.c`), the option negotiation automaton (`ppp_fsm.c`),
LCP, PAP, CHAP-MD5, MS-CHAP v1 and v2, IPCP and CCP (`ppp.c`); the two
compression algorithms CCP can agree on, Deflate (`ppp_deflate.c`) and MPPC
(`ppp_mppc.c`); and encryption: MS-CHAP v1 or v2 with the MPPE cipher
(`ppp_crypto.c`, `ppp_mppe.c`). Portable C99, integer only, no allocation,
no OS calls.

This file is about the compression and encryption part: what was built, from
what, how it is bounded, and every place where the RFCs leave a choice. The
encryption readings are near the end, under "RFC 2759 / 3078 / 3079".

## Sources

Only these, kept in `refs/rfc/`: RFC 1962 (CCP), RFC 1979 (PPP Deflate),
RFC 1951 (the deflate format), RFC 2118 (MPPC), RFC 1994 (CHAP), RFC 2433
(MS-CHAP v1), RFC 2759 (MS-CHAPv2), RFC 3078 (MPPE) and RFC 3079 (deriving the MPPE keys), together
with RFC 1661 for the automaton. No other implementation was read. Python's `zlib` module is
used as a black box in `test/zlib_check.py`; a real pppd 2.5.1 is used as a
black box in `srv/test/ns_pppd.c`, and its packet log is the source of the
"observed" remarks below.

## What is negotiated

`ppp_config_t.comp` is a mask of `PPP_COMP_DEFLATE` and `PPP_COMP_MPPC`; 0
means CCP is not spoken at all and the protocol is rejected as before
(`[ppp] compression = off`). CCP starts when IPCP may start (after
authentication) and never holds IP up: IP flows uncompressed until CCP
opens, and again whenever it closes.

The two directions are separate (RFC 1962 section 4, "Configuration
Options"): the option we Ack governs what the caller sends us, the option
the caller Acks governs what we send.

- Our request names one algorithm: Deflate (option 26) with our largest
  window first. A Nak with a smaller window is taken. A Reject of 26 moves us
  to the draft number 24, a Reject of that to MPPC (option 18, bits
  0x00000001), a Reject of that to an empty request: CCP then opens with
  nothing sent compressed towards the caller, which RFC 1962 allows.
- In the caller's request we Ack the first option we can use in the order
  Deflate 26, Deflate 24, MPPC, and Reject the others in the same answer
  (pppd sends 26, 24 and BSD together and expects exactly that).
- Encryption changes option 18. With `cfg.encrypt` 0 (the default, and
  what the daemon uses since 0.14) encryption is given on request: a caller
  whose option 18 carries MPPE key bits (0x20 / 0x40 / 0x80), and whose
  login was MS-CHAP (v1 or v2, so the keys are in hand), gets MPPE, MPPC's
  bit asked as well or not: we do not do both at once, and Windows XP with
  "Require data encryption" and software compression on asks 0xe1,
  Windows 98 0x21 (or 0x31), and both hang up (error 742) on a Nak toward
  MPPC alone (0.14.0 did that, taking such a request for "optional").
  Windows' unticked "Require data encryption" asks MPPC only (0x01), with
  or without "Require encrypted password", and keeps compression. That
  request ranks above any compression in it. With the keys in hand CCP waits (a
  Restart period at most) for the caller's first request before sending
  ours, so ours is MPPE, in the mode and key length asked, when the caller
  wants it: pppd with require-mppe ends the link on a server whose first
  request is anything else. A caller that asks later still gets it: our
  request becomes MPPE (sent at once if CCP is still negotiating, or by
  negotiating CCP again if our compression had been Acked); and from then
  on nothing goes in the clear
  either way, as with encryption required. A caller that asks for no MPPE
  gets compression and the clear, as before; one that asks without the
  keys (another login) is Nak'd down to MPPC's 0x00000001. With
  `cfg.encrypt` 1 (required) `comp` is empty (no Deflate, no MPPC), we
  request and require
  128-bit MPPE (option 18, bit 0x00000040), or for a caller that has only
  shorter keys the strongest of 56-bit (0x80) and 40-bit (0x20) it has
  (Windows 9x/ME without the 128-bit upgrade), stateless (| 0x01000000) unless
  the caller wants stateful, as Windows does on a modem; and if MPPE is not
  agreed in both directions the link is closed rather than run in the clear. That path is detailed under "RFC 2759 / 3078 / 3079" below.

## Bounds

- RAM per port, on the ARM build, `ppp_t` complete: 193464 octets with the
  window limit at 15 (`PPP_DEFLATE_MAX_WBITS`). Of that the Deflate
  compressor is 146296, the Deflate decompressor 33608, the MPPC compressor
  32772 and the MPPC decompressor 8194 (the two algorithms are separate
  storage; a union would save about 41 kB and was not done, for clarity).
  The window limit is a compile-time constant:

  | `PPP_DEFLATE_MAX_WBITS` | compressor | decompressor | `ppp_t` | HTML ratio | text ratio |
  |---|---|---|---|---|---|
  | 15 | 146296 | 33608 | 193464 | 3.30 | 3.04 |
  | 14 | 97144 | 17224 | 127928 | 3.18 | 2.96 |
  | 13 | 72568 | 9032 | 95160 | 3.03 | 2.86 |
  | 12 | 60280 | 4936 | 82036 | 2.87 | 2.73 |

  Windows 11, 10, 9 and 8 (what a peer may force by Nak) give 2.68, 2.35,
  2.16, 1.95 on the HTML sample and 2.59, 2.30, 2.14, 1.97 on the text one;
  MPPC gives 2.42 and 2.33. Ratios are octets of the IP packets over octets
  of the PPP information fields sent, on the two 300000-octet sample files
  cut into 1460-octet segments behind 40-octet headers. The unit has about
  21 MB free, so 15 is the default.
- Time. `ppp_deflate_compress` examines at most `effort.work` candidate
  strings for one packet (default 10000, at most 24 per position); when the
  allowance is used up the rest of the packet is coded with the first
  candidate only. Measured with qemu-arm instruction counts: about 225
  instructions per input octet on HTML, 238 on text, 262 on random octets,
  and 374 (562000 for one 1500-octet packet) on the worst input found, long
  runs of near-repeats built to keep every hash chain full;
  the decompressor about 85 per output octet. MPPC: about 72 per octet each
  way. On top of that `ppp.c` gives the compressor a budget of
  `PPP_COMP_RATE` (40) octets per millisecond per port, refilled by
  `ppp_tick`; a packet that arrives with the budget used up goes out
  uncompressed. 40 kB/s at 225 instructions is 9 million instructions a
  second per port.

## Readings

Where an RFC is silent or can be read two ways, this is what was chosen.

RFC 1962 (CCP):

1. More than one option in a request. Section 4 lets a peer list several
   algorithms and leaves the pick to the receiver; it does not say how the
   pick is told. We Ack by rejecting all but one in a Configure-Reject, and
   Ack the request that then comes with one option. In our own request we
   list one algorithm at a time.
2. Reset-Request and Reset-Ack (section 3.6) are used for both algorithms.
   The identifier of the Reset-Ack copies the request. A Reset-Ack that
   answers nothing is ignored.
3. A Reset-Request that is not answered is repeated after the restart time,
   three times; after that CCP is taken down and negotiated again
   (the RFC only says the request "may be retransmitted").
4. Packets compressed before a reset are useless to the peer once it has
   asked for the reset. On a Reset-Request we take the compressed frames
   that are still waiting in our transmit buffer back out, and the Reset-Ack
   goes right behind the frame being sent. Likewise our own Reset-Request
   jumps the queue. Without this a V.22 line without error control spends
   half a minute sending frames that will be thrown away.
5. On lines without error control the time to wait for the Reset-Ack is set
   by the caller of the library (`comp_reset_ms`; `dce.c` adds the time one
   full-size frame takes each way), and `ppp_set_comp_queue` limits how much
   compressed data may wait in the transmit buffer, so a reset never has
   more than about a fifth of a second of stale frames in front of it.
6. Protocols compressed: IP only (0x0021). Section 2 forbids LCP and the
   control protocols; we also leave out everything else we do not carry.
7. A compressed packet (0x00FD) received while no decompressor is agreed
   is dropped silently when CCP is opened with an empty option list and
   answered with a Protocol-Reject when CCP is not spoken.
8. CCP going down (Terminate, renegotiation, a Configure-Request while
   opened) stops compression at once in both directions and leaves IPCP
   alone. The counters of the session survive it.
9. The compressed frame must fit the peer's MRU (section 2 "Padding" and
   RFC 1661): a packet whose compressed form with its header would be
   longer than its native form or than the MRU is sent in its native form.

RFC 1979 (Deflate):

10. Option length. Section 3 draws a four-octet option {type, 4, method and
    window, check}; that is what we send and the only length we accept.
    Method must be 8 and the check field 0, anything else is Nak'd with our
    values.
11. Window field. The window is `((w - 8) << 4) | 8`, w from 8 to 15. A peer
    that asks for 8 gets 8: the decompressor and the compressor here work
    with a true 256-octet window, so the well-known trouble other
    implementations have with that size does not arise on our side. If a
    peer Naks our 8 upward we follow.
12. The window a peer names in its request is the window of the compressor
    it will run towards us, and so the size our decompressor must keep; the
    window in the request the peer Acks is the one our compressor may use.
    We use a smaller one than allowed when `PPP_DEFLATE_MAX_WBITS` is
    smaller. (The RFC's wording "window size the sender is prepared to
    receive with" was checked against pppd: `deflate 12,10` makes it ask for
    12 and Nak our 15 down to 10, and both directions then decode.)
13. Sequence number (section 2.1): two octets, starts at 0 after the
    negotiation and after every reset, and counts every packet that went
    through the compressor, whether it was then sent compressed or in its
    native form (see 14; the receiver counts the native ones the same way).
    A packet with the wrong number means loss: it is dropped, a
    Reset-Request goes out, and compressed packets are dropped until the
    Reset-Ack.
14. What enters the history. Section 2.1 says the protocol field is
    compressed with the information; "Data Expansion" says an incompressible
    packet is sent as it is and still enters the history on both sides. We
    take this to hold for every packet of a protocol in the range 0x0000 to
    0x3FFF, except 0x00FD and 0x00FB, that is received while Deflate is on
    (we ourselves send only IP that way), and not for control protocols. The history gets the protocol in the same
    form the compressed packet would carry it.
15. Protocol field inside the compressed data: sent in one octet when the
    number allows it, whatever LCP agreed for the outside; both forms are
    accepted on receive.
16. End of a packet. The compressor ends each packet with an empty stored
    block cut after its three header bits and the padding (the four octets
    00 00 FF FF are not sent, section 2.1). The decompressor accepts any
    sequence of stored, fixed and dynamic blocks and requires the data to
    end exactly at that point; a block with BFINAL set, data left over, a
    distance beyond the window or a packet over the MRU is a decode error
    and leads to a Reset-Request.
17. The draft option number 24 is offered only after 26 was rejected, and
    accepted from a peer that offers only it. The packet format is the same.

RFC 1951:

18. Block choice. One block per packet: dynamic codes unless the fixed
    codes are shorter (short packets); stored blocks are never used because
    the native form does that job. Code lengths are limited to 15 and 7 by
    halving the counts and building the tree again.
19. A dynamic block with a single distance code or none is written with two
    one-bit distance codes, so that the code is complete and no decoder has
    to know the exception of section 3.2.7. The decompressor rejects
    over-subscribed codes, accepts incomplete ones, and treats a bit
    pattern that has no symbol as a decode error.

RFC 2118 (MPPC):

20. Bit order: most significant bit first, as the examples of section 4
    imply. The last octet is padded with zero bits.
21. Header bits (section 3.1): A (0x8000) history flushed before this
    packet, B (0x4000) packet starts at the front of the history, C
    (0x2000) compressed, D (0x1000) is MPPE and must be zero; a packet with
    D set is a decode error. The low 12 bits count every packet we send
    with the header, wrapping at 4095.
22. Wrap of the history: when a packet does not fit in what is left of the
    8192 octets, the compressor goes back to the front and sets B; the old
    contents behind the new packet stay usable for copies. After a flush
    the packet is at the front too, and A and B are set together. The
    compressor never codes a copy that reaches across the front. The
    decompressor treats the history as a ring, which accepts both
    readings of what lies "before" the front.
23. Expansion (section 3.2 "Packet Expansion"): a packet that would grow is
    sent with C clear, uncompressed behind the header, and the history is
    flushed (A on the next packet). It does not enter the history. If even
    that form exceeds the MRU, the packet goes out as native IP without any
    header, which every PPP accepts while CCP is open.
24. Protocol field inside the packet: always two octets when we send (the
    RFC says the protocol field is compressed "if negotiated"; two octets
    are valid either way); one or two accepted.
25. Synchronization (section 3.1 "Coherency count"): a jump in the count
    means loss. We drop the packet, send a Reset-Request and drop packets
    until one arrives with A set. We also send a Reset-Ack when we get a
    Reset-Request, in addition to flushing and setting A, because RFC 1962
    asks for one; the RFC 2118 text makes the A bit the answer. A peer that
    never sends a Reset-Ack is therefore fine: the Reset-Request is repeated
    only when a packet without A arrives after the waiting time, never
    while the peer is silent, and never leads to a renegotiation.
26. The option is four octets of supported bits, 0x00000001 for MPPC. The
    other bits are encryption (MPPE); the MPPC module itself never Acks them,
    and they are handled one layer up in `ppp.c` (see below) only when
    `[ppp] encryption = on`.

RFC 2759 / 3078 / 3079 (MS-CHAPv2 and MPPE):

27. MS-CHAPv2 (`ppp_crypto.c`) is offered through LCP as CHAP with algorithm
    0x81. On the caller's 49-octet response we form the NT-Response from the
    MD4 of the password in UTF-16LE (the configured secret is UTF-8 and is
    decoded, surrogate pairs included, which is what a Windows client
    hashes), check it, and answer "S=" and the 40-hex authenticator. A
    Failure is "E=691 R=0 C=<32 hex> V=3 M=..." (RFC 2759 section 6; no
    retry is offered, the C= challenge is sent because the format has it).
    An unknown user is hashed with an empty secret, so the answer takes as
    long either way. The vectors of RFC 2759 section 9.2 are in
    `test/test_crypto.c`.
28. Authentication fallback. A peer that Naks our method gets its own
    suggestion if we allow it, otherwise the strongest method left:
    the MS-CHAP not yet tried, then CHAP-MD5, then PAP; never straight to
    PAP and a password in clear. With encryption required there is no
    fallback past MS-CHAP: MPPE is keyed from it, so a refusal ends the
    link before any password is sent.
28a. Which MS-CHAP goes first (since 0.14.1). MS-CHAPv2, except for Windows
    98: its Dial-Up Networking Acks MS-CHAPv2 and logs in, then cannot make
    MPPE keys from it and hangs up after the Success (error 742; its own PPP
    log says "Login was successful" then "Remote access driver is shutting
    down"), so a Nak-driven fallback never reaches v1 for it. It is known
    by its first LCP request, which is exactly ACCM, magic, PFC, ACFC and
    callback by CBCP (13, operation 6), in that order and nothing else;
    Windows XP adds MRRU and an endpoint discriminator, pppd asks for no
    callback. With both MS-CHAP versions allowed, our first LCP request
    waits for the caller's (a second at most; callers send theirs at once)
    and offers v1 first for that shape, v2 for anything else
    (`ppp_counters_t.win98` says which). Linux pppd must get v2: 2.5.0 to
    2.5.4 receive with an all-zero key after v1 (`mppe_set_chapv1` hands
    one buffer to `mppe_set_keys` as both keys, which zeroes it between the
    copies; fixed in 2.5.5) and no pppd does 40-bit MPPE after v1.
28b. MS-CHAP v1 (RFC 2433, algorithm 0x80): an 8-octet challenge; of the
    49-octet response only the NT one (flag 1) is taken, as the LAN Manager
    one alone proves only the password upper-cased and cut to 14 octets; a
    plain Success, "E=691 R=0" on a failure. Its MPPE keys (RFC 3079
    section 2) are the same both ways: 128-bit from SHA1 of the NT hash's
    hash twice and the challenge, 40/56-bit from the first 8 octets of the
    LAN Manager hash (no challenge in them: Microsoft's design, and all
    Windows 98 does). The RFC's printed 128-bit InitialSessionKey has a
    typo ("ac ca" for "ac c1"; its own next line has c1). Vectors of RFC
    2433 Appendix B and RFC 3079 2.5 are in `test/test_crypto.c`.
29. The MPPE keys (RFC 3079) come from the NT-Response and the hash of the
    NT-hash: the master key, then a send and a receive start key (the
    server's assignment). Each direction's first session key is
    GetNewKeyFromSHA(start, start), truncated, no RC4 (RFC 3079 3.3; the
    "session key" of its 3.5 samples, whose 128- and 40-bit RC4 lines
    reproduce; the one 56-bit line is inconsistent in the RFC itself). A key
    change (RFC 3078 7.3) is GetNewKeyFromSHA(start, current), truncated,
    then RC4-encrypted under itself; then the short keys are forced (40-bit:
    D1 26 9E; 56-bit: D1).
30. MPPE on the wire (`ppp_mppe.c`), stateless (bit H, 0x01000000) with
    the Linux kernel's schedule, which is what pppd and Windows VPN clients
    do: the coherency count starts at 4095,
    and a key change comes before every packet, the first one (number 0)
    included -- so no packet is ever sent under the initial key. The header
    is A (FLUSHED, always) | D (ENCRYPT) | the 12-bit count, then RC4 of the
    protocol field and the packet. The receiver makes one key change per
    count since the last packet, stepping over lost ones, up to
    `PPP_MPPE_WINDOW` (2047, under half the counts, as the kernel does); a
    count at or behind the last one (a replay) or further ahead is dropped
    and changes nothing. The key changes for a run of lost packets are paid
    out of a credit (8 a packet, at most 512 saved): a run longer than the
    credit is caught up on over the next packets (the receiver steps as far
    as the credit goes and drops the packet), so a long burst of loss is
    always followed, while a peer that jumps ahead in every packet costs
    only the credit. Past the window neither mode can get back in step
    (re-keying would reuse the keystream): after `PPP_MPPE_LATE_MAX` (32)
    such packets in a row the link ends with `PPP_END_MPPE_LOST`. A frame
    without D is refused. One key length both ways: we offer 128-bit (S,
    0x40) and accept 56-bit (M, 0x80) or 40-bit (L, 0x20) only from a peer
    whose request or Nak shows it has nothing longer (its strongest); the
    short keys are the first 8 octets of the master start keys, forced as
    in 3.4 (RFC 3079 3.3; the kernel does the same). Should the two
    directions be Acked at different lengths (a peer that showed its short
    keys only after Acking our 128-bit request), CCP is negotiated again
    before anything is keyed. A Nak (one frame anyone on the line could
    inject) never takes the length below the strongest of the peer's own
    request. On request (the default) our first request follows the
    caller's, key length included; with encryption required it is 128-bit
    alone, as when Windows XP and 11 were confirmed against it, and a
    Windows 9x/ME client Naks it down. The short keys' packets are checked
    against a reference written apart (`test/test_mppe.c`,
    test_short_vectors; its 40-bit initial key is RFC 3079 3.5's), and
    `srv/test/ns_pppd.c` runs a `require-mppe-40` call against the
    kernel's `ppp_mppe`.
30a. Stateful MPPE (H clear; RFC 3078 7.2, 8.2), per direction, which is
    the only mode a Windows client does on a modem or PPPoE link (issue #6;
    Windows does stateless on its VPN links only). We ask for stateless
    first, take stateful when the peer Naks toward it, and once the peer has
    asked for stateful we ask for it too. One RC4 stream runs from packet to
    packet; the first packet (count 0) is under the initial key with no
    FLUSHED (as the kernel sends it); the key changes on flag packets (count
    ending in 0xFF), which we send FLUSHED. FLUSHED received means "RC4 again
    from the current key", not a key change (RFC 3078 3; Windows sets it on
    its first packet and after a Reset-Request, and not on its flag
    packets). A gap in the counts puts the receiver out of step: one CCP
    Reset-Request, everything dropped until a FLUSHED or flag packet, then
    one key change per flag packet missed. A Reset-Request from the peer is
    answered with a Reset-Ack (the kernel's receiver waits for one) until the
    peer Code-Rejects that, as Windows does, and does NOT
    restart RC4 on the current key as 8.2 has it (that would send a used
    keystream again): the count jumps to the next flag packet, which changes
    the key and goes out FLUSHED; the peer sees the skipped counts as lost.
    `test/test_mppe.c` starts from RFC 3079's own "test message" sample
    (exactly a stateful packet 0); `test/test_ccp.c` has a Windows client
    modelled apart from ppp_mppe.c. The per-packet vectors in `test/test_mppe.c` and
    `test/test_crypto.c` come from a separate reference (Python's SHA-1 and
    an RC4 written apart from ours) that reproduces RFC 3079's samples, and
    `srv/test/ns_pppd.c` runs a whole call against pppd with
    `require-mppe-128` and the kernel's `ppp_mppe`.
31. Fails closed. With `cfg.encrypt`: CCP offers and accepts option 18 with
    MPPE only, 128-bit or the short keys above (`comp` is cleared at `ppp_init`, so no
    Deflate or MPPC to fall back to); if MPPE is not agreed in both
    directions when CCP opens, if the peer Protocol-Rejects CCP, if the login
    gave no keys, or if MPPE is still not up after CCP's whole retry span
    (restart time times one more than Max-Configure), the link is ended with
    `PPP_END_ENCRYPT_REQUIRED`. Meanwhile no cleartext IP is sent
    (`ppp_send_ip` refuses) or accepted (dropped and counted). An IP packet
    must fit the peer's MRU with the 4 octets MPPE adds
    (`PPP_MPPE_OVERHEAD`); the daemon sets an encrypting port's TUN MTU that
    much lower.
32. A CCP re-open does not re-key. Starting the schedule again from the
    master key would reuse the keystream, so MPPE is keyed once per
    authentication; a CCP re-open after it has run ends the link. A bad or
    short MPPE frame is dropped and counted; stateless, never answered with a
    Reset-Request; stateful, only out of step (30a), and a Reset-Request
    never leads to a CCP renegotiation. The keys of a login are wiped when
    LCP goes down.
33. What the caller asked for on option 18 is kept (`mppe_peer_req`)
    whether or not we encrypt, so a call that ends because the caller
    wanted MPPE and did not get it (Windows error 742) shows its request
    in the log.
34. A trace (`cfg.trace`, optional) of every control packet each way (any
    protocol from 0x8000: LCP, PAP, CHAP, IPCP, CCP), never IP: the time
    since `ppp_init`, the protocol, up to `PPP_TRACE_MAX` (64) octets from
    the Code octet, and the whole length. PAP and CHAP give their 4-octet
    header only: no password, challenge answer or name reaches a log, and
    nor does one inside an LCP Protocol-Reject of a PAP or CHAP packet.
    Received packets are traced on arrival, before they are judged.
    Configure-Nak and -Reject replies, like every other packet we send, fit
    the peer's MRU (whole options, as many as fit; the peer asks again
    without them).
35. Van Jacobson TCP/IP header compression (RFC 1144; `ppp_vj.c`), IPCP
    option 2 with protocol 0x002d (RFC 1332 3.2), each way on its own. Our
    request offers it with Max-Slot-Id 15 (16 slots) and Comp-Slot-Id 1;
    the peer's, in RFC 1332's form or RFC 1172's older 4-octet one, is
    Acked with any slot count (we use at most 16) and its Comp-Slot-Id
    honoured (0: every compressed packet names its slot). Any other
    compression protocol (RFC 2507's IPHC, 0x0061) is Rejected; a Nak
    toward one drops ours. `cfg.no_vj` turns it off both ways. A TCP
    datagram goes as 0x002d (only what changed: a few octets for an echoed
    keystroke) or 0x002f (whole, its slot in the protocol field: a new
    connection, a resend, changed options); anything else as 0x0021.
    VJ works on the datagram, before CCP: what it makes is compressed or
    encrypted like IP, and under MPPE a cleartext VJ packet is dropped
    like cleartext IP. After a frame lost on the way in (a bad FCS, an
    abort, an overlong or short frame, one the decompressor or MPPE could
    not use) compressed packets are tossed until one names its slot or an
    uncompressed one comes (RFC 1144 4.2); the sender's TCP resends what
    was lost, which goes uncompressed and puts the slot right. A packet
    that cannot be sent keeps the compressor's slots as they were. VJ that
    was not agreed is Protocol-Rejected; a Protocol-Reject of VJ from the
    peer stops it towards the peer. Counters: `vj_tx_comp`,
    `vj_tx_uncomp`, `vj_rx_comp`, `vj_rx_uncomp`, `vj_errors`, and the
    slots agreed each way. `test/test_vj.c` rebuilds long mixed streams to
    the octet and fuzzes the decompressor; `srv/test/ns_pppd.c` runs every
    call against pppd and the kernel's slhc with it on (and one with
    `novj`).

## Windows callers: assumed, not tested

No Windows dial-up client was available. The code assumes, from RFC 2118
and the behaviour described in it:

- the client requests option 18 alone, with 0x00000001 or with encryption
  bits added; it rejects options 26 and 24 with a Configure-Reject (it does
  not Nak them), and Acks option 18 with 0x00000001 in our request;
- it accepts a Nak that reduces its encryption bits to 0x00000001 unless
  the user demanded encryption, in which case it gives up CCP or the call;
- it resynchronizes on the A bit and may or may not send a Reset-Ack;
- it accepts native IP packets while MPPC is on;
- with encryption, it authenticates with MS-CHAP (v1 on Windows 98),
  requests option 18 with stateful MPPE (H clear; 0x60 for "require",
  0x40 for "maximum strength"; with software compression on XP asks 0xe1,
  Windows 98 0x21), Naks our stateless request down to
  stateful, derives the same keys, and sends every IP packet encrypted with
  the ENCRYPT bit and a rising coherency count (30a). This was learnt the
  hard way: 0.8.0 offered stateless only and no Windows caller could
  encrypt (issue #6: errors 742, 628, and "optional" calls dropped after
  CCP's retry span). Windows 9x/ME without the 128-bit upgrade has 40-bit
  keys only; up to 0.13 they were refused (error 742), and since 0.14 they
  are taken (31). Stateful Windows behaviour is from RFC 3078 and a
  2011 linux-kernel report of Windows XP/7 PPPoE clients against pppd.

The models of these in `test/pppc.c` (`PPPC_CCP_WINDOWS`,
`PPPC_CCP_WINDOWS_MPPE`) encode exactly these assumptions and nothing more.
MS-CHAPv2 and MPPE are also checked against a peer not written here:
`srv/test/ns_pppd.c` makes a whole call to the daemon from pppd 2.5 with
`require-mppe-128`, through the Linux kernel's `ppp_mppe` (whose key
schedule was itself matched against Windows), and moves files both ways
over TCP. Since 0.14.1 real Windows 98 SE and XP clients have been run
against the daemon (on the ISP's rig, through a modem bridge): with and
without "Require data encryption", MPPE at 40 and 128 bits, pings and a
web page across.

## Tests

`make test` (host), `make test-arm` (qemu, shortened runs; `ARM_ARGS=` for
the full length), `make bench` (ratios on the sample files).

- `test/test_deflate.c`: bit-exact vectors, every block type, all window
  sizes, 100000 packet round trips of mixed traffic, the effort bound,
  garbage and truncated input.
- `test/zlib_check.py` with `test/comp_tool.c`: our compressor's output
  inflated by Python zlib with a kept history, zlib's sync-flushed output
  (all levels and strategies, windows 9 to 15) decoded by our decompressor.
- `test/test_mppc.c`: vectors for each literal and copy encoding class, the
  history wrap, the count wrap at 4095, expansion, garbage.
- `test/test_ccp.c`: the negotiation table (pppd, old pppd, small windows,
  Windows with and without MPPE, unknown options, rejects, Naks), data both
  ways, resets, loss, renegotiation, MRU limits, the processor budget, and
  the MPPE path: a full encrypted session, the fail-closed cases (peer
  refuses MPPE, cleartext refused in and out), and a CCP re-open ending the
  link rather than re-keying.
- `test/test_crypto.c`: the RFC 2759 and 3079 vectors (NT-Response,
  authenticator, master and session keys).
- `test/test_mppe.c`: the RFC 3079 RC4 sample, round trips at 128/40/56-bit,
  loss recovery, and a plaintext frame refused.
- `test/test_e2e.c`: client and server over the HDLC framing with loss.
