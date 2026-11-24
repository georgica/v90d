#!/bin/sh
# Print the list of the bulletin board number (srv/bbs.h). The daemon runs
# this once a day on a box that uses the number (srv/bbslink.c), then takes
# what it prints only when Netstalgia signed it (srv/bbssig.h); otherwise
# the box keeps the list it has (at first the one in its image).
#
# Netstalgia (https://netstalgia.online) serves the Telnet BBS Guide's
# list in our format, checked and refreshed daily, with its signature as
# the last line. Plain http: the signature is what is trusted, not the
# connection. BBS_API names another place (tests).
api=${BBS_API:-http://api.netstalgia.online/v1/bbs.txt}
tmp=${BBS_TMP:-/tmp/v90d.bbs.dl}
trap 'rm -f "$tmp"' EXIT
rm -f "$tmp"; umask 077
wget -q -T 20 -O - "$api" 2>/dev/null | head -c 262144 > "$tmp"
# an error or challenge page is no list: no need to check its signature
[ "$(grep -v -e '^#' -e '^$' "$tmp" 2>/dev/null | head -n 1)" = "v90d-bbs 2" ] || exit 1
cat "$tmp"
