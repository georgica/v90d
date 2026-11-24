#!/bin/sh
# The bulletin board list fetch (fw/bbsfetch.sh), without the network: files
# made here, served from a directory by a stand-in for wget. The signature
# is checked by the daemon (srv/bbssig.c, srv/test/test_srv.c).
set -e
fw=$(cd "$(dirname "$0")/.." && pwd)
d=$(mktemp -d); trap 'rm -rf "$d"' EXIT
fail=0
check() { if [ "$2" = "$3" ]; then :; else echo "  FAIL $1"; echo "    got:  $2"; echo "    want: $3"; fail=1; fi; }

mkdir "$d/site" "$d/bin"
cat > "$d/bin/wget" <<EOF2
#!/bin/sh
# stand-in: the last argument is the URL; serve its file name from the site directory
for a; do u=\$a; done
echo "\$u" >> "$d/asked"
[ -f "$d/site/\${u##*/}" ] && cat "$d/site/\${u##*/}"
EOF2
chmod +x "$d/bin/wget"
run() { PATH="$d/bin:$PATH" BBS_TMP="$d/tmp" sh "$fw/bbsfetch.sh"; }

list='# from Netstalgia
v90d-bbs 2
bbs telehack 2278353422 telehack.com Telehack
# sig ed25519 00'
printf '%s\n' "$list" > "$d/site/bbs.txt"
: > "$d/asked"
run > "$d/out" || { echo "  FAIL the list is there, yet failure"; fail=1; }
check "the list as it is, signature line and all" "$(cat "$d/out")" "$list"
check "asked Netstalgia" "$(cat "$d/asked")" "http://api.netstalgia.online/v1/bbs.txt"
[ ! -e "$d/tmp" ] || { echo "  FAIL the download is left behind"; fail=1; }

# a challenge or error page, or nothing at all, is no list
echo "<html>Just a moment...</html>" > "$d/site/bbs.txt"
if run > "$d/out" 2>/dev/null; then echo "  FAIL a challenge page became a list"; fail=1; fi
rm "$d/site/bbs.txt"
if run > "$d/out" 2>/dev/null; then echo "  FAIL nothing fetched, yet success"; fail=1; fi
# nothing else is ever asked for
check "only Netstalgia, ever" "$(sort -u "$d/asked")" "http://api.netstalgia.online/v1/bbs.txt"

# the numbers of services: the same, with the place and the first line given
svc='# serial 3
v90d-svc 1
svc 13855961775 nwnrevival.com:5000 raw AOL Neverwinter Nights
# sig ed25519 00'
printf '%s\n' "$svc" > "$d/site/services.txt"
: > "$d/asked"
runs() { PATH="$d/bin:$PATH" BBS_TMP="$d/tmp" sh "$fw/bbsfetch.sh" http://api.netstalgia.online/v1/services.txt 'v90d-svc 1'; }
runs > "$d/out" || { echo "  FAIL the services list is there, yet failure"; fail=1; }
check "the services list as it is" "$(cat "$d/out")" "$svc"
check "asked for the services list" "$(cat "$d/asked")" "http://api.netstalgia.online/v1/services.txt"
# a list of boards where the services list should be is not one
cp "$d/site/services.txt" "$d/keep"; printf '%s\n' "$list" > "$d/site/services.txt"
if runs > "$d/out" 2>/dev/null; then echo "  FAIL a list of boards taken for the services list"; fail=1; fi
# and the image's daemon asks for exactly this (srv/bbslink.c)
grep -q "sh /etc/bbsfetch.sh http://api.netstalgia.online/v1/services.txt 'v90d-svc 1'" "$fw/../srv/bbslink.c" \
    || { echo "  FAIL srv/bbslink.c asks for something else"; fail=1; }

[ $fail = 0 ] && echo "bbsfetch: all checks pass" || { echo "bbsfetch: FAILED"; exit 1; }
