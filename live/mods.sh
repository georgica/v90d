#!/bin/sh
# one PPP call per modulation, forced in the calling modem
cd "$(dirname "$0")/.." || exit 1
for m in ${MODS:-V34 V32B V22B V22 V21}; do
  echo "=== $m"
  if UPWAIT=150 CT=120 ./live/pppup.sh "AT+MS=$m,0"; then
    ping -c 4 -i 1 -W 8 -I ppp0 10.64.64.1 | tail -2
    ping -c 3 -i 1 -W 8 -I ppp0 1.1.1.1 | tail -2
    curl -s -m4 -u admin:admin http://10.42.90.240/status.json | head -c 215; echo
  fi
  ./live/pppdown.sh
  echo 'tail -1 /tmp/v90d.log' | TSH_HOST=10.42.90.240 TSH_PORT=23 python3 fw/tools/tsh.py 3 | grep "v90d info"
  sleep 4
done
