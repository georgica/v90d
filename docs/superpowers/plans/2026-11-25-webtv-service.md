# WebTV Service Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A WebTV / MSN TV box dialing its usual numbers on a v90d port reaches WebTV Redialed, built in the way PROTOWEB is.

**Architecture:** Fixed numbers recognised in srv/srv.c set a per-call `webtv` flag (PAP accept-any, no encryption, no menu, V.34 cap on the toll-free number). netsvc.c sees the flag like PROTOWEB's and asks nat.c for two per-peer DNATs (10.0.0.1 to WebTV Redialed, 157.57.221.51:53 to the port's name server). A child process resolves community.webtv.zone.

**Tech Stack:** C99, v90d's own PPP (ppp/ppp.h `pap_cb`), nftables netlink and iptables back ends in srv/nat.c, the cfg parser.

**Spec:** `docs/superpowers/specs/2026-11-25-webtv-service-design.md`

## Global Constraints

- No em dashes in code comments, docs or UI text.
- Numbers, with or without a leading 1: `8009328800`, `8006138199` (V.34 at most), `4848201337`.
- Answered only on ports whose service is internet or archive, never on the port's own number, only while `[services] webtv = on` (default on).
- WebTV Redialed's name: `community.webtv.zone`; the box's fixed addresses: `10.0.0.1` (service), `157.57.221.51` (DNS).
- Only one public IPv4 answer of the lookup is taken; a failed lookup keeps the last good address.
- Call label: "WebTV (PPP)".
- All modem behaviour stays in libv90modem; nothing here changes the modem.

## Review Focus

- The port's own number set to a WebTV number: it answers as the port's own (normal login), not WebTV. Test in Task 2.
- A WebTV number on a Dreamcast or Saturn port, or with `webtv = off`: not answered as WebTV. Test in Task 2.
- `[ppp] encryption = on` on the port: a WebTV call still gets PAP and no MPPE. Test in Task 2.
- A lookup that answers a private address (10.x, 127.x) or nothing: the address is not taken. Test in Task 4.
- Both ports in WebTV calls at once: rules for both peers. Test in Task 3.

---

### Task 1: The `[services] webtv` setting

**Files:**
- Modify: `cfg/config.h` (field `int svc_webtv;` beside `svc_netstalgia`), `cfg/config.c` (key `K_SVC_WEBTV` "webtv", default 1, parse, check, write after `menu`), `cfg/settings.c` (select "WebTV numbers" on the Services page, field `F_SVCWEBTV`, on/off), `cfg/v90d.conf.example`, `fw/v90d.conf`, `cfg/README.md`
- Test: `cfg/test/test_config.c`, `cfg/test/test_settings.c`

**Interfaces:**
- Produces: `cfg_t.svc_webtv` (1 = answer the WebTV numbers).

- [ ] Test: `cfg_default` gives `svc_webtv == 1`; `"[services]\nwebtv = off\n"` gives 0; `webtv = maybe` fails with "webtv: 'maybe' is not one of: on, off"; write/read round trip keeps 0; the settings form posting the field off gives 0.
- [ ] Run `make test` (cfg tests): FAIL.
- [ ] Implement as the `svc_netstalgia` key is done.
- [ ] Run `make test`: PASS. Commit "cfg: [services] webtv".

### Task 2: The numbers and the call (srv)

**Files:**
- Modify: `srv/srv.h` (`SRV_WEBTV "8009328800"`, `SRV_WEBTV_TF "8006138199"`, `SRV_WEBTV_56 "4848201337"`; field `int webtv;` beside `protoweb`; `int srv_webtv(const srv_t *s);`), `srv/srv.c` (`webtv_here()`, `tel_more`, `call_start`, PAP callback), `srv/termsvc.c` (label)
- Test: `srv/test/test_srv.c`

**Interfaces:**
- Produces: `int srv_webtv(const srv_t *s)`: 1 while a WebTV call is in progress.

