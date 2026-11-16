#!/bin/sh
# Print the list of the bulletin board number (srv/bbs.h), made from the
# Telnet BBS Guide's own archive, fetched from the guide as it is:
# https://www.telnetbbsguide.com (Lists, Download). The daemon runs this
# once a day on a box that uses the number (srv/bbslink.c) and checks what
# it prints before it believes it.
#
# The guide's archives are ibbsMMDDYY.zip (the list as of that day) and
# ibbsMMYY.zip (the month's official one). Today's is tried first, then
# this month's, then last month's for the first days of a month.
base=${BBS_GUIDE:-https://www.telnetbbsguide.com/bbslist}
zip=${BBS_TMP:-/tmp/v90d.bbs.zip}
conv=${BBS_AWK:-/etc/bbsguide.awk}
now=${BBS_NOW:-$(date +%Y-%m-%d)}
y=${now%%-*}; rest=${now#*-}; m=${rest%%-*}; d=${rest#*-}
[ "$y" -ge 2026 ] 2>/dev/null || exit 1             # no clock yet: no file name
yy=${y#??}
pm=$((1${m} - 101)); py=$y
[ $pm -eq 0 ] && { pm=12; py=$((y - 1)); }
last=$(printf '%02d%s' $pm "${py#??}")
trap 'rm -f "$zip"' EXIT
rm -f "$zip"; umask 077
for n in "ibbs$m$d$yy" "ibbs$m$yy" "ibbs$last"; do
    wget -q -T 20 -O - "$base/$n.zip" 2>/dev/null | head -c 4194304 > "$zip"
    unzip -p "$zip" dialdirectory.xml > /dev/null 2>&1 || continue
    unzip -p "$zip" dialdirectory.xml | awk -f "$conv" | head -c 262144
    exit 0
done
exit 1
