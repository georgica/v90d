#!/bin/sh
# check.sh REPLAY: replay every fixture of fixtures.txt that is here and
# compare what the modem connected with. Exit 1 on any difference.
cd "$(dirname "$0")" || exit 1
R=$1; fail=0; n=0; tmp=${TMPDIR:-/tmp}/v90m-fixture.$$
grep -v '^#' fixtures.txt | while IFS= read -r line; do
    [ -z "$line" ] && continue
    f=$(echo "$line" | awk '{print $1}')
    opts=$(echo "$line" | sed 's/^[^ ]* *//; s/ *modulation.*//')
    want=$(echo "$line" | grep -o 'modulation [0-9]*, [0-9]* bit/s down, [0-9]* up')
    if [ ! -f "$f" ]; then echo "  $f: not here, skipped"; continue; fi
    xz -dc "$f" > $tmp.raw
    got=$($R $opts $tmp.raw | grep -o 'modulation [0-9]*, [0-9]* bit/s down, [0-9]* up')
    if [ "$got" = "$want" ]; then echo "  $f: $got"; else echo "  FAIL $f: got \"$got\", want \"$want\""; echo fail > $tmp.fail; fi
done
rm -f $tmp.raw
if [ -f $tmp.fail ]; then rm -f $tmp.fail; echo "FIXTURES FAILED"; exit 1; fi
echo "fixtures: all present ones pass"
