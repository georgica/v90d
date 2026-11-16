#!/bin/sh
# push the freshly built daemon to the box (RAM only) and restart it
cd "$(dirname "$0")/.." || exit 1
BOX=${BOX:-10.42.90.240}
(timeout 40 nc -l 9097 < srv/build/v90d-arm &) ; sleep 1
echo 'sleep 12 | nc 10.42.90.1 9097 > /usr/sbin/v90d.new; md5sum /usr/sbin/v90d.new; chmod 755 /usr/sbin/v90d.new; mv /usr/sbin/v90d.new /usr/sbin/v90d.bin; rm -f /tmp/cap.raw; : > /tmp/v90d.log; kill $(pidof v90d.bin) $(pidof v90d) 2>/dev/null; sleep 3; ps | grep "[v]90d.bin" | wc -l' | TSH_HOST=$BOX TSH_PORT=23 python3 fw/tools/tsh.py 22 | tail -4
md5sum srv/build/v90d-arm
