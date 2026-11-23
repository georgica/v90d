#!/bin/sh
# pppmppe.sh NAME [extra AT command]: dial with pppd requiring MS-CHAPv2 and
# 128-bit MPPE, test the link, hang up. The box must run a v90d with
# [ppp] encryption = on for the port dialled (the Modem page, "PPP encryption").
# Keeps: live/NAME.ppp.txt (pppd log), prints the summary.
cd "$(dirname "$0")/.." || exit 1
N=$1; INIT=${2:-AT}; BOX=${BOX:-192.168.1.172}; L=live/$N.ppp.txt; : > $L
sudo -n modprobe ppp_mppe 2>/dev/null
# require-mppe-128 makes pppd require 128-bit MPPE, which forces MS-CHAPv2.
sudo -n pppd /dev/ttyACM0 115200 crtscts noauth nodefaultroute nodetach debug noipv6 user guest password guest \
  require-mppe-128 refuse-pap refuse-chap refuse-eap \
  lcp-echo-interval 0 logfile $L \
  connect "chat -v -t 75 '' ATZ OK ATE0 OK ATX4W2 OK '$INIT' OK '\\d\\dATDT${NUM:-6387825442}' CONNECT" > /dev/null 2>&1 &
i=0; while [ $i -lt 90 ]; do ip -4 addr show ppp0 2>/dev/null | grep -q inet && break; sleep 1; i=$((i+1)); done
if ip -4 addr show ppp0 2>/dev/null | grep -q inet; then
  echo "PPP up after about $i s: $(ip -4 -br addr show ppp0)"
  ping -c 5 -i 0.5 -W 3 -I ppp0 10.64.64.1 | tail -2
  ping -c 10 -i 0.5 -W 3 -I ppp0 1.1.1.1 | tail -2
  curl -s -m4 -u admin:admin http://$BOX/status.json | head -c 400; echo
else echo "PPP did not come up"; fi
sudo -n pkill -x pppd; sleep 3
# the lines that prove MPPE: MS-CHAPv2 chosen, MPPE negotiated, and traffic encrypted
grep -E "CONNECT|CARRIER|BUSY|CHAP|MS-CHAP|MPPE|mppe|CCP|Compression|rcvd \[IPCP ConfAck|local  IP|remote IP|terminat|Failed|failed" $L | head -20
