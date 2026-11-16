#!/bin/sh
# tput2.sh FILE: one call on the modem, download FILE.bin from the box, sample the box's CPU and V.42 counters meanwhile
cd "$(dirname "$0")/.." || exit 1
sudo -n iptables -I OUTPUT -o ppp0 -d 224.0.0.0/4 -j DROP
if ./live/pppup.sh "${2:-AT}"; then
  (sleep 8; echo 'P=$(pidof v90d); A=$(head -1 /proc/stat); B=$(cut -d" " -f14,15 /proc/$P/stat); sleep 6; echo "S1 $A"; echo "S2 $(head -1 /proc/stat)"; echo "D1 $B"; echo "D2 $(cut -d" " -f14,15 /proc/$P/stat)"' | TSH_HOST=10.42.90.240 TSH_PORT=23 python3 fw/tools/tsh.py 9 > live/tput2.top.txt) &
  curl -s -m 180 --interface ppp0 -o /dev/null -w "$1: %{size_download} octets in %{time_total} s = %{speed_download} octets/s\n" http://10.64.64.1:8088/$1.bin
  wait
  python3 - <<'PY'
import re
t=open('live/tput2.top.txt').read()
g=lambda k:[int(x) for x in re.search(r'^'+k+r'\s+(?:cpu\s+)?([\d ]+)',t,re.M).group(1).split()]
s1,s2,d1,d2=g('S1'),g('S2'),g('D1'),g('D2')
tot=sum(s2)-sum(s1); idle=s2[3]-s1[3]
print("box over 6 s: %.0f %% busy (user %.0f %%, system %.0f %%, softirq %.0f %%); daemon %.0f %% (user %.0f, system %.0f)" % (100*(tot-idle)/tot, 100*(s2[0]-s1[0])/tot, 100*(s2[2]-s1[2])/tot, 100*(s2[6]-s1[6])/tot, 100*(sum(d2)-sum(d1))/tot, 100*(d2[0]-d1[0])/tot, 100*(d2[1]-d1[1])/tot))
PY
fi
./live/pppdown.sh
sudo -n iptables -D OUTPUT -o ppp0 -d 224.0.0.0/4 -j DROP
