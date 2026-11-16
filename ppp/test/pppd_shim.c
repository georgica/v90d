/* LD_PRELOAD shim for the pppd interop run (host only, never shipped).
 *
 * pppd on Linux needs /dev/ppp, which only real root can open; the kernel
 * behind it does the HDLC framing and owns the network interface, while
 * pppd itself runs LCP, authentication and IPCP in user space. This shim
 * replaces that kernel side so an unprivileged pppd can run as a peer:
 *
 *   - open("/dev/ppp") returns a duplicate of the SOCK_SEQPACKET socket
 *     named by $PPP_SHIM_FD; every PPP packet pppd writes becomes one
 *     datagram (protocol number first), and datagrams from the harness are
 *     what pppd reads;
 *   - the PPPIOC* ioctls, the N_PPP line discipline switch and the network
 *     interface ioctls on "ppp0" are accepted and ignored, and "ppp0"
 *     resolves to an interface index, so pppd believes it has configured
 *     an interface (there is none; no IP traffic can flow through pppd).
 *
 * What this means for the result: pppd's protocol state machines are the
 * real, unmodified ones; the kernel's ppp_async framing is NOT exercised. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <net/if.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <unistd.h>

static int is_ppp_dev(const char *path)
{
    return path && strcmp(path, "/dev/ppp") == 0;
}

static int shim_dup(void)
{
    const char *s = getenv("PPP_SHIM_FD");
    return s ? dup(atoi(s)) : -1;
}

#define OPEN_WRAPPER(name)                                              \
    int name(const char *path, int flags, ...)                          \
    {                                                                   \
        static int (*real)(const char *, int, ...);                     \
        mode_t mode = 0;                                                \
        if (is_ppp_dev(path))                                           \
            return shim_dup();                                          \
        if (flags & (O_CREAT | O_TMPFILE)) {                            \
            va_list ap;                                                 \
            va_start(ap, flags);                                        \
            mode = (mode_t)va_arg(ap, int);                             \
            va_end(ap);                                                 \
        }                                                               \
        if (!real)                                                      \
            real = (int (*)(const char *, int, ...))dlsym(RTLD_NEXT, #name); \
        return real(path, flags, mode);                                 \
    }

OPEN_WRAPPER(open)
OPEN_WRAPPER(open64)

int __open_2(const char *path, int flags)
{
    return open(path, flags);
}

int __open64_2(const char *path, int flags)
{
    return open64(path, flags);
}

int ioctl(int fd, unsigned long req, ...)
{
    static int (*real)(int, unsigned long, ...);
    va_list ap;
    void *arg;

    va_start(ap, req);
    arg = va_arg(ap, void *);
    va_end(ap);

    /* every PPPIOC* request has ioctl type 't' */
    if (_IOC_TYPE(req) == 't') {
        if ((_IOC_DIR(req) & _IOC_READ) && arg)
            memset(arg, 0, _IOC_SIZE(req));   /* unit 0, channel 0, flags 0 */
        return 0;
    }
    if (req == TIOCSETD)                      /* "attach N_PPP": pretend */
        return 0;
    /* interface configuration on the (nonexistent) ppp0 */
    if ((req & 0xff00) == 0x8900) {
        if (req == SIOCGIFFLAGS && arg)
            ((struct ifreq *)arg)->ifr_flags = 0;
        return 0;
    }
    if (!real)
        real = (int (*)(int, unsigned long, ...))dlsym(RTLD_NEXT, "ioctl");
    return real(fd, req, arg);
}

/* pppd looks its interface up by name around the ip-pre-up script */
#define FAKE_IFINDEX 4242

unsigned int if_nametoindex(const char *name)
{
    static unsigned int (*real)(const char *);

    if (name && strcmp(name, "ppp0") == 0)
        return FAKE_IFINDEX;
    if (!real)
        real = (unsigned int (*)(const char *))dlsym(RTLD_NEXT, "if_nametoindex");
    return real(name);
}

char *if_indextoname(unsigned int idx, char buf[IF_NAMESIZE])
{
    static char *(*real)(unsigned int, char *);

    if (idx == FAKE_IFINDEX)
        return strcpy(buf, "ppp0");
    if (!real)
        real = (char *(*)(unsigned int, char *))dlsym(RTLD_NEXT, "if_indextoname");
    return real(idx, buf);
}
