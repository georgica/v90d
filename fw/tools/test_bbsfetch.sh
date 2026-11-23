#!/bin/sh
# The bulletin board list (fw/bbsfetch.sh): from Netstalgia's API first,
# then from the Telnet BBS Guide's archive (fw/bbsguide.awk), without the
# network: files made here, served from a directory by a stand-in for wget.
set -e
fw=$(cd "$(dirname "$0")/.." && pwd)
d=$(mktemp -d); trap 'rm -rf "$d"' EXIT
fail=0
check() { if [ "$2" = "$3" ]; then :; else echo "  FAIL $1"; echo "    got:  $2"; echo "    want: $3"; fail=1; fi; }

cat > "$d/dialdirectory.xml" <<'XML'
<?xml version="1.0" ?>
<EtherTerm>
    <Phonebook version="1.0">
        <BBS name="20 For Beers BBS" ip="20forbeers.com" port="1337" protocol="TELNET" login="" password="" font="vga8x16.bmp" keyMap="ANSI" />
        <BBS name="20 For Beers BBS (ssh)" ip="20forbeers.com" port="1338" protocol="ssh" login="" password="" />
        <BBS name="4 Wheel Ham BBS" ip="bbs.4wheelham.com" port="23" protocol="TELNET" login="" password="" />
        <BBS name="Air & Wave BBS" ip="bbs.airandwave.net" port="2323" protocol="TELNET" login="" password="" />
        <BBS name="Atari BBS Gateway" ip="ataribbs.ddns.net" port="6502" protocol="TELNET" login="" password="" />
        <BBS name="A-Net Online ANetBBS" ip="bbs.a-net.fyi" port="2233" protocol="TELNET" login="" password="" />
        <BBS name="Diskbox ][" ip="tag.diskbox2.net" port="23" protocol="TELNET" login="" password="" />
        <BBS name="Dream Land BBS" ip="ccns.cc" port="3456" protocol="TELNET" login="" password="" />
        <BBS name="Dreamland BBS" ip="75.82.21.80 " port="23" protocol="TELNET" login="" password="" />
        <BBS name="Bad Host" ip="-oProxy.example;x" port="23" protocol="TELNET" login="" password="" />
        <BBS name="Bad Port" ip="ok.example" port="99999" protocol="TELNET" login="" password="" />
        <BBS name="!!!" ip="ok.example" port="23" protocol="TELNET" login="" password="" />
        <BBS name="A Very Long Name Of A Board That Goes On And On And On" ip="long.example" port="23" protocol="TELNET" login="" password="" />
        <BBS name="ZZ" ip="short.example" port="23" protocol="TELNET" login="" password="" />
    </Phonebook>
