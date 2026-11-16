#!/bin/sh
# pwr.sh CODE: build the daemon with INFO1d maximum power code CODE, push it, make V.90 calls and print the rates
cd "$(dirname "$0")/.." || exit 1
rm -f srv/build/v90d-arm
make -s -C srv v90d-arm CFLAGS="-std=c99 -O2 -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200809L -DV90_MAX_TX_CODE=$1" || exit 1
./live/push.sh > /dev/null
for n in 1 2; do
  if ./live/pppup.sh AT > /dev/null; then sleep 4; ping -c 5 -i 0.4 -I ppp0 10.64.64.1 | tail -2 | head -1; fi
  ./live/pppdown.sh; sleep 4
  echo 'grep "call to" /tmp/v90d.log | tail -1' | TSH_HOST=10.42.90.240 TSH_PORT=23 python3 fw/tools/tsh.py 3 | grep "call to"
done
