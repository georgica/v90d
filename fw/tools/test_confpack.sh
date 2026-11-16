#!/bin/sh
# The settings and the VPN profile travel to the flash as one file
# (fw/confpack.sh): packed and split again they are what they were, and a
# packed file is still a settings file to a firmware that knows no profile.
#   tools/test_confpack.sh [SHELL]     (default sh; give "busybox sh" too)
SH=${1:-sh}; here=$(cd "$(dirname "$0")" && pwd); d=$(mktemp -d); fail=0; n=0
ok() { n=$((n+1)); if ! eval "$1"; then echo "  FAIL: $1"; fail=$((fail+1)); fi; }
run() { CONF=$d/conf VPN=$d/vpn $SH -c ". $here/../confpack.sh; $1"; }

printf '[line]\nnumber = 5551234\n# a comment\n\n[status]\nport = 80\n' > $d/conf.orig
printf 'client\r\nremote vpn.example 1194\n\n<ca>\n-----BEGIN CERTIFICATE-----\nab+/cd==\n</ca>\n#%%vpn odd line\n  indented\n' > $d/vpn.orig

# with a profile
cp $d/conf.orig $d/conf; cp $d/vpn.orig $d/vpn
run "conf_pack > $d/all"
ok "grep -q '^number = 5551234' $d/all"
ok "[ \$(grep -vc '^#' $d/all) -eq \$(grep -vc '^#' $d/conf.orig) ]"      # nothing but comments was added
rm $d/conf $d/vpn; cp $d/all $d/conf
run "conf_split"
ok "cmp -s $d/conf $d/conf.orig"
ok "cmp -s $d/vpn $d/vpn.orig"
ok "[ \$(stat -c %a $d/vpn) = 600 ]"
# packed again: the same file
run "conf_pack > $d/all2"; ok "cmp -s $d/all $d/all2"

# without one: the settings as they are, and no profile appears
cp $d/conf.orig $d/conf; rm -f $d/vpn
run "conf_pack > $d/all"
ok "cmp -s $d/all $d/conf.orig"
run "conf_split"
ok "cmp -s $d/conf $d/conf.orig"; ok "[ ! -e $d/vpn ]"
# an empty profile file counts as none
: > $d/vpn; run "conf_pack > $d/all"; ok "cmp -s $d/all $d/conf.orig"
# a profile that was there is gone after a split of settings without one
cp $d/vpn.orig $d/vpn; cp $d/conf.orig $d/conf; run "conf_split"; ok "[ ! -e $d/vpn ]"

rm -rf $d
echo "test_confpack ($SH): $n checks, $fail failures"
[ $fail -eq 0 ]
