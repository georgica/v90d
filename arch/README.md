# v90d/arch: archive mode gateway (v90arch)

With archive mode on for a telephone port (`service = archive` in its
`[line]` section, cfg/README.md), that port's caller's web and FTP traffic goes to
Protoweb (protoweb.org), a service that serves restored 1990s sites to old
browsers. The old PC needs no proxy setting: the box's kernel redirects the
caller's DNS, HTTP and FTP to `v90arch`, and `v90arch` talks to the Protoweb
proxy (one plain HTTP forward proxy, wayback.protoweb.org:7851).

One process, one poll() loop, non-blocking sockets, fixed tables, no
allocation after startup.

## The three services

**DNS (UDP).** Protoweb has no DNS and most archived names do not exist in
the public DNS, so every A query is answered with an address from a private
pool (default 198.18.0.0/15, TTL 60 s) and the pair name/address is kept in
a 512 entry table. The same name keeps its address while it is in the table;
when the table is full the least recently used name is replaced (looking a
name up by its address counts as a use). A new name always gets the next
free address of the pool, so the address of a replaced name is not reused
until the pool has gone around. Addresses ending in .0 and .255 are not
handed out. Other query types (AAAA, MX, PTR ...) get an empty NOERROR
answer. A name the table cannot hold (longer than 127 characters, or with
characters outside a-z 0-9 - _ .) gets NXDOMAIN. Malformed packets are
ignored. There is no upstream DNS at all.

**HTTP relay (TCP).** Reads the request head (at most 8 kB), picks the host
(the Host header; else the name that belongs to the connection's original
destination address; else that address in dotted form; a pool address in
the Host header is turned back into its name), rewrites the request line to
`METHOD http://host/path HTTP/1.0` (left alone when it already is absolute),
drops Connection, Keep-Alive and Proxy-Connection, adds `Connection: close`
(and a Host header when there was none), passes a request body through
(Content-Length) and relays the answer until the proxy closes. One request
per connection. An HTTP/0.9 request (`GET /path`, no version) gets the body
without the response head. The proxy's own pages (for example the 503 for a
site it does not have) are passed through untouched. When the proxy cannot
be reached, or takes the request and never answers, the client gets a short
plain 502 page saying that archive mode could not reach the archive.

**FTP gateway (TCP).** Protoweb has FTP only as FTP-over-HTTP on the same
proxy, so this is a small FTP server towards the old PC that fetches
`ftp://host/path` URLs through the proxy. The host is the name that belongs
to the original destination address of the control connection.

- USER/PASS: anything is accepted (331, 230). Commands work without login too.
- SYST, PWD/XPWD, CDUP/XCUP, TYPE (A, I, L accepted, no conversion is done),
  MODE S, STRU F, NOOP, QUIT, FEAT (211, none), REST 0, ABOR.
- CWD/XCWD **verifies**: the directory URL is fetched and only a 200 changes
  the directory (the proxy answers 404 for a missing directory and for a
  file with a slash appended). This matters because browsers use CWD to tell
  a directory from a file. Only the status line is read; `CWD /` and CDUP
  are answered without asking.
- LIST/NLST: the HTML listing of the proxy is turned into `ls -l` lines
  (`drwxr-xr-x` for entries whose link ends in "/", `-rw-r--r--` otherwise;
  the size column "1.2K"/"3.4M"/"825"/"-" becomes octets, approximately;
  the date becomes `Mon DD HH:MM` when it is less than six months old by the
  box's clock, else `Mon DD  YYYY`). The banner and the link to the parent
  directory are not listed. ls options (`-la`) are ignored.
- RETR: the file is fetched and streamed; any status but 200 is a 550. When
  the proxy gave a Content-Length and fewer octets arrived, the transfer
  ends with 426 instead of 226.
- SIZE: one HEAD request; 213 with the proxy's Content-Length (the exact
  size, unlike the rounded one in a listing), else 550.
- PASV: listens on a port of the configured range and answers 227 with the
  ORIGINAL destination address of the control connection (the address the
  client believes it is talking to) and that port; the kernel redirect
  brings the data connection back. Only the control connection's peer may
  connect to it.
