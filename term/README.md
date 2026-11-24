# term: the terminal main menu

Netstalgia's terminal main menu (approved 2026-11-24), shared by v90d's
daemon (`srv/termsvc.c`) and Netstalgia's own dial-in (nsdiald). Portable
C99, no allocation, no globals, no input or output of its own.

| file | what |
|---|---|
| `tscr.h` | 80x25 CP437 cells in the 16 CGA colours, half-block pixels; to the line as ANSI (whole, or only what changed) or plain ASCII |
| `tart.h` | the planet with its ring, the NETSTALGIA wordmark |
| `guide.h` | the Telnet BBS Guide's `full_<Mon>_<YY>.txt`, and the board details list `v90d-bbsinfo 1` |
| `tdir.h` | the BBS directory: the list of boards with those details, sorted, a search per caller done a slice at a time |
| `tmenu.h` | the screens and keys; the owner feeds it octets and does what it asks (start PPP, connect to a board, hang up) |
| `bbsinfo.c` | the `v90d-bbsinfo` tool |

## The board details list

A v90d box fetches Netstalgia's list of boards (`srv/bbs.h`), which has a
login, a number, a host and a name for each. The directory's board pages
also show where a board is, its software and nodes, what a new user logs in
as, its website and the guide's description. Those come from a second list,
signed the same way (`srv/bbssig.h`) and fetched once a day while the menu
is on: `http://api.netstalgia.online/v1/bbsinfo.txt`.

It is made from the guide's monthly file and the list of boards, joined by
host and port, with:

    make -C term build/v90d-bbsinfo
    term/build/v90d-bbsinfo full_Nov_26.txt bbs.txt > bbsinfo.txt

then given a serial and signed like the other lists. The format is in
`guide.h`; it never holds a host or a port, and a reader passes over keys it
does not know, so fields can be added without a new version. The guide is
"a free list, distribute it everywhere"; the list credits it at the top.

## Tests

    make -C term test        # host
    make -C term test-arm    # ARM, under qemu

`test/vt.h` is a small ANSI terminal: the menu tests play everything sent
into it and check that the caller sees exactly the menu's screen.
