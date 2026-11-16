#!/bin/sh
# host test of v90nv on plain files:  fw/tools/test_v90nv.sh [path-to-v90nv [runner]]
# The environment it writes is checked against an independent CRC (python zlib).
nv="${2:+$2 }${1:-./v90nv.host}"
t=$(mktemp -d); trap 'rm -rf "$t"' EXIT
n=0; f=0
ok() { n=$((n+1)); if eval "$1"; then :; else f=$((f+1)); echo "  FAIL: $1"; fi; }
printf 'bootcmd=gsboot\nbootdelay=1\n# a comment\nbootm=setenv bootargs ${x}; bootm ${loadaddr} - ${devtree_addr}\n' > $t/list
ok "$nv env-write $t/env $t/list"
ok "[ \$(stat -c %s $t/env) = 8192 ]"
ok "python3 -c \"import zlib,sys;d=open('$t/env','rb').read();sys.exit(0 if int.from_bytes(d[:4],'little')==zlib.crc32(d[4:]) else 1)\""
ok "python3 -c \"import sys;d=open('$t/env','rb').read()[4:];sys.exit(0 if d.startswith(b'bootcmd=gsboot\\\\0bootdelay=1\\\\0bootm=setenv bootargs \\\${x}; bootm \\\${loadaddr} - \\\${devtree_addr}\\\\0\\\\0') else 1)\""
ok "[ \"\$($nv env-get $t/env bootdelay)\" = 1 ]"
ok "[ \"\$($nv env-get $t/env bootcmd)\" = gsboot ]"
ok "! $nv env-get $t/env nothere"
ok "! $nv env-get $t/env boot"
ok "$nv env-set $t/env tries 3 && [ \"\$($nv env-get $t/env tries)\" = 3 ]"
ok "$nv env-set $t/env tries 2 && [ \"\$($nv env-get $t/env tries)\" = 2 ] && [ \$($nv env-print $t/env | grep -c tries) = 1 ]"
ok "[ \"\$($nv env-get $t/env bootcmd)\" = gsboot ] && [ \$($nv env-print $t/env | wc -l) = 4 ]"
ok "$nv env-set $t/env tries && ! $nv env-get $t/env tries && [ \$($nv env-print $t/env | wc -l) = 3 ]"
ok "python3 -c \"import zlib,sys;d=open('$t/env','rb').read();sys.exit(0 if int.from_bytes(d[:4],'little')==zlib.crc32(d[4:]) else 1)\""
# unchanged: not rewritten
before=$(stat -c %Y.%y $t/env); sleep 1.1
ok "$nv env-set $t/env bootdelay 1 && [ \"\$(stat -c %Y.%y $t/env)\" = \"$before\" ]"
# an erased or damaged environment: never created by env-set
head -c 8192 /dev/zero | tr '\0' '\377' > $t/erased
ok "! $nv env-set $t/erased tries 3 2>/dev/null && [ \$(tr -d '\377' < $t/erased | wc -c) = 0 ]"
ok "! $nv env-print $t/erased 2>/dev/null"
cp $t/env $t/bad; printf 'X' | dd of=$t/bad bs=1 seek=100 conv=notrunc 2>/dev/null
ok "! $nv env-get $t/bad bootcmd"
ok "! $nv env-set $t/bad tries 3 2>/dev/null && cmp -s $t/bad $t/bad"
printf 'a=1\na=2\n' > $t/dup; ok "! $nv env-write $t/x $t/dup 2>/dev/null"
printf 'novalue\n' > $t/nov; ok "! $nv env-write $t/x $t/nov 2>/dev/null"
# too much for 8 KiB
python3 -c "print('big=' + 'x'*9000)" > $t/big; ok "! $nv env-write $t/x $t/big 2>/dev/null"
# blob
printf 'number 5551234\nuser a b\n' > $t/conf
ok "$nv blob-save $t/blob $t/conf && $nv blob-load $t/blob $t/out && cmp -s $t/conf $t/out"
ok "! $nv blob-load $t/erased $t/out2 && [ ! -e $t/out2 ]"
cp $t/blob $t/bblob; printf 'X' | dd of=$t/bblob bs=1 seek=14 conv=notrunc 2>/dev/null
ok "! $nv blob-load $t/bblob $t/out3"
: > $t/empty; ok "$nv blob-save $t/blob $t/empty && $nv blob-load $t/blob $t/out4 && [ ! -s $t/out4 ]"
head -c 70000 /dev/zero > $t/huge; ok "! $nv blob-save $t/blob $t/huge 2>/dev/null"
echo "v90nv: $n checks, $f failures"; [ $f = 0 ]