- PORT: connects out to the client; an address other than the control
  connection's peer is refused.
- STOR, STOU, APPE, DELE, MKD, RMD, RNFR, RNTO: 550 "Read only archive".
  Everything else: 502.
- While a fetch is running only ABOR and QUIT are acted on; other commands
  wait until it is over.

## Command line

    v90arch [-x proxyhost:port] [-d dnsport] [-w httpport] [-f ftpport]
            [-p pasvlo-pasvhi] [-n pool_base/prefix] [-b bind_address]
            [-t idle_seconds] [-r] [-v]

| option | default | meaning |
|--------|---------|---------|
| -x | wayback.protoweb.org:7851 | the proxy (alternate: wayback2.protoweb.org:7851). A numeric address is used as it is and no name lookup is ever made. |
| -d | 8053 | UDP port of the DNS responder |
| -w | 8080 | TCP port of the HTTP relay |
| -f | 8021 | TCP port of the FTP gateway (control) |
| -p | 50000-50015 | passive data ports |
| -n | 198.18.0.0/15 | address pool; prefix 8 to 22 (the pool must be larger than the name table) |
| -b | 0.0.0.0 | address all listeners bind to |
| -t | 120 | idle time in seconds after which a session is dropped |
| -r | off | only connections the kernel redirected here (a caller's port 80 or 21, nat.c): one made to `-w` or `-f` itself, from the LAN, is closed at once. v90d starts the gateway with it. |
| -v | off | one line per session on stderr, plus start and stop |

Exit status 0 after SIGTERM or SIGINT, 1 when a listener cannot be opened,
2 for a bad command line. A proxy name that does not resolve at startup is
not an error (the line to the Internet may come up later): it is tried
again when the first session needs it.

## What the integration has to provide

Redirect the caller's traffic (the PPP interface, here `ppp0`) to the local
ports. The passive range must be redirected WITHOUT changing the port,
because the 227 reply names the port v90arch listens on:

    table ip v90arch {
        chain prerouting {
            type nat hook prerouting priority dstnat;
            iifname "ppp0" udp dport 53 redirect to :8053
            iifname "ppp0" tcp dport 80 redirect to :8080
            iifname "ppp0" tcp dport 21 redirect to :8021
            iifname "ppp0" tcp dport 50000-50015 redirect
        }
    }

(iptables: `-t nat -A PREROUTING -i ppp0 -p tcp --dport 80 -j REDIRECT
--to-ports 8080` and so on.) This is an untested sketch, the rules were not
tried on the box. Needed in the kernel: conntrack and NAT with the redirect
target (nf_conntrack, nf_nat, nft_redirect or xt_REDIRECT). The original
destination is read with getsockopt(SOL_IP, SO_ORIGINAL_DST), which
conntrack provides; when that fails the local address of the connection is
used, which is what happens in the tests.

Also worth doing:

- Let the input filter accept the four destinations above from the caller.
- Reject (TCP reset / ICMP) everything else the caller sends to the pool
  (`ip daddr 198.18.0.0/15`), HTTPS in particular. Every name resolves to a
  pool address in archive mode, so mail, IRC, HTTPS and the like would
  otherwise be forwarded to addresses that lead nowhere and hang until they
  time out.
- The box itself needs a working resolver for the proxy name (or give `-x`
  a numeric address). The box's own DNS traffic must not be redirected:
  redirect only what comes in from the caller.
- When archive mode is switched off, stop v90arch and remove the rules.
  Names looked up meanwhile stay cached in the PC (TTL 60 s, but old
  browsers keep addresses much longer): restart the browser.

## Limits

- 12 TCP sessions at a time (an FTP session with its data and proxy
  connections is one). More are refused: HTTP gets a 503 page, FTP a 421,
  written once before the close (a client that already sent its request
  may see a reset instead).
- Request head at most 8 kB (400 beyond that). FTP command line at most
  511 characters, paths at most 255, listing entries with names over 255
  characters or HTML lines over 1024 characters are skipped.
- 512 names in the DNS table; names up to 127 characters.
- Timeouts: 120 s without any movement drops a session (FTP: 421); an HTTP
  request head must be whole 30 s after the connect (or the idle time, if
  shorter), however it trickles in; 20 s for the connect to the proxy; 30 s
  waiting for the client to close after an HTTP answer was sent. Checked
  once a second.
- Sessions: 12 at a time, at most 8 of them from one caller, so a browser
  on one line cannot take the other line's. More are answered 503 (FTP 421).
- A request body is passed on by its Content-Length only: one sent chunked
  (no browser of the time does) is answered 411.
- FTP: an ABOR or QUIT is taken at once even behind other commands that wait
  for the transfer. A RETR or LIST that fails before its 150 (not in the
  archive, no proxy) leaves the PASV or PORT for the next one, unless the
  client had already made the data connection.
- The proxy name is resolved with getaddrinfo at startup and, after a
  failed connect, again at most once a minute, in a child process (the
  loop polls its pipe): a slow resolver holds up no session. With a
  numeric `-x` no lookup is made.
- Memory: about 365 kB of static data (12 sessions of 22.5 kB, 72 kB name
  table), touched only as sessions and names come into use; no heap. Socket
  buffers are asked to be 16 kB (send towards the caller, receive from the
  proxy) so that a fast proxy cannot queue megabytes in the kernel for a
  slow line.
- CPU: with no session open the process sleeps in poll() without a timeout
  (no wakeups at all); with sessions it wakes once a second plus once per
  chunk of data. A DNS query costs one pass over the 512 entry table.

## Not handled

- HTTPS and CONNECT (the proxy has neither). Port 443 is not redirected;
  see the reject rule above.
- Uploads over FTP, FTP restart (REST other than 0), EPSV/EPRT, MDTM, IPv6.
- FTP or HTTP by IP address without a DNS query first: the address is not
  in the table, so the archive is asked for the dotted address, which it
  will not know. The same happens when the name has fallen out of the table.
- FTP data in active mode comes from the box's own address and an arbitrary
  port, not from the "server" address port 20. Clients of the time do not
  check; a client-side firewall might.
- Keep-alive and pipelining: one request per connection, the answer is
  HTTP/1.0 and the connection is closed.
- Request bodies without Content-Length (chunked uploads) are not passed.
- The proxy's response heads use bare LF line ends; they are passed as they
  are (the FTP side copes with both forms).
- ASCII mode conversion: TYPE A is accepted but files are sent as they are.
- DNS over TCP, EDNS (an OPT record is ignored), queries with more than
  one question.
- Listing sizes are rounded by the proxy ("1.2K"), so the size in LIST
  differs from the real one; SIZE is exact.
- The original owner, group and permissions of archived files are not
  known; every entry is shown as `ftp ftp`, directories 755, files 644.

## Files

    v90arch.c     the loop: sockets, sessions, timeouts
    dns.c/.h      name table and DNS packet responder
    http.c/.h     request head parsing and rewriting, response head
    listing.c/.h  HTML listing -> ls lines
    ftp.c/.h      FTP command state machine
    test/         unit tests, e2e.py, two listing pages saved from the proxy

## Build and test

    make v90arch       host binary, build/v90arch
    make v90arch-arm   static, -Os, stripped: build/v90arch-arm
    make test          unit tests + end-to-end test (host)
    make test-arm      unit tests cross-built, under qemu
    make test-arm-e2e  the end-to-end test against the ARM binary under qemu

The unit tests need no sockets: DNS packets in and out and the table
behaviour, the request head rewrite, the listing conversion on the two real
pages, the FTP state machine fed with lines. The end-to-end test
(`test/e2e.py`, python3) starts the real program on high ports against a
fake proxy inside the script and drives real sockets: DNS over UDP, HTTP
fetches, an FTP session with PASV and PORT, LIST, NLST, RETR, SIZE, ABOR,
the session limit, the proxy going away, timeouts (a second run with
`-t 1`), SIGTERM. It never leaves 127.0.0.0/8: the pool is set to
127.64.0.0/16, where every address is local on Linux, so a connection to a
pool address reaches v90arch with that address as its local address and the
name lookup by address works without any redirect rule.
