#!/bin/sh
# Start an image on the HT802 from RAM, out of its running stock firmware
# (1.0.65), without touching the flash and without a serial cable.
#   fw/tools/ramboot.sh [build/v90d.zimage]
# Needs the stock firmware's root shell on port 2323 (HARDWARE.md). The
# watchdog is armed at 100 s before the jump: an image that does not feed it
# ends in a reset and the stock firmware is back.
set -e
fw=$(cd "$(dirname "$0")/.." && pwd)
img=${1:-$fw/build/v90d.zimage}
me=${RAMBOOT_ME:-192.168.1.45}
tsh="python3 $fw/tools/tsh.py"
size=$(stat -c %s "$img")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

cat > "$tmp/go.sh" <<'EOF'
#!/bin/sh
cd /tmp
sleep 2
ifconfig eth0 down
./peek -w 05f00000 0
WDT./peek -W 05700000 2
WDT./peek -W 05700004 abcd
WDT./peek -W 05700004 abcd
sync
insmod kjump.ko
EOF

# RAMBOOT_NOWD=1: the watchdog is left alone (for an image that starts it
# itself, like the loader; one that then hangs needs the power plug)
if [ -n "$RAMBOOT_NOWD" ]; then sed -i '/^WDT/d' "$tmp/go.sh"; else sed -i 's/^WDT//' "$tmp/go.sh"; fi

serve() { (timeout 90 nc -l "$1" < "$2" >/dev/null 2>&1 &); }
serve 9091 "$fw/tools/peek.arm"
serve 9092 "$fw/tools/kjump/kjump.ko"
serve 9094 "$tmp/go.sh"
serve 9095 "$img"
sleep 1

echo "== tools over, telephony stopped"
printf '%s\n' 'cd /tmp' \
    "nc $me 9091 > peek" "nc $me 9092 > kjump.ko" "nc $me 9094 > go.sh" \
    'chmod +x peek go.sh' \
    'killall -9 gs_ata app_dsp gs_cpe 2>/dev/null' 'sleep 1' \
    './peek -w 05300050 00040022' './peek -w 05300058 ced00b6e' \
    'ls -l peek kjump.ko go.sh' 'ps | grep -c "gs_ata\|app_dsp\|gs_cpe"' | $tsh 12 | grep -v TSH | tail -6
echo "== image over ($size bytes)"
printf '%s\n' 'cd /tmp' "nc $me 9095 | ./peek -l c9800000" "./peek -s c9800000 $size" | $tsh 25 | grep -v TSH | tail -3
echo "   sum here: $(python3 -c "import sys;print(sum(open(sys.argv[1],'rb').read()))" "$img") (decimal)"
[ -n "$RAMBOOT_DRY" ] && exit 0
echo "== jump"
# the stock busybox has no setsid: the session stays open until the jump
printf '%s\n' 'cd /tmp' 'sh ./go.sh >/dev/null 2>&1 < /dev/null &' | $tsh 8 >/dev/null 2>&1 || true
echo "   telnet on port 23 in about 20 s if the image is a debug one"
