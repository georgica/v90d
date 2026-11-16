# DreamPi and the Dreamcast: facts for a "Dreamcast mode" (researched 2026-11-15)

Facts about behaviour only, from DreamPi's public repository (MIT,
github.com/Kazade/dreampi: dreampi.py, netlink.py, dcnow.py,
etc/dnsmasq.conf), the Dreamcast Live guides (dreamcastlive.net) and a few
secondary pages. Nothing here is code, and none of DreamPi's code is used.
"V" = read in a primary source, "S" = secondary, "H" = hearsay.

## What DreamPi does

- A USB voice modem wired to the Dreamcast, with a "line voltage inducer"
  because a USB modem feeds no loop current. It plays a dial tone sample,
  reads the dialed DTMF digits (a 2 s gap ends the number), then stops the
  tone, waits 8 s, sends ATA, waits for CONNECT, waits 5 s and starts pppd.
  It never rings and gives no ringback. Any number it does not know means
  "ISP". (V)
- pppd: `auth` with a PAP wildcard secret (any user, any password),
  `ms-dns <itself>`, `proxyarp` (the Dreamcast becomes a host on the LAN,
  so the home router can DMZ it), `noccp`, `ktune`, `debug`; no `novj`, no
  MTU/MRU, no LCP echo. Serial rate at most 57600. `noccp` since 2015:
  "Don't try to negotiate compression (Starlancer gets it wrong)". (V)
- DNS: dnsmasq with `no-resolv`, forwarding only to 46.101.91.123 and
  209.50.50.129, which answer ordinary names and send the dead game
  servers' names to the revived ones. Both answered on 2026-11-15;
  volunteer-run. 67.222.144.120 (old list) is dead. (V)
- Packet fixes: TTL of the Dreamcast's packets set to 64 (its own is said
  to be 30; added for Daytona USA); a repeated TCP SYN within 1 s from
  source ports 3200-3205 dropped; a fetched list (shumania.ddns.net/dnat.txt)
  of dead server addresses rewritten to revival hosts. (V)
- "Dreamcast Now": every 15 s a POST to dcnow-2016.appspot.com with a hash
  of the Pi's MAC and a hash of the last name the console looked up
  (which game is being played). Optional. (V)
- UPnP port mapping per game (off by default), an OpenVPN client for the
  IWANGO games, closed helper programs (dcvoip, dcgamespy, dc2k2,
  dcdaytona) whose behaviour is not documented. (V)
- Saturn NetLink / XBAND and some dial-code games: numbers of the form
  `#...#` answer the modem with error control and compression off and relay
  the raw octets to a peer over UDP (matchmaking at saturn.dreampipe.net);
  some dial codes bridge the modem to TCP game servers. A Pi cannot ring;
  the called side's console dials too and is shown the text of a ring
  (second pass, below). (V)

## The console

- US and later Japanese consoles: 56k modem; PAL and early Japanese: 33.6k
  (S). Whether the 56k one does V.90 or only K56flex: not found.
- Modem "A" is powered by the console, "B" by the line; PAL said to need
  18 V at about 50 mA, US "B" about 9 V (S, H).
- It waits for dial tone unless told to dial blind (H).
- Settings people use: any number (111-1111; Alien Front Online resets on
  short ones like 555), a login of their own, any password, DNS 0.0.0.0
  (taken from IPCP). (V)
- Quirks: PSO disconnects cured by an MTU of 1400 on the router; Monaco GP
  and POD by the game's "limit to 28.8k"; a Capcom/KDDI number that needs
  `novj`, a non-empty PAP password and LCP echo every second. (V)
- Not found anywhere: a PPP trace of a Dreamcast (its LCP/IPCP requests,
  whether it does CHAP), how long it waits for answer tone, whether it
  minds ringback.

## Ports some games need opened towards the console (connection guide, V)

Alien Front Online UDP 7980; ChuChu Rocket UDP 9789; Dee Dee Planet UDP
9879; Driving Strikers (host) UDP 30099; Floigan Bros TCP 37001; Internet
Game Pack UDP 5656, TCP 5011, 10500-10503; 2K series UDP 5502, 5503, 5656,
TCP 5011, 6666; Next Tetris TCP/UDP 3512; Ooga Booga UDP 6001; Starlancer
and PBA Bowling TCP/UDP 2300-2400, 47624, UDP 6500 (PBA also UDP 13139);
World Series Baseball 2K2 UDP 37171, 13713; Worms World Party TCP 17219;
Planet Ring UDP 1028, 1285, 7648. PSO, Quake III and 4x4 Evolution need
only the DNS.

## Second pass (2026-11-15): DreamPi v2.1 source read for behaviour, nothing copied

Corrections to the above: the address-rewrite rules match protocol and
usually port, not the address alone; Dreamcast Now is HTTPS; a DreamPi can
be the called side of a NetLink game, by writing the text of a ring into
the data stream.

### Address-rewrite list
- `https://shumania.ddns.net/dnat.txt`, fetched once when the service
  starts (3 s connect timeout), never again. A non-200 answer leaves the
  previous copy in use; with no copy there are no rules. (V)
- Plain text, one rule a line, single spaces: original destination, new
  destination, optional port, optional protocol. 15 lines on the day, e.g.
  `172.17.18.2 146.185.135.179 9500`, `172.17.1.14 146.185.135.179 9500 udp`,
  `63.251.242.131 146.185.135.179 8080`, `45.79.32.243 10.200.0.1 8888`
  (that target is inside the VPN). Nine lines are 203.179.41.x to
  146.185.135.179 port 9500. (V)
- Rules (NAT, before routing, any interface, any source): four fields:
  that destination, that protocol, that port, rewritten to the new address,
  port kept. Three fields: the same for TCP. Two fields: TCP, any port. The
  port is never changed; UDP only when the line says so. (V)

