#!/bin/sh
# ppp.sh NAME [extra AT command]: dial with pppd, test the link, hang up.
# Keeps: live/NAME.ppp.txt (pppd log), prints the summary.
cd "$(dirname "$0")/.." || exit 1
N=$1; INIT=${2:-AT}; BOX=${BOX:-10.42.90.240}; L=live/$N.ppp.txt; : > $L
sudo -n pppd /dev/ttyACM0 115200 crtscts noauth nodefaultroute nodetach debug noipv6 user guest password guest \
  lcp-echo-interval 0 logfile $L \
  connect "chat -v -t 75 '' ATZ OK ATE0 OK ATX4W2 OK '$INIT' OK '\\d\\dATDT${NUM:-5551234}' CONNECT" > /dev/null 2>&1 &
i=0; while [ $i -lt 90 ]; do ip -4 addr show ppp0 2>/dev/null | grep -q inet && break; sleep 1; i=$((i+1)); done
if ip -4 addr show ppp0 2>/dev/null | grep -q inet; then
  echo "PPP up after about $i s: $(ip -4 -br addr show ppp0)"
  ping -c 5 -i 0.5 -W 3 -I ppp0 10.64.64.1 | tail -2
  ping -c 10 -i 0.5 -W 3 -I ppp0 1.1.1.1 | tail -2
  [ -n "$BULK" ] && { curl -s -m 60 --interface ppp0 -o /dev/null -w "download: %{size_download} octets at %{speed_download} octets/s\n" --resolve speedtest.tele2.net:80:${DLIP} http://speedtest.tele2.net/${DLFILE:-100KB.zip}; }
  curl -s -m4 -u admin:admin http://$BOX/status.json | head -c 330; echo
else echo "PPP did not come up"; fi
sudo -n pkill -x pppd; sleep 3
grep -E "CONNECT|CARRIER|BUSY|rcvd \[IPCP ConfAck|local  IP|remote IP|PAP|terminat|Failed|failed" $L | head -12
