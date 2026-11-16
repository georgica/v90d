#!/bin/sh
# tput.sh [AT-init]: one call, then timed downloads of random, HTML and text data served by the box itself
cd "$(dirname "$0")/.." || exit 1
sudo -n iptables -I OUTPUT -o ppp0 -d 224.0.0.0/4 -j DROP
if ./live/pppup.sh "${1:-AT}"; then
  curl -s -m4 -u admin:admin http://10.42.90.240/status${PORT2:+2}.json | grep -o '"modulation.*"v42":"[a-z]*","compression":[a-z]*'
  for f in random html text; do
    curl -s -m 180 --interface ppp0 -o /dev/null -w "$f: %{size_download} octets in %{time_total} s = %{speed_download} octets/s\n" http://10.64.64.1:8088/$f.bin
  done
fi
./live/pppdown.sh
sudo -n iptables -D OUTPUT -o ppp0 -d 224.0.0.0/4 -j DROP
