#!/bin/sh
# v34x.sh N NAME: build daemon and replay with -DV34X=N, push the daemon (RAM), record one V.34 call, replay it
cd "$(dirname "$0")/.." || exit 1
N=$1; NAME=$2
F="-std=c99 -O2 -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200809L -DV34X=$N"
touch modem/v34ans.c
make -s -C srv v90d-arm CFLAGS="$F" || exit 1
(cd srv && make -s -f ../live/replay.mk ../live/replay CFLAGS="$F") || exit 1
./live/push.sh | tail -1
./live/rec.sh $NAME 'AT+MS=V34,0' 45
./live/replay live/box.conf live/$NAME.raw > live/$NAME.replay.txt 2>&1
grep -E "caller's MP|V.42 state|rates tx" live/$NAME.replay.txt | head -12
python3 live/hdlc.py live/data_rx.txt 2>/dev/null | head -5
