#!/bin/sh
# call.sh NAME [AT-setup ...]: dial port 1's number, keep the result, fetch the recording and the log
cd "$(dirname "$0")/.." || exit 1
BOX=${BOX:-10.42.90.240}; N=$1; shift
python3 live/at.py -t ${WAIT:-60} ATZ ATE0 ATX4 "ATS7=${WAIT:-60}" "$@" ATDT${NUM:-5551234} > live/$N.modem.txt 2>&1
python3 live/at.py -t 2 '+++' >/dev/null 2>&1; python3 live/at.py ATH0 ATI6 >> live/$N.modem.txt 2>&1
sleep 2; (timeout 30 nc -l 9098 > live/$N.raw &) ; sleep 1
echo 'nc 10.42.90.1 9098 < /tmp/cap.raw; rm -f /tmp/cap.raw; tail -6 /tmp/v90d.log' | TSH_HOST=$BOX TSH_PORT=23 python3 fw/tools/tsh.py 10 | grep "v90d" > live/$N.box.txt
cat live/$N.modem.txt | tail -${TAIL:-25}; cat live/$N.box.txt
