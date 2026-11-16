#!/bin/sh
# wrap.sh [EXTRA DAEMON OPTIONS]: run the box's daemon with extra options and a log in /tmp/v90d.log (RAM only, until reboot)
cd "$(dirname "$0")/.." || exit 1
echo "[ -f /usr/sbin/v90d.bin ] || { cp /usr/sbin/v90d /usr/sbin/v90d.bin; mv /usr/sbin/v90d /usr/sbin/v90d.orig; }; printf '#!/bin/sh\nexec /usr/sbin/v90d.bin \"\$@\" $* 2>>/tmp/v90d.log\n' > /usr/sbin/v90d; chmod 755 /usr/sbin/v90d; : > /tmp/v90d.log; kill \$(pidof v90d.bin) \$(pidof v90d) 2>/dev/null; sleep 5; ps | grep '[v]90d.bin' | cut -c27-150" | TSH_HOST=${BOX:-10.42.90.240} TSH_PORT=23 python3 fw/tools/tsh.py 9 | grep v90d.bin
