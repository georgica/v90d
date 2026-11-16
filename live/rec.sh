#!/bin/sh
# rec.sh NAME AT-init [seconds]: one recorded PPP attempt; fetches the recording and the box log
cd "$(dirname "$0")/.." || exit 1
N=$1; BOX=${BOX:-10.42.90.240}
echo 'rm -f /tmp/cap.raw' | TSH_HOST=$BOX TSH_PORT=23 python3 fw/tools/tsh.py 2 >/dev/null
UPWAIT=${3:-60} CT=${3:-60} ./live/pppup.sh "$2"; UP=$?
[ $UP = 0 ] && { ping -c 3 -i 1 -W 8 -I ppp0 10.64.64.1 | tail -2; }
./live/pppdown.sh; cp live/last.ppp.txt live/$N.ppp.txt
sleep 2; (timeout 40 nc -l 9098 > live/$N.raw &) ; sleep 1
echo 'nc 10.42.90.1 9098 < /tmp/cap.raw; grep answered /tmp/v90d.log' | TSH_HOST=$BOX TSH_PORT=23 python3 fw/tools/tsh.py 14 | grep "v90d info" | tail -1 | cut -c1-110
command ls -l live/$N.raw | awk '{print $5, "octets recorded"}'
