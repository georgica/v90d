#!/bin/sh
# ramboot.sh PAYLOAD STUB: on the OBi200 under its stock kernel, boot
# PAYLOAD (shim + zImage + dtb) from RAM. The telephony and the network are
# stopped first: their DMA would keep writing into memory the new kernel is
# about to use. The early console's page (0x03f00000) and the shim's mark
# (0x03f01000) are cleared, so what is there afterwards is this run's.
# Never returns; run it detached (setsid). Its own log: /tmp/ramboot.log.
PATH=/sbin:/usr/sbin:/bin:/usr/bin
here=$(dirname "$0")
exec > /tmp/ramboot.log 2>&1
echo "ramboot $1 at $(cut -d' ' -f1 /proc/uptime)"
# the launcher's loops first (they would start the line again), then the
# rest. The loops run as "/bin/sh ./obi"; this BusyBox has no awk.
for p in $(ps | grep '[/]bin/sh \./obi' | sed 's/^ *//' | cut -d' ' -f1); do kill "$p"; done
for n in obiline v90d udhcpc pppd; do killall "$n" 2>/dev/null; done
sleep 2
# obiline starts the phones' TDM and never stops it: its DMA runs on
# and the audio DMA (tdmoff: rings off, then the TDM)
[ -e /tmp/rb/no-tdmoff ] && echo "tdmoff skipped" > /tmp/tdmoff.out || "$here/tdmoff" > /tmp/tdmoff.out 2>&1
ifconfig eth1 down 2>/dev/null
ifconfig unm down 2>/dev/null
"$here/pmem" -w 03f01000 0 0 0 0
"$here/pmem" -w 03f20000 0 0 0 0
# a note for run.sh (0x03f10000): is anything still interrupting (= DMA)?
{ cat /tmp/tdmoff.out; ps; grep -E '^ *[0-9]+:' /proc/interrupts; sleep 2; echo "2 s later:"
  grep -E '^ *[0-9]+:' /proc/interrupts; } 2>&1 | "$here/physput" 03f10000
sync
"$here/kjump" "$1" "$2"
echo "kjump returned"
