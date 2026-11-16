#!/bin/sh
# bench only: build the daemon with -DV92D_PROBE=$1, push it, record one V.92 call as live/$2, rebuild the replay to match
cd "$(dirname "$0")/.." || exit 1
P="-std=c99 -O2 -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200809L ${1:+-DV92D_PROBE=$1}"
touch v92/v92_p34d.c
make -s -C srv v90d-arm CFLAGS="$P" || exit 1
./live/push.sh | tail -1
timeout 200 ./live/rec.sh "$2" "${3:-AT+PIG=0}" 70 2>&1 | grep "call to\|PPP\|packets"
(cd srv && make -s -f ../live/replay.mk ../live/replay CFLAGS="$P") || exit 1
