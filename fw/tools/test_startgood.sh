#!/bin/sh
# When may a start be called a good one (fw/startgood.sh)? On a board a
# unit has run: as before, the daemon alive is enough. On any other board:
# only once something has been received on the network port, so that a box
# whose network does not work goes back to the stock firmware by itself.
#   tools/test_startgood.sh [SHELL]     (default sh; give "busybox sh" too)
SH=${1:-sh}; here=$(cd "$(dirname "$0")" && pwd); d=$(mktemp -d); fail=0; n=0
ok() { n=$((n+1)); if ! eval "$1"; then echo "  FAIL: $1"; fail=$((fail+1)); fi; }
good() { SYSNET=$d/net $SH -c ". $here/../startgood.sh; start_good $d/where eth0"; }
mkdir -p $d/net/eth0/statistics

printf 'scheme=loader\nboard=9610003725A known\ntried=yes\n' > $d/where
echo 0 > $d/net/eth0/statistics/rx_packets
ok "good"                                   # the tried board: nothing asked of the network
printf 'scheme=loader\nboard=9610003611C known\ntried=no\n' > $d/where
ok "! good"                                 # untried, nothing heard yet
echo 7 > $d/net/eth0/statistics/rx_packets
ok "good"                                   # untried, the network has been heard
echo 0 > $d/net/eth0/statistics/rx_packets
printf 'scheme=loader\nboard=9610003611C known\n' > $d/where
ok "! good"                                 # no word about the board: treated as untried
rm $d/where; ok "! good"
rm -r $d/net/eth0; printf 'tried=no\n' > $d/where
ok "! good"                                 # no such interface
printf 'tried=yes\n' > $d/where; ok "good"

rm -rf $d
echo "startgood ($SH): $n checks, $fail failures"
[ $fail -eq 0 ]