- [ ] Tests (each number with and without the 1): on an internet port the call is WebTV (`srv_webtv` 1); PPP config seen by the DCE has `auth == PPP_AUTH_PAP`, `pap_cb` set and accepting "x"/"y", `encrypt == 0` even with `[ppp] encryption = on`, no menu link; `8006138199` has none of V90M_MOD_V90/V92/X2/K56; `4848201337` keeps V90 when the port has it; not WebTV when the number is the port's own, on a Dreamcast port, or with `svc_webtv = 0`; `tel_more` says 2 for each full number and 1 for "800932".
- [ ] Run `make test`: FAIL.
- [ ] Implement: `webtv_here(s)` = `c->svc_webtv` and service internet or archive. In `call_start` decide `s->webtv` after `protoweb`, before `acts` (and `acts` excludes it); for a WebTV call set `enc = 0`, `d.ppp.auth = PPP_AUTH_PAP`, `d.ppp.pap_cb` = a function returning 1, skip the menu, strip V.90/V.92/x2/K56flex for the toll-free number (release `heavy_held` if it was taken: decide the strip before the x2/K56 slot block). termsvc: "WebTV (PPP)".
- [ ] Run `make test`: PASS. Commit "srv: the WebTV numbers".

### Task 3: The NAT rules (nat)

**Files:**
- Modify: `srv/nat.h` (`int webtv[2];` per peer, `uint32_t webtv_to;` WebTV Redialed, `uint32_t webtv_dns;` the port's name server, both host order; defines `NAT_WEBTV_ADDR` 10.0.0.1 and `NAT_WEBTV_DNS` 157.57.221.51), `srv/nat.c` (both back ends)
- Test: `srv/test/ns_nat.c`

- [ ] Test: with `webtv[0]` and `webtv_to` set, rules DNAT peer 0's TCP and UDP to 10.0.0.1 (any port) to `webtv_to`, and UDP and TCP 53 to 157.57.221.51 to `webtv_dns`; with `webtv_to == 0` only the DNS rules; both peers set: rules for both; `nat_apply` counts the plan as wanting rules.
- [ ] Run the ns_nat test: FAIL.
- [ ] Implement with the existing `dnat_msg` (nft) and the iptables `DNAT` path, as the console rewrites are done.
- [ ] Run: PASS. Commit "nat: per-call WebTV rewrites".

### Task 4: The address and the wiring (netsvc, main)

**Files:**
- Modify: `srv/netsvc.c`, `srv/netsvc.h` (`void netsvc_set_webtv(const char *addr);` for `-Q`), `srv/main.c` (`-Q ADDR`: WebTV Redialed's address, no lookup; `-Q ""`: no lookup at all)
- Create: `srv/webtv.h`, `srv/webtv.c`: `uint32_t webtv_pick(const struct addrinfo *ai);` (first public IPv4, host order, 0 = none) and `#define WEBTV_HOST "community.webtv.zone"`
- Test: `srv/test/test_webtv.c` (picks), `srv/test/ns_call.c` (a call)

- [ ] Tests: `webtv_pick` skips 10.x, 127.x, 192.168.x, 100.64.x and returns the first public one; NULL gives 0. ns_call: with `-Q 192.0.2.7` a call to 18009328800 logs "port 1: a call to WebTV: 10.0.0.1 goes to WebTV Redialed (192.0.2.7)" and the plan carries `webtv[0]`; after hang-up the rules go.
- [ ] Run: FAIL.
- [ ] Implement: a lookup child like `dc_poll` (getaddrinfo in the child, writes the 4 octets to a pipe) while any port has `webtv_here`, every 10 minutes, a failure keeping the last address; `port_webtv(i)` in the `pw_poll` loop alongside `port_archive`; `build_plan` fills `webtv[]`, `webtv_to`, `webtv_dns` (`ppp_local` when the relay is on, else `dns[0]`) and wants rules while any WebTV call is up; a call with no address yet logs a warning.
- [ ] Run `make test`, `make check`: PASS. Commit "netsvc: WebTV Redialed for WebTV calls".

### Task 5: Docs

- [ ] README (services list), DESIGN.md (beside PROTOWEB), PROGRESS.md entry. Commit "Docs: the WebTV numbers".
