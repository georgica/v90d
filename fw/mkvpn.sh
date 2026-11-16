#!/bin/sh
# Build the OpenVPN client for the box: fw/build/vpn/openvpn, static, with
# mbed TLS for its cryptography. Neither is ours and neither is changed; they
# are built from the release archives that fw/setup.sh fetched and checked.
# (OpenVPN is GPL-2.0, mbed TLS Apache-2.0 or GPL-2.0-or-later.)
#
# Left out of OpenVPN: LZO and LZ4 (a peer that compresses is still spoken
# to, by the stub), plugins, the management interface, PKCS#11, data channel
# offload, and libcap-ng (it only serves to drop root, which is not done
# here: configure insists on the library, so its define is taken out again).
set -e
fw=$(cd "$(dirname "$0")" && pwd)
top=$(dirname "$fw")
OV=openvpn-2.6.12
MB=mbedtls-2.28.9
B=$fw/build/vpn
tc=$(echo "$top"/toolchain/*/bin)
[ -x "$tc/arm-linux-gcc" ] || { echo "no cross toolchain: sh fw/setup.sh" >&2; exit 1; }
[ -x "$B/openvpn" ] && [ "$B/openvpn" -nt "$fw/mkvpn.sh" ] && { ls -l "$B/openvpn"; exit 0; }
mkdir -p "$B"
[ -d "$B/$MB" ] || tar -C "$B" -xjf "$fw/dl/$MB.tar.bz2"
[ -d "$B/$OV" ] || tar -C "$B" -xzf "$fw/dl/$OV.tar.gz"
echo "building mbed TLS"
make -s -C "$B/$MB" -j"$(nproc)" lib CC="$tc/arm-linux-gcc" AR="$tc/arm-linux-ar" \
    CFLAGS="-Os -ffunction-sections -fdata-sections" > "$B/mbedtls.log" 2>&1
echo "building OpenVPN"
( cd "$B/$OV"
  PATH="$tc:$PATH" ./configure --host=arm-linux CC=arm-linux-gcc --with-crypto-library=mbedtls \
    MBEDTLS_CFLAGS="-I$B/$MB/include" MBEDTLS_LIBS="-L$B/$MB/library -lmbedtls -lmbedx509 -lmbedcrypto" \
    LIBCAPNG_CFLAGS=" " LIBCAPNG_LIBS=" " \
    --disable-lzo --disable-lz4 --enable-comp-stub --disable-plugins --disable-plugin-auth-pam \
    --disable-plugin-down-root --disable-dco --disable-management --disable-debug --enable-small \
    --disable-port-share --disable-fragment --disable-ofb-cfb --disable-unit-tests --disable-pkcs11 \
    --disable-systemd --disable-selinux IFCONFIG=/sbin/ifconfig ROUTE=/sbin/route \
    CFLAGS="-Os -ffunction-sections -fdata-sections" LDFLAGS="-Wl,--gc-sections" > "$B/openvpn-configure.log" 2>&1
  sed -i 's|^#define HAVE_LIBCAPNG 1|/* no libcap-ng: it runs as root and stays root */|' config.h
  PATH="$tc:$PATH" make -s -j"$(nproc)" -C src/compat > "$B/openvpn-make.log" 2>&1
  PATH="$tc:$PATH" make -s -j"$(nproc)" -C src/openvpn LDFLAGS="-all-static -Wl,--gc-sections" >> "$B/openvpn-make.log" 2>&1 )
"$tc/arm-linux-strip" -o "$B/openvpn" "$B/$OV/src/openvpn/openvpn"
ls -l "$B/openvpn"
