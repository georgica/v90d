# ppp/: the PPP server

HDLC framing (`ppp_hdlc.c`), the option negotiation automaton (`ppp_fsm.c`),
LCP, PAP, CHAP-MD5, IPCP and CCP (`ppp.c`), and the two compression
algorithms CCP can agree on: Deflate (`ppp_deflate.c`) and MPPC
(`ppp_mppc.c`). Portable C99, integer only, no allocation, no OS calls.

This file is about the compression part: what was built, from what, how it
is bounded, and every place where the RFCs leave a choice.

## Sources

Only these, kept in `refs/rfc/`: RFC 1962 (CCP), RFC 1979 (PPP Deflate),
RFC 1951 (the deflate format), RFC 2118 (MPPC), together with RFC 1661 for
the automaton. No other implementation was read. Python's `zlib` module is
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
  (pppd sends 26, 24 and BSD together and expects exactly that). MPPC with
  encryption bits (MPPE, 0x00000020 / 0x00000040 / 0x00000080 / 0x01000000)
  is answered with a Nak that carries 0x00000001 only. A caller that insists
  on encryption runs into Max-Failure and gets a Reject: no compression,
  IP stays up.

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
    other bits are encryption and are never Ack'd.

## Windows callers: assumed, not tested

No Windows dial-up client was available. The code assumes, from RFC 2118
and the behaviour described in it:

- the client requests option 18 alone, with 0x00000001 or with encryption
  bits added; it rejects options 26 and 24 with a Configure-Reject (it does
  not Nak them), and Acks option 18 with 0x00000001 in our request;
- it accepts a Nak that reduces its encryption bits to 0x00000001 unless
  the user demanded encryption, in which case it gives up CCP or the call;
- it resynchronizes on the A bit and may or may not send a Reset-Ack;
- it accepts native IP packets while MPPC is on.

The models of these in `test/pppc.c` (`PPPC_CCP_WINDOWS`,
`PPPC_CCP_WINDOWS_MPPE`) encode exactly these assumptions and nothing more.

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
  ways, resets, loss, renegotiation, MRU limits, the processor budget.
- `test/test_e2e.c`: client and server over the HDLC framing with loss.
