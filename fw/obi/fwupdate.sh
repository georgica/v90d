#!/bin/sh
# Install the v90d release named by $1 (a version like 0.11.0) on the OBi200:
# download our own release file for this box and hand it to the flash tool
# (obinv), as the Firmware page's upload does; obinv writes what differs and
# restarts the box. The daemon runs this for the Firmware page's Install
# button, only after the owner clicked, and only with a version it found on
# GitHub itself. Only our own release URL is fetched, and the version is
# checked here too. The HT802 has fw/fwupdate.sh. Ours; written for this image.
set -e
ver="$1"
repo="${V90D_REPO:-georgica/v90d}"
obinv="${OBINV:-/obi/obinv}"
case "$ver" in
    '' | *[!0-9.]*) echo "fwupdate: bad version '$ver'" >&2; exit 2 ;;
esac
url="https://github.com/$repo/releases/download/v$ver/v90d-obi200.fw"
tmp="/tmp/v90d-update.fw"
rm -f "$tmp"
echo "fwupdate: fetching $url"
wget -q -T 60 -O "$tmp" "$url"
"$obinv" check "$tmp"
echo "fwupdate: writing the flash"
# obinv takes the file as the Firmware page's upload gives it, and restarts
# the box once it is written (OBINV_FLASH and the rest come from /obi/obi)
"$obinv" serve-upgrade "$(wc -c < "$tmp")" < "$tmp" > /dev/null
rm -f "$tmp"
echo "fwupdate: done"
