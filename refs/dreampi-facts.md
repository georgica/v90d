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
  some dial codes bridge the modem to TCP game servers. A Pi cannot ring,
  so it cannot be the called side. (V)

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
