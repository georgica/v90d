#!/bin/sh
# pppup.sh [AT-init]: bring the dial-up link up and leave it up (pppdown.sh ends it)
INIT=${1:-AT}; L=$(dirname "$0")/last.ppp.txt; : > $L
sudo -n pppd /dev/ttyACM0 ${DTE:-230400} crtscts noauth nodefaultroute nodetach debug noipv6 user guest password guest lcp-echo-interval 0 logfile $L \
  connect "chat -v -t ${CT:-75} '' ATZ OK ATE0 OK ATX4W2 OK '$INIT' OK '\\d\\dATDT${NUM:-6387825442}' CONNECT" > /dev/null 2>&1 &
i=0; while [ $i -lt ${UPWAIT:-90} ]; do ip -4 addr show ppp0 2>/dev/null | grep -q inet && { echo "PPP up after about $i s"; exit 0; }; sleep 1; i=$((i+1)); done
echo "PPP did not come up"; grep -E "CONNECT|CARRIER|BUSY|Failed|failed|terminat" $L | tail -3; exit 1