### Dreamcast Now
- HTTPS POST to `https://dcnow-2016.appspot.com/api/update/<hash>/` every
  15 s from the moment PPP is up (first one at once) until PPP ends; no
  "offline" message. Timeout 15 s, answer ignored. (V)
- `<hash>`: SHA-256, 64 lowercase hex digits, of the 17-character text of
  the gateway's own MAC, uppercase hex pairs joined by colons. (V)
- Header `User-Agent: Mozilla/4.0 (compatible; MSIE 5.5; Windows NT), Dreamcast Now`.
  Body form-encoded with at most one field, `dns_query` = SHA-256 hex of
  the last name the console asked the DNS for (A query), exactly as asked.
  With nothing to report the body is empty. Names containing `appspot` are
  never reported; names containing `gameloft` or
  `onsen0.overworks.isao.net` once a session; others on every poll while
  they are recent. (V)
- On by default in DreamPi; a local page on port 1998 switches it. (V)

### UPnP
- Off unless the daemon is started with a switch the shipped unit does not
  pass. Once at start: delete, then add; delete again at exit. External
  port = internal port, towards the console's LAN address, description
  `DreamPi: <game>`. (V)
- UDP 1028, 1285, 3512, 6001, 6500, 7648, 7980, 9789, 13139, 13713, 37171
  and 2300-2400; TCP 3512, 17219, 47624 and 2300-2400. Smaller than the
  connection guide's list (no 5011, 5502, 5503, 5656, 6666, 9879, 30099,
  37001, 10500-10503). (V)

### The console's address
- A /24 around the default gateway is assumed; walking down from .99 the
  first two addresses not in the ARP table become the gateway's PPP end and
  the console (typically .99 and .98). No probe, no DHCP. Proxy ARP for the
  console's address. One DNS, the gateway itself. (V)
- The Capcom/KDDI number is 0642542154: no VJ, PAP with a non-empty
  password, LCP echo every second (4 lost ends the call). (V)

### The helper programs (shipped as ARM binaries without source)
All are packet rewriters on a netfilter queue; packets pass untouched when
a helper is not running. (V)
- dcgamespy: TCP from the console with exactly SYN and PSH set: PSH is
  removed, checksums redone. (V, from its own description)
- dcvoip: UDP source and destination port 1285 (Planet Ring voice); given
  the public address. Believed to rewrite an address inside the
  negotiation payload. Exact octets unknown.
- dc2k2: TCP with PSH to port 5011 (2K series "challenge"); believed to
  rewrite an embedded address. Exact rewrite unknown.
- dcdaytona: not in the repository; "IP fix" needing the WAN address.
  Unknown.

### VPN
- OpenVPN client towards one of three volunteer nodes; neither the server
  list nor the keys are published (they ship in the SD-card image and are
  refreshed through the tunnel). Said to be needed for the IWANGO games:
  Sega Tetris, Aero Dancing, Golf Shiyouyo 2, Daytona USA. (V, S)

### NetLink tunnel (upstream eaudunord/Netlink, which DreamPi updates to)
- Dialled numbers: `0` or `00` wait for a direct call; `#0NN#` wait with
  matchmaking for game NN; `#a*b*c*d#` or `#` 12 digits `#` call that IPv4
  address; other `#...#` call through matchmaking (game = last two
  characters); a gap of 1.5 s ends the number. (V)
- Modem towards its own console, both sides: error control and compression
  off, then answer (`ATA`), up to 60 s for the connect. The waiting side's
  console never hears a ring: once a peer is there, the text `RING` CR LF
  `CONNECT` CR LF is written into the data stream and the game takes it for
  result codes. The calling side's console gets nothing. (V)
- Direct: the waiting side listens on TCP 65432 (120 s). The caller
  connects and sends the 7 octets `readyip`; the answer is the 5 octets
  `g2gip`. Then UDP: the waiter sends to the caller's address port 20002,
  the caller to the dialled address port 20001. (V)
- Matchmaking: public address and port by STUN (stun.l.google.com:19302)
  from the data socket (local 20001 waiting, 20002 calling; the waiter
  repeats every 5 s). HTTPS GET
  `https://saturn.dreampipe.net/match_service.php` with `action`
  (`wait`, `status`, `match`, `timeout`), `gameID`, `client_ip` (12 digits,
  octets padded to three, no dots), `port` (with wait and match), and a
  fixed `key` that is in the public source. Needs a browser-like
  User-Agent (a short one is refused with 406). Answer JSON: `status`
  and `opponent ip_address` (key with a space), the latter a pair of
  12-digit address and port when `status` is `matched` (to `status`) or
  `found opponent` (to `match`). Waiter: `wait` once, `status` every
  second, `timeout` after 120 s. No TCP on this path. (V)
- UDP data: TOS 184. Control datagrams, exact ASCII: `OPEN_SHIRO` (once,
  ignored), `PING_SHIRO` (about every second), `PONG_SHIRO` (answer). A
  data datagram holds one to five records joined by the text
  `<packetSplit>`; a record is the raw modem octets, the text
  `<dataSplit>`, and a sequence number in decimal. Newest first, the four
  before it behind it. Sequence from 0, one per read from the modem. Every
  datagram is sent twice. No escaping. At most 1024 octets read per
  datagram. Receiver: expects 0 first; uses the record with the expected
  number if present, else the newest; a newest below the expected one drops
  the datagram; after use, expected = that number + 1; repeats while newer
  records remain. No acknowledgement, no time-out; the session ends with
  the local modem's carrier. (V)
- XBAND and the dial-code TCP servers: not noted here beyond their
  existence (peer control on TCP 65433; Sega Rally 2 on 0352002700 relays
  to shumania.ddns.net port 21330).
