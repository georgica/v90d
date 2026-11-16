#!/bin/sh
# flash.sh: write fw/build/v90d.uimage to the box's image partition, verify, reboot, confirm
cd "$(dirname "$0")/.." || exit 1
exec python3 - <<'PY'
import time, subprocess, os, hashlib
img='fw/build/v90d.uimage'; data=open(img,'rb').read(); md5=hashlib.md5(data).hexdigest(); size=len(data)
env=dict(os.environ, TSH_HOST=os.environ.get('BOX','10.42.90.240'), TSH_PORT='23')
def T(cmd, s=3):
    return subprocess.run(['python3','fw/tools/tsh.py',str(s)], input=cmd+'\n', text=True, capture_output=True, env=env).stdout
ENV='M=$(sed -n "s/^\\(mtd[0-9]*\\):.*\\"uboot-env\\"$/\\1/p" /proc/mtd); v90nv env-get /dev/$M v90d_tries'
o=T(ENV,3)
if '\n3' not in '\n'+o: print('fallback counter is not 3, not flashing:', o); raise SystemExit(1)
srv=subprocess.Popen(['timeout','60','nc','-l','9091'], stdin=open(img,'rb')); time.sleep(1)
o=T('rm -f /tmp/cap.raw /tmp/fc.log; sleep 8 | nc 10.42.90.1 9091 > /tmp/v90d.uimage; md5sum /tmp/v90d.uimage',14); srv.kill()
if md5 not in o: print('transfer failed:', o); raise SystemExit(1)
T('(flashcp -v /tmp/v90d.uimage /dev/mtd2 >/tmp/fc.out 2>&1; echo rc=$? > /tmp/fc.log) &',2)
for i in range(18):
    time.sleep(10)
    o=T('cat /tmp/fc.log 2>/dev/null',2)
    if 'rc=' in o: break
print('flashcp:', [l for l in o.split('\n') if l.startswith('rc=')])
o=T('head -c %d /dev/mtd2 | md5sum' % size,6)
if md5 not in o: print('READBACK MISMATCH, not rebooting'); raise SystemExit(1)
print('readback md5 ok', md5, size)
T('sync; reboot',2); time.sleep(100)
print(T('uptime; pidof v90d; '+ENV+'; dmesg | grep "start confirmed"',4))
PY
