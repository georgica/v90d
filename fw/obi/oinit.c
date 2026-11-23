/* oinit: the whole initramfs of v90d's own kernel on the OBi200. It mounts
 * the box's own root file system (the stock squashfs, mtd3: its BusyBox,
 * its libraries) and v90d's (/obi, mtd5) on it, moves /dev there, makes it
 * the root and runs /obi/kinit with the stock system's shell. On any
 * failure the box restarts: the launcher under the stock kernel then sees
 * the RAM boot counter (bootcount.c) and stays on the stock kernel. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <unistd.h>

static void say(const char *what, const char *arg)
{
    char b[160];
    int fd = open("/dev/kmsg", O_WRONLY), n;
    n = snprintf(b, sizeof b, "oinit: %s %s: %s\n", what, arg, strerror(errno));
    if (fd >= 0) { (void)!write(fd, b, n > 0 ? (size_t)n : 0); close(fd); }
}

static void fail(const char *what, const char *arg)
{
    say(what, arg);
    sync();
    sleep(2);
    reboot(RB_AUTOBOOT);
    for (;;) pause();
}

int main(void)
{
    mkdir("/dev", 0755);
    if (mount("devtmpfs", "/dev", "devtmpfs", 0, 0) && errno != EBUSY) fail("mount", "/dev");
    mkdir("/root", 0755);
    if (mount("/dev/mtdblock3", "/root", "squashfs", MS_RDONLY, 0)) fail("mount the root file system", "/dev/mtdblock3");
    if (mount("/dev/mtdblock5", "/root/obi", "squashfs", MS_RDONLY, 0)) fail("mount", "/dev/mtdblock5 on /obi");
    if (mount("/dev", "/root/dev", 0, MS_MOVE, 0)) fail("move", "/dev");
    if (chdir("/root") || mount(".", "/", 0, MS_MOVE, 0) || chroot(".") || chdir("/")) fail("switch to", "/root");
    execl("/bin/sh", "sh", "/obi/kinit", (char *)0);
    fail("run", "/obi/kinit");
    return 1;
}
