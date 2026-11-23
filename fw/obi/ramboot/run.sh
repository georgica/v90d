#!/bin/sh
# run.sh [PAYLOAD]: one bring-up cycle on the OBi200 from this machine.
# Uploads the tools and the payload (build/payload.bin) to the box's RAM,
# RAM-boots it (ramboot.sh, detached), waits for the box to come back (the
# watchdog resets a kernel that stops), then prints what the run left:
# the shim's mark, the low-level debug page (DEBUG_LL, 0x03f02000), the
# early console (0x03f10000, 64 KiB) and pstore's console (0x03e00000).
# OBI_SSH: a command that runs its arguments on the box over ssh.
set -e
here=$(cd "$(dirname "$0")" && pwd)
P=${1:-$here/build/payload.bin}
SSH=${OBI_SSH:?set OBI_SSH to a command that runs its arguments on the box}
BOX=${OBI_BOX:-192.168.1.100}
$SSH 'mkdir -p /tmp/rb'
for f in build/kjump build/tdmoff build/physput build/stub.bin build/physdump ramboot.sh; do $SSH "cat > /tmp/rb/$(basename $f)" < "$here/$f"; done
$SSH 'cat > /tmp/rb/payload.bin' < "$P"
$SSH "rm -f /tmp/rb/no-tdmoff; ${NO_TDMOFF:+touch /tmp/rb/no-tdmoff}"
$SSH 'cp /scratch/pmem /tmp/rb/ && chmod +x /tmp/rb/*; (setsid sh /tmp/rb/ramboot.sh /tmp/rb/payload.bin /tmp/rb/stub.bin < /dev/null > /dev/null 2>&1 &)'
t0=$(date +%s)
i=0; while [ $i -lt 60 ] && ping -c1 -W1 "$BOX" >/dev/null 2>&1; do i=$((i+1)); sleep 1; done
echo "box gone after $(( $(date +%s) - t0 )) s"
i=0; until nc -z -w2 "$BOX" 22 2>/dev/null; do i=$((i+1)); [ $i -lt 150 ] || { echo "box not back"; exit 1; }; sleep 3; done
echo "box back after $(( $(date +%s) - t0 )) s"
sleep 3
$SSH 'cat > /tmp/physdump && chmod +x /tmp/physdump' < "$here/build/physdump"
$SSH '/tmp/physdump 03e00000 100000' > /tmp/obi-pstore.bin
$SSH '/tmp/physdump 03f00000 30000' > /tmp/obi-pages.bin
python3 - /tmp/obi-pages.bin /tmp/obi-pstore.bin <<'PY'
import struct, sys
pages = open(sys.argv[1], 'rb').read(); ps = open(sys.argv[2], 'rb').read()
def text(page, name):
    n = struct.unpack_from('<I', page, 4)[0]
    if not 0 < n <= len(page) - 8:
        print('--- %s: empty' % name); return
    print('--- %s (%d octets):' % (name, n)); print(page[8:8 + n].decode(errors='replace'))
print('--- shim mark: %r' % pages[0x1000:0x1004])
if pages[0x10000:0x10004] == b'NOTE': text(pages[0x10000:0x20000], 'ramboot note (before the jump)')
text(pages[0x20000:0x30000], 'kernel log (DEBUG_LL, earlyprintk)')

# pstore's console zone: after the oops records; find its persistent_ram header "DBGC"
i = ps.find(b'DBGC')
while i >= 0:
    start, size = struct.unpack_from('<II', ps, i + 4)
    if 0 < size < len(ps) - i:
        print('--- pstore zone at +0x%x (%d octets used):' % (i, size))
        print(ps[i + 12:i + 12 + size].decode(errors='replace')[-4000:])
    i = ps.find(b'DBGC', i + 4)
PY
