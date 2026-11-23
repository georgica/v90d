#!/bin/sh
# Fetch the OBi firmware file the OBi200's v90d file is built from: the
# community firmware arrynrob13 (obifirmware.com), checked against the
# SHA-256 the v90d builds were tested with. Prints the file's path.
#   sh fw/obi/getbase.sh            (into fw/obi/dl/)
#   make -C fw/obi image BASE=$(sh fw/obi/getbase.sh)
set -e
here=$(cd "$(dirname "$0")" && pwd)
NAME=OBi2xx-3-2-2-8680EX-arrynrob13.fw
URL=https://obifirmware.com/fw/$NAME
SHA=ea68fc963c76fc0219698bc1d44b124602bce2f1a27122722fa8429b2df49877
F=$here/dl/$NAME
mkdir -p "$here/dl"
if ! { [ -f "$F" ] && echo "$SHA  $F" | sha256sum -c --status; }; then
    echo "fetching $URL" >&2
    curl -fsSL --retry 3 -o "$F.part" "$URL"
    echo "$SHA  $F.part" | sha256sum -c --status || { echo "$NAME: checksum does not match" >&2; rm -f "$F.part"; exit 1; }
    mv "$F.part" "$F"
fi
echo "$F"
