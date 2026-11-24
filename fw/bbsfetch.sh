#!/bin/sh
# Print a list from Netstalgia: by default the list of the bulletin board
# number (srv/bbs.h); with a URL and the list's first line as arguments,
# another (the numbers of services, srv/svc.h). The daemon runs this once a
# day (srv/bbslink.c), then takes what it prints only when Netstalgia signed
# it (srv/bbssig.h); otherwise the box keeps the list it has (at first the
# one in its image).
#
# Netstalgia (https://netstalgia.online) serves the Telnet BBS Guide's
# list in our format, checked and refreshed daily, with its signature as
# the last line. Plain http: the signature is what is trusted, not the
# connection. BBS_API names another place for the default list (tests).
api=${1:-${BBS_API:-http://api.netstalgia.online/v1/bbs.txt}}
want=${2:-v90d-bbs 2}
tmp=${BBS_TMP:-/tmp/v90d.${api##*/}.dl}       # one each: the two lists may be fetched at once
trap 'rm -f "$tmp"' EXIT
rm -f "$tmp"; umask 077
wget -q -T 20 -O - "$api" 2>/dev/null | head -c 262144 > "$tmp"
# an error or challenge page is no list: no need to check its signature
[ "$(grep -v -e '^#' -e '^$' "$tmp" 2>/dev/null | head -n 1)" = "$want" ] || exit 1
cat "$tmp"
