# WebTV as a built-in service (short design)

## Intent

A WebTV / MSN TV box plugged into a v90d port should reach WebTV Redialed
(community.webtv.zone) the way it already does through Netstalgia's ISP,
which the owner confirmed working on 2026-11-25. Built in the way PROTOWEB
is: fixed numbers, answered on the port's dial-up connection, with per-call
NAT.

## Approaches

- **A. Fixed numbers with per-call NAT, like PROTOWEB (picked).** The box
  dials its usual numbers, nothing to set up on the box or the port.
- B. A port service mode "WebTV" (like Dreamcast). Ties up a port for one
  kind of caller; the box's numbers already tell us who is calling.
- C. Route WebTV through Netstalgia only. Needs a Netstalgia account and the
  link that is paused; does nothing for a box on its own.

## Design (approved 2026-11-25)

- **Numbers**, each with or without a 1 in front, on ports whose service is
  internet or archive, never the port's own number:
  - 800-932-8800 (1-800-WEBTV-00);
  - 800-613-8199, the toll-free number a box with no TellyScript dials:
    V.34 at most (no V.90, V.92, x2, K56flex), as a box hangs up above 33.6K
    there;
  - 484-820-1337, the TellyScript's number: the port's usual modulations.
- **The call:** PPP asks for PAP and accepts any name and password (the
  box's built-in login); no encryption, whatever [ppp] says; no terminal
  menu.
- **Network, for that call only (per-call NAT):**
  - TCP and UDP to 10.0.0.1 go to WebTV Redialed's address;
  - DNS (UDP and TCP 53) to 157.57.221.51, the box's fixed name server,
    goes to the name server the port gives its callers (the box when the
    relay is on, else [network] dns 1).
- **WebTV Redialed's address:** looked up from community.webtv.zone by a
  child process (getaddrinfo), when a port can take WebTV calls, every
  10 minutes; only one public IPv4 answer is taken; a failed lookup keeps the
  last good address. Before any lookup has worked a WebTV call still
  connects, with a warning in the log.
- **Setting:** `[services] webtv = on|off`, default on, on the Services
  page. Calls show as "WebTV (PPP)".

## Non-goals

- nsdiald's ratefall / pcmfall (modem behaviour: libv90modem's, if wanted).
- 10.0.1.129 (the Dreamcast's address): the console service has its own list.

## Left to the builder

- Command-line override for the address in tests (no lookup).

## Changed (2026-11-25, owner's word)

After the first build the owner chose approach B instead: WebTV is a
service per port (`[line] service = webtv`), on which every call is the
box's whatever it dials; the three numbers end the dialling at once, the
toll-free one still V.34 at most. `[services] webtv` and the numbers on
internet and archive ports are gone. The NAT, the lookup and the login
rules are unchanged.