</EtherTerm>
XML
want='v90d-bbs 2
bbs 20forbeers 2272036723 20forbeers.com:1337 20 For Beers BBS
bbs 4wheelham 2274943354 bbs.4wheelham.com 4 Wheel Ham BBS
bbs airwave 2272479283 bbs.airandwave.net:2323 Air & Wave BBS
bbs atarigateway 2272827442 ataribbs.ddns.net:6502 Atari BBS Gateway
bbs anetonlineanetbbs 2272638665 bbs.a-net.fyi:2233 A-Net Online ANetBBS
bbs diskboxii 2273475269 tag.diskbox2.net Diskbox ][
bbs dreamland 2273732652 ccns.cc:3456 Dream Land BBS
bbs dreamland2 2273732653 75.82.21.80 Dreamland BBS
bbs averylongnameofaboardthatgoesona 2272837956 long.example A Very Long Name Of A Board That Goes On And On
bbs zz 2279900000 short.example ZZ'
check "the guide's directory as our list" "$(awk -f "$fw/bbsguide.awk" "$d/dialdirectory.xml")" "$want"

# the archive names: today's, this month's, last month's; the first that is there wins
mkdir "$d/site" "$d/bin"
(cd "$d" && zip -q site/monthly.zip dialdirectory.xml)
cat > "$d/bin/wget" <<EOF2
#!/bin/sh
# stand-in: the last argument is the URL; serve its file name from the site directory
for a; do u=\$a; done
echo "\${u##*/}" >> "$d/asked"
[ -f "$d/site/\${u##*/}" ] && cat "$d/site/\${u##*/}"
EOF2
chmod +x "$d/bin/wget"
run() { PATH="$d/bin:$PATH" BBS_TMP="$d/tmp.zip" BBS_AWK="$fw/bbsguide.awk" BBS_NOW="$1" sh "$fw/bbsfetch.sh"; }
: > "$d/asked"
if run 2026-11-16 > "$d/out" 2>/dev/null; then echo "  FAIL nothing to fetch, yet success"; fail=1; fi
check "the names asked for" "$(tr '\n' ' ' < "$d/asked")" "bbs.txt ibbs111626.zip ibbs1126.zip ibbs1026.zip "
cp "$d/site/monthly.zip" "$d/site/ibbs1126.zip"
: > "$d/asked"
run 2026-11-16 > "$d/out" || { echo "  FAIL the monthly archive is there, yet failure"; fail=1; }
check "the list from the monthly archive" "$(cat "$d/out")" "$want"
check "stopped at the first that is there" "$(tr '\n' ' ' < "$d/asked")" "bbs.txt ibbs111626.zip ibbs1126.zip "
[ ! -e "$d/tmp.zip" ] || { echo "  FAIL the archive is left behind"; fail=1; }
# January: last month is December of the year before
: > "$d/asked"; rm "$d/site/ibbs1126.zip"
run 2027-01-03 > /dev/null 2>&1 || true
check "the names in January" "$(tr '\n' ' ' < "$d/asked")" "bbs.txt ibbs010327.zip ibbs0127.zip ibbs1226.zip "
# a box that does not know the date asks only Netstalgia (no archive name)
: > "$d/asked"
if run 1970-01-01 > /dev/null 2>&1; then echo "  FAIL no date, yet success"; fail=1; fi
check "no date, no archive asked" "$(tr '\n' ' ' < "$d/asked")" "bbs.txt "
# something that is not an archive (an error page) is no list
echo "<html>not found</html>" > "$d/site/ibbs111626.zip"
if run 2026-11-16 > "$d/out" 2>/dev/null && [ -s "$d/out" ]; then echo "  FAIL an error page became a list"; fail=1; fi

# Netstalgia's list: taken as it is, before any archive, date or not
rm -f "$d/site/ibbs111626.zip"
api='# from Netstalgia
v90d-bbs 2
bbs telehack 2278353422 telehack.com Telehack'
printf '%s\n' "$api" > "$d/site/bbs.txt"
cp "$d/site/monthly.zip" "$d/site/ibbs1126.zip"
: > "$d/asked"
run 2026-11-16 > "$d/out" || { echo "  FAIL Netstalgia's list is there, yet failure"; fail=1; }
check "Netstalgia's list as it is" "$(cat "$d/out")" "$api"
check "only Netstalgia asked" "$(tr '\n' ' ' < "$d/asked")" "bbs.txt "
run 1970-01-01 > "$d/out" || { echo "  FAIL Netstalgia's list needs no date, yet failure"; fail=1; }
check "Netstalgia's list without a clock" "$(cat "$d/out")" "$api"
# a challenge page (not our head) is no list: on to the guide
echo "<html>Just a moment...</html>" > "$d/site/bbs.txt"
: > "$d/asked"
run 2026-11-16 > "$d/out" || { echo "  FAIL a challenge page and the archive, yet failure"; fail=1; }
check "a challenge page falls back to the guide" "$(cat "$d/out")" "$want"
check "then the guide's names" "$(tr '\n' ' ' < "$d/asked")" "bbs.txt ibbs111626.zip ibbs1126.zip "
# BBS_API="" skips Netstalgia; a box without the converter has only Netstalgia
: > "$d/asked"
BBS_API= run 2026-11-16 > /dev/null 2>&1
check "BBS_API empty skips it" "$(tr '\n' ' ' < "$d/asked")" "ibbs111626.zip ibbs1126.zip "
: > "$d/asked"
if PATH="$d/bin:$PATH" BBS_TMP="$d/tmp.zip" BBS_AWK="$d/none.awk" BBS_NOW=2026-11-16 sh "$fw/bbsfetch.sh" > /dev/null 2>&1; then
    echo "  FAIL no converter and no Netstalgia list, yet success"; fail=1; fi
check "no converter, no archive asked" "$(tr '\n' ' ' < "$d/asked")" "bbs.txt "
[ ! -e "$d/tmp.zip" ] || { echo "  FAIL the download is left behind"; fail=1; }

[ $fail = 0 ] && echo "bbsfetch: all checks pass" || { echo "bbsfetch: FAILED"; exit 1; }
