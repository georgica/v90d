#!/usr/bin/env python3
"""The U-Boot environment for a v90d box, as a NAME=VALUE list for
`v90nv env-write`.

A saved environment replaces U-Boot's built-in one entirely, so the list
starts from the built-in one, taken from the unit's own U-Boot image (the
flash backup), and changes only what the stage says:

    mkenvlist.py UBOOT.bin save      stage 1: the trial script, but its boot
                                     step only saves the environment
    mkenvlist.py UBOOT.bin boot      stage 2: the real thing

The boot command is `run v90d_try; gsboot`: whatever happens in the trial,
short of our kernel starting, the stock boot follows. The trial counts
v90d_tries down by one and saves that BEFORE starting our image (at
flash offset 0x2E0000, mapped at 0x802E0000), so an image that hangs gets at
most three power cycles before the unit is back on the stock firmware. Our
system puts the count back to 3 once it is up (fw/rcS).

Two things learned from this unit's U-Boot (newer than the published source):
its `gsboot` overwrites gxp_console itself, so no variable of ours reaches
the stock kernel's command line (there is no cheap "is the saved environment
in use" probe; stage 1 shows it by the count going from 3 to 2 in flash);
and `console` stays `no`: that is what keeps U-Boot deaf to noise on the
unconnected serial input during its one-second boot delay.
"""
import sys

def builtin(path):
    d = open(path, 'rb').read()
    i = d.rfind(b'bootcmd=gsboot\0')         # the second copy is default_environment[]
    e = d.find(b'\0\0', i)
    return [v.decode('ascii') for v in d[i:e].split(b'\0')]

TRY = ' '.join('if test "${v90d_tries}" = %d; then setenv v90d_tries %d; run v90d_boot; fi;' % (n, n - 1) for n in (1, 2, 3)).rstrip(';')

def main(uboot, stage):
    env = builtin(uboot)
    names = [v.split('=', 1)[0] for v in env]
    assert names[0] == 'bootcmd' and 'gxp_console' in names and len(names) == len(set(names)), names
    def put(name, value):
        if name in names: env[names.index(name)] = name + '=' + value
        else: names.append(name); env.append(name + '=' + value)
    if stage in ('save', 'boot'):
        put('bootcmd', 'run v90d_try; gsboot')
        put('v90d_try', TRY)
        put('v90d_boot', 'saveenv' if stage == 'save' else 'saveenv && bootm 0x802E0000')
        put('v90d_tries', '3')
    else:
        sys.exit(__doc__)
    print('\n'.join(env))
if __name__ == '__main__':
    if len(sys.argv) != 3: sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
