#!/bin/sh
# Write the list of what goes into the image's root file system, in the
# format of the kernel's gen_init_cpio (CONFIG_INITRAMFS_SOURCE).
#   fw/mkinitramfs.sh [debug] > build/initramfs.list
# "debug" adds a root shell on telnet and the bring-up tools. Never ship that.
set -e
fw=$(cd "$(dirname "$0")" && pwd)
top=$(dirname "$fw")
bb=$fw/build/busybox-1.36.1/busybox
[ -x "$bb" ] || { echo "build busybox first" >&2; exit 1; }

cat <<LIST
dir /bin 755 0 0
dir /sbin 755 0 0
dir /usr 755 0 0
dir /usr/bin 755 0 0
dir /usr/sbin 755 0 0
dir /etc 755 0 0
dir /etc/init.d 755 0 0
dir /dev 755 0 0
dir /proc 755 0 0
dir /sys 755 0 0
dir /tmp 1777 0 0
dir /var 755 0 0
dir /var/run 755 0 0
dir /root 700 0 0
nod /dev/console 600 0 0 c 5 1
nod /dev/null 666 0 0 c 1 3
file /bin/busybox $bb 755 0 0
slink /init /bin/busybox 777 0 0
slink /sbin/init /bin/busybox 777 0 0
slink /bin/sh /bin/busybox 777 0 0
file /etc/inittab $fw/inittab 644 0 0
file /etc/init.d/rcS $fw/rcS 755 0 0
file /etc/udhcpc.script $fw/udhcpc.script 755 0 0
file /etc/dclease.script $fw/dclease.script 755 0 0
file /etc/v90d.conf $fw/v90d.conf 600 0 0
file /usr/sbin/v90d $top/srv/build/v90d-arm 755 0 0
file /usr/sbin/v90nv $fw/tools/v90nv.arm 755 0 0
slink /etc/resolv.conf /tmp/resolv.conf 777 0 0
LIST
# the archive mode gateway (arch/), when it has been built
[ -f "$top/arch/build/v90arch-arm" ] || { echo "build the archive gateway first (make -C arch v90arch-arm)" >&2; exit 1; }
echo "file /usr/sbin/v90arch $top/arch/build/v90arch-arm 755 0 0"
if [ "$1" = debug ]; then
    echo "file /etc/debug $fw/debug.note 644 0 0"
    for t in peek slicpoke tdmpoke; do
        [ -f "$fw/tools/$t.arm" ] && echo "file /usr/sbin/$t $fw/tools/$t.arm 755 0 0"
    done
fi
exit 0
