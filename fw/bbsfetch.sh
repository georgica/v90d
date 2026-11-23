#!/bin/sh
# Print the list of the bulletin board number (srv/bbs.h). The daemon runs
# this once a day on a box that uses the number (srv/bbslink.c) and checks
# what it prints before it believes it.
#
# First from Netstalgia (https://netstalgia.online), which serves the
# Telnet BBS Guide's list already in our format, checked and refreshed
# daily: one small file, nothing to unpack or convert. Plain http: the
# boxes' wget does not check certificates anyway, and every line is
# checked again by the daemon. BBS_API="" skips it.
#
# Then from the Telnet BBS Guide's own archive, fetched from the guide as
# it is: https://www.telnetbbsguide.com (Lists, Download). Its archives are
# ibbsMMDDYY.zip (the list as of that day) and ibbsMMYY.zip (the month's
# official one). Today's is tried first, then this month's, then last
# month's for the first days of a month. This needs unzip and awk.
api=${BBS_API-http://api.netstalgia.online/v1/bbs.txt}
base=${BBS_GUIDE:-https://www.telnetbbsguide.com/bbslist}
zip=${BBS_TMP:-/tmp/v90d.bbs.zip}
conv=${BBS_AWK:-/etc/bbsguide.awk}
now=${BBS_NOW:-$(date +%Y-%m-%d)}
trap 'rm -f "$zip"' EXIT
rm -f "$zip"; umask 077

if [ -n "$api" ]; then
    wget -q -T 20 -O - "$api" 2>/dev/null | head -c 262144 > "$zip"
    if [ "$(grep -v -e '^#' -e '^$' "$zip" 2>/dev/null | head -n 1)" = "v90d-bbs 2" ]; then
        cat "$zip"
        exit 0
    fi
fi

y=${now%%-*}; rest=${now#*-}; m=${rest%%-*}; d=${rest#*-}
[ "$y" -ge 2026 ] 2>/dev/null || exit 1             # no clock yet: no file name
[ -r "$conv" ] || exit 1                            # no converter on this box
yy=${y#??}
pm=$((1${m} - 101)); py=$y
[ $pm -eq 0 ] && { pm=12; py=$((y - 1)); }
last=$(printf '%02d%s' $pm "${py#??}")
for n in "ibbs$m$d$yy" "ibbs$m$yy" "ibbs$last"; do
    wget -q -T 20 -O - "$base/$n.zip" 2>/dev/null | head -c 4194304 > "$zip"
    unzip -p "$zip" dialdirectory.xml > /dev/null 2>&1 || continue
    unzip -p "$zip" dialdirectory.xml | awk -f "$conv" | head -c 262144
    exit 0
done
exit 1
