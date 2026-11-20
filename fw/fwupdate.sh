#!/bin/sh
# Install the v90d release named by $1 (a version like 0.6.0): download our own
# release image and hand it to the flash tool, which writes the inactive copy;
# then ask the init script to restart into it. The daemon runs this for the
# Firmware page's Install button, only after the owner clicked, and only with a
# version it found on GitHub itself. Only our own release URL is fetched, and
# the version is checked here too. Overridable in the daemon with -U (tests).
set -e
ver="$1"
repo="${V90D_REPO:-georgica/v90d}"
v90nv="${V90NV:-/usr/sbin/v90nv}"
case "$ver" in
    '' | *[!0-9.]*) echo "fwupdate: bad version '$ver'" >&2; exit 2 ;;
esac
url="https://github.com/$repo/releases/download/v$ver/v90d-ht802fw.bin"
tmp="/tmp/v90d-update.bin"
rm -f "$tmp"
echo "fwupdate: fetching $url"
wget -q -T 60 -O "$tmp" "$url"
echo "fwupdate: writing the flash"
"$v90nv" upgrade "$tmp"
rm -f "$tmp"
# the same note the Restart button leaves; fw/rcS sees it and reboots into
# the copy just written.
: > /tmp/v90d.restart
echo "fwupdate: done, restarting"
