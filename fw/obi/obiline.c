/* obiline: the v90d telephone line of the Obihai OBi200, in user space.
 *
 * The OBi200 keeps its maker's kernel and its telephony module (ffxav_mp.ko),
 * which offers the ProSLIC's registers and the TDM highway to a program
 * instead of the maker's own application (obiapp). This program is the line
 * driver that fw/kernel/drivers/misc/v90line is on the HT80x, for the one FXS
 * port of the OBi200, and serves the daemon the same protocol over a UNIX
 * packet socket (srv/line.h, "seq:PATH"):
 *
 *   the SLIC  a dual-channel Si3226x (rev C, LCQCUK converter) of which
 *             channel 0 is wired to the jack; brought up by fw/drv/slic.c,
 *             patch-free, through the module's register ioctls on /dev/fxs0
 *             (no raw SPI: ps_bus.reg_rd / reg_wr)
 *   the audio /dev/pcm: one read or write every 10 ms carries 320 octets,
 *             80 16-bit samples of channel 0 and then 80 of channel 1. The
 *             part runs 16-bit linear; what we send is the exact linear value
 *             of each codeword (pump_lin16), as on the HT80x in linear mode
 *   the clock the module's DMA, driven the way the maker's application drives
 *             it: both channels started on their /dev/fxsN (0x7191, 0x718a),
 *             then a write whenever the device takes one and a read whenever
 *             it has one (poll). The module stops a channel for good on an
 *             overrun of its three-period ring, so this loop runs SCHED_FIFO
 *             and does nothing else; the daemon's slack is the pump's
 *             (LATENCY codewords), between this loop and the socket.
 *
 * Facts about the module: fw/obi/README.md (read off the box; no vendor
 * code). Written for this project. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "pump.h"
#include "slic.h"

/* ffxav_mp.ko, /dev/fxsN: register access to that channel of the SLIC, and
 * the TDM channel behind it */
#define FXS_REG_RD   0xc00171b2u    /* arg[0] = register; value back in arg[4] */
#define FXS_REG_WR   0x400471b3u    /* arg[0] = register, arg[4] = value */
#define FXS_TDM_PREP 0x7191u        /* what the maker's application issues first */
#define FXS_TDM_ON   0x718au
#define FXS_TDM_OFF  0x718bu
#define FXS_RESET    0x71b6u        /* arg 0: the SLIC's reset line low ("Reset SLIC chip"),
                                       else high ("UnReset"); the part is held in reset
                                       from boot until the application lets it go */

#define CHANS        2              /* TDM channels the module runs together */
#define PCM_OCTETS   (CHANS * 2 * PUMP_BLOCK)
#define LATENCY      240            /* as v90line: the daemon may be 20 ms late */
#define OUTQ         4              /* answers waiting for the device to take them */

static const char *sock_path = "/var/run/v90line";
static const char *stats_path = "/var/run/v90line.stats";
static int verbose;
static int loopback = -1;        /* -L n: write n to the LOOPBACK register after bring-up (bench) */

static void logm(const char *fmt, ...)
{
    va_list ap;
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    fprintf(stderr, "obiline %ld.%03ld: ", (long)t.tv_sec, t.tv_nsec / 1000000);
    va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    fputc('\n', stderr);
}

/* ---- the SLIC through the module ---- */

typedef struct { int fd; unsigned long errors; } fxs_t;

static uint8_t fxs_rd(void *ctx, uint8_t reg)
{
    fxs_t *f = ctx;
    unsigned char a[8] = { 0 };
    a[0] = reg;
    if (ioctl(f->fd, FXS_REG_RD, a) < 0) { f->errors++; return 0xFF; }
    return a[4];
}

static void fxs_wr(void *ctx, uint8_t reg, uint8_t v)
{
    fxs_t *f = ctx;
    unsigned char a[8] = { 0 };
    a[0] = reg; a[4] = v;
    if (ioctl(f->fd, FXS_REG_WR, a) < 0) f->errors++;
}

static uint8_t fxs_xfer(void *ctx, uint8_t out) { (void)ctx; (void)out; return 0xFF; }

static void sleep_ms(unsigned ms)
{
    struct timespec t = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000L };
    while (nanosleep(&t, &t) < 0 && errno == EINTR) {}
}
static void fxs_msleep(void *ctx, unsigned ms) { (void)ctx; sleep_ms(ms); }

/* ---- the power light (ffxav_led.ko, /dev/led) ----
 * The module starts the power light blinking. The maker's application, once
 * it is up, leaves it in the module's state that a "blink" (command 4: light
 * 0, states 0 and 1, a count that never runs out, period 100) followed by an
 * "off" (command 2: light 0) gives: state 0 shown steadily, the steady green
 * light. Read off the module's table on a unit while that application ran;
 * the same calls here leave the table exactly as it had it. */
#define LED_OFF   2
#define LED_BLINK 4
static int led_fd = -1;

/* Under v90d's own kernel the lights are LED class devices (fw/kernel
 * leds-ffxav-phy.c and gpio-leds): kinit starts the power light blinking
 * (timer trigger), steady once the line is up, as on the maker's system. */
#define SYS_LED "/sys/class/leds/obi200:green:"
static void sys_led(const char *name, const char *file, const char *v)
{
    char p[96];
    int fd;
    snprintf(p, sizeof p, SYS_LED "%s/%s", name, file);
    if ((fd = open(p, O_WRONLY | O_CLOEXEC)) < 0) return;
    (void)!write(fd, v, strlen(v));
    close(fd);
}

static void power_led(void)
{
    struct { uint8_t led, a, b, pad; uint32_t count, period; } arg = { 0, 0, 1, 0, 0x7fffffffu, 100 };
    if (led_fd < 0) led_fd = open("/dev/led", O_RDONLY | O_CLOEXEC);
    if (led_fd < 0) {
        sys_led("power", "trigger", "none");
        sys_led("power", "brightness", "1");
        return;
    }
    (void)ioctl(led_fd, LED_BLINK, &arg);
    (void)ioctl(led_fd, LED_OFF, &arg);
}

/* The phone light: the module's light 1 (the first telephone port of the
 * maker's two-port boards, channel 0 of the SLIC; light 2 is the second),
 * on (command 1) while the telephone is off hook, off (command 2) on hook,
 * as the maker documents it. (Its ringing blink has no use here: v90d does
 * not ring the telephone.) */
#define LED_ON 1
static void phone_led(int on)
{
    struct { uint8_t led, a, b, pad; uint32_t count, period; } arg = { 1, 0, 0, 0, 0, 0 };
    if (led_fd < 0) led_fd = open("/dev/led", O_RDONLY | O_CLOEXEC);
    if (led_fd >= 0) (void)ioctl(led_fd, on ? LED_ON : LED_OFF, &arg);
    else sys_led("phone", "brightness", on ? "1" : "0");
}

/* ---- shared state, under lk ---- */

static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
static pump_t pump;
static int hook, fresh, law, lin = 1, want_law, want_lin = 1;
static int16_t lin16[2][256];
static pump_meter meter;
static int notify[2] = { -1, -1 };      /* the audio loop tells the server a block is ready */
static unsigned long frames, dev_short, out_idle, out_dropped, stale, slic_inits, slic_err_last;
static long vbat_mv;
static int slic_up, slic_stage;

static uint8_t idle_of(int alaw) { return alaw ? 0xd5 : 0xff; }

static void pump_restart_locked(int h)
{
    hook = h;
    pump_start(&pump, idle_of(law), LATENCY);
    fresh = 1;
}

/* the nearest codeword of a linear sample, for a reader that asks for
 * companded receive while the part stays linear */
static uint8_t encode(int alaw, int16_t v)
{
    int best = 0, i;
    long bd = 1L << 30;
    for (i = 0; i < 256; i++) {
        long d = (long)lin16[alaw][i] - v;
        if (d < 0) d = -d;
        if (d < bd) { bd = d; best = i; }
    }
    return (uint8_t)best;
}

/* ---- the audio loop ---- */

static int pcm_fd = -1;

static long mono_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long)t.tv_sec * 1000000L + t.tv_nsec / 1000;
}

static void *audio_loop(void *arg)
{
    struct sched_param sp = { 80 };
    uint8_t in[PCM_OCTETS], outq[OUTQ][PCM_OCTETS], idle[PCM_OCTETS];
    unsigned oq_r = 0, oq_w = 0;
    /* The module paces us: one read and one write per 10 ms. If it stops
     * doing so (a channel it stopped, an error that returns at once), this
     * thread at real-time priority would starve the box: leave instead. */
    long last_rd = mono_us(), rep_t = last_rd, gap_max = 0;
    unsigned fast = 0, rd_errs = 0, wr_errs = 0, wr_unread = 0, n_rd = 0, n_wr = 0, n_poll = 0;
    (void)arg;
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0) logm("audio: no SCHED_FIFO");
    memset(idle, 0, sizeof idle);
    for (;;) {
        struct pollfd pf = { pcm_fd, POLLIN | POLLOUT, 0 };
        int r = poll(&pf, 1, 200);
        if (r < 0) { if (errno == EINTR) continue; logm("audio: poll: %s", strerror(errno)); break; }
        if (r == 0) { logm("audio: the TDM stopped"); break; }
        n_poll++;
        if (rd_errs > 20 || wr_errs > 20) { logm("audio: the device fails at once (%s)", strerror(errno)); break; }
        /* writes taken with no block read in between: the receive side has
         * stopped and nothing paces us any more */
        if (wr_unread > 20) { logm("audio: the device takes blocks but gives none"); break; }
        if (!(pf.revents & (POLLIN | POLLOUT))) { logm("audio: poll gives %x", pf.revents); break; }
        if (pf.revents & POLLOUT) {
            const uint8_t *o = oq_r != oq_w ? outq[oq_r++ % OUTQ] : idle;
            if (o == idle) out_idle++;
            if (write(pcm_fd, o, PCM_OCTETS) != PCM_OCTETS) { dev_short++; wr_errs++; } else { wr_errs = 0; n_wr++; wr_unread++; }
        }
        if (pf.revents & POLLIN) {
            int16_t rx[PUMP_BLOCK];
            uint8_t tx[PUMP_BLOCK];
            int i, ready, l, a;
            long now;
            if (read(pcm_fd, in, PCM_OCTETS) != PCM_OCTETS) { dev_short++; rd_errs++; continue; }
            rd_errs = 0; wr_unread = 0; n_rd++;
            now = mono_us();
            if (now - last_rd > gap_max) gap_max = now - last_rd;
            /* a block every 10 ms; a run of them much faster means the
             * module no longer waits for the hardware */
            if (now - last_rd < 2000) { if (++fast > 50) { logm("audio: blocks come unpaced"); break; } }
            else fast = 0;
            last_rd = now;
            if (verbose && now - rep_t >= 10000000L) {
                logm("audio: %u polls, %u reads, %u writes in %ld ms, worst gap %ld us, idle writes %lu",
                     n_poll, n_rd, n_wr, (now - rep_t) / 1000, gap_max, out_idle);
                rep_t = now; gap_max = 0; n_poll = n_rd = n_wr = 0;
            }
            pthread_mutex_lock(&lk);
            l = lin; a = law;
            for (i = 0; i < PUMP_BLOCK; i++) {
                int16_t v = (int16_t)(uint16_t)(in[2 * i] | in[2 * i + 1] << 8);
                pump_meter_add(&meter, v);
                rx[i] = l ? v : encode(a, v);
            }
            pump_tick(&pump, rx, tx, PUMP_BLOCK);
            ready = pump_ready(&pump);
            frames += PUMP_BLOCK;
            pthread_mutex_unlock(&lk);
            if (oq_w - oq_r < OUTQ) {
                uint8_t *o = outq[oq_w++ % OUTQ];
                for (i = 0; i < PUMP_BLOCK; i++) {
                    uint16_t v = (uint16_t)lin16[a][tx[i]];
                    o[2 * i] = (uint8_t)v; o[2 * i + 1] = (uint8_t)(v >> 8);
                }
                memset(o + 2 * PUMP_BLOCK, 0, PCM_OCTETS - 2 * PUMP_BLOCK);
            } else {
                out_dropped++;      /* the device has not taken the last four: this one is lost */
            }
            if (ready) { char c = 1; (void)!write(notify[1], &c, 1); }
        }
    }
    exit(3);    /* the module has stopped the channel: let the supervisor start us again */
    return NULL;
}

/* ---- the SLIC and the hook ---- */

static fxs_t fx0;
static ps_bus bus0;

static int bring_up(void)
{
    slic_cfg cfg = { SLIC_PCM_LINEAR, 0, 0 };
    slic_info info;
    int r;
    memset(&info, 0, sizeof info);
    /* from reset, as on the HT80x: held 10 ms, then 250 ms to come out.
     * This also clears whatever the maker's application left in the part
     * (its patch). Then our own bring-up of channel 0. */
    if (ioctl(fx0.fd, FXS_RESET, 0) < 0) logm("SLIC reset: %s", strerror(errno));
    sleep_ms(10);
    if (ioctl(fx0.fd, FXS_RESET, 1) < 0) logm("SLIC reset release: %s", strerror(errno));
    sleep_ms(250);
    r = slic_init(&bus0, &cfg, &info);
    slic_inits++;
    slic_err_last = (unsigned long)(long)r;
    slic_stage = info.stage;
    vbat_mv = info.vbat_mv;
    if (r) logm("SLIC start failed: %d at stage %d (id %02x, battery %ld mV)", r, info.stage, info.id, info.vbat_mv);
    else logm("SLIC id %02x up, battery %ld mV", info.id, info.vbat_mv);
    if (!r && loopback >= 0) { ps_wr(&bus0, PS_LOOPBACK, (uint8_t)loopback); logm("LOOPBACK = %02x", ps_rd(&bus0, PS_LOOPBACK)); }
    return r;
}

static void write_stats(void)
{
    char tmp[256];
    FILE *f;
    snprintf(tmp, sizeof tmp, "%s.tmp", stats_path);
    f = fopen(tmp, "w");
    if (!f) return;
    pthread_mutex_lock(&lk);
    fprintf(f, "frames %lu\nslic_up %d\nslic_inits %lu\nslic_last_error %ld\nslic_last_stage %d\n"
            "battery_mv %ld\nhook %d\nlaw %s\nreceive %s\ntdm_short %lu\ntdm_idle_writes %lu\n"
            "tdm_dropped %lu\npump_underruns %lu\npump_overruns %lu\nstale_writes %lu\nspi_errors %lu\n",
            frames, slic_up, slic_inits, (long)slic_err_last, slic_stage, vbat_mv, hook,
            law ? "alaw" : "ulaw", lin ? "linear" : "companded", dev_short, out_idle, out_dropped,
            pump.underruns, pump.overruns, stale, fx0.errors);
    pthread_mutex_unlock(&lk);
    fclose(f);
    if (rename(tmp, stats_path) < 0) {
        static int told;
        if (!told++) logm("%s: %s", stats_path, strerror(errno));
    }
}

/* The threads that share `lk` with the audio loop run real-time too, above
 * the daemon (SCHED_FIFO 50) and below the audio loop (80): a holder of the
 * lock must never wait behind the daemon's signal processing while the
 * audio loop waits for it. Their work under the lock is a few microseconds;
 * the rest of their time they sleep or wait for the socket. */
#define HELPER_PRIO 70
static void helper_prio(const char *who)
{
    struct sched_param sp = { HELPER_PRIO };
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0) logm("%s: no SCHED_FIFO", who);
}

static void *slic_loop(void *arg)
{
    int seen = 0, run = 0, tick = 0, fails = 0;
    (void)arg;
    helper_prio("hook");
    for (;;) {
        int h, wl, wn, changed;
        if (!slic_up) {
            if (bring_up()) {
                fails++;
                write_stats();
                sleep_ms(fails < 5 ? 5000 : 60000);
                continue;
            }
            fails = 0;
            slic_up = 1;
            power_led();
            seen = run = 0;
            pthread_mutex_lock(&lk); pump_restart_locked(0); pthread_mutex_unlock(&lk);
        }
        pthread_mutex_lock(&lk);
        wl = want_law; wn = want_lin;
        changed = wl != law || wn != lin;
        if (changed) {
            /* the part stays linear; only our side changes */
            law = wl; lin = wn;
            pump_restart_locked(hook);
        }
        pthread_mutex_unlock(&lk);
        if (changed) logm("%s, %s receive", wl ? "A-law" : "mu-law", wn ? "linear" : "companded");
        /* the hook, believed when it reads the same three times running */
        h = slic_offhook(&bus0);
        if (h == seen) {
            if (run < 3 && ++run == 3 && h != hook) {
                pthread_mutex_lock(&lk); pump_restart_locked(h); pthread_mutex_unlock(&lk);
                phone_led(h);
                if (verbose) logm("%s hook", h ? "off" : "on");
            }
        } else {
            seen = h;
            run = 1;
        }
        if (++tick >= 100) {
            tick = 0;
            vbat_mv = slic_vbat_mv(&bus0);
            if (ps_rd(&bus0, PS_PCMMODE) != (0x10 | SLIC_PCM_LINEAR)) {
                logm("the SLIC lost its settings, starting it again");
                slic_up = 0;
                phone_led(0);
                pthread_mutex_lock(&lk); pump_restart_locked(0); pthread_mutex_unlock(&lk);
            }
            write_stats();
        }
        sleep_ms(10);
    }
    return NULL;
}

/* ---- the daemon's socket ---- */

static void serve(int cl)
{
    uint8_t blk[PUMP_READ_LINEAR], msg[PUMP_READ_LINEAR];
    int16_t smp[PUMP_BLOCK];
    pthread_mutex_lock(&lk); pump_restart_locked(hook); pthread_mutex_unlock(&lk);
    for (;;) {
        struct pollfd pf[2] = { { cl, POLLIN, 0 }, { notify[0], POLLIN, 0 } };
        if (poll(pf, 2, 1000) < 0) { if (errno == EINTR) continue; return; }
        if (pf[1].revents & POLLIN) {
            char drain[256];
            while (read(notify[0], drain, sizeof drain) > 0) {}
        }
        for (;;) {
            unsigned len = 0;
            int got;
            pthread_mutex_lock(&lk);
            got = pump_read(&pump, smp);
            if (got) { len = pump_block_out(hook, lin, smp, blk); fresh = 0; }
            pthread_mutex_unlock(&lk);
            if (!got) break;
            if (send(cl, blk, len, MSG_NOSIGNAL) < 0) return;
        }
        if (pf[0].revents & (POLLHUP | POLLERR)) return;
        if (pf[0].revents & POLLIN) {
            ssize_t n = recv(cl, msg, sizeof msg, 0);
            if (n <= 0) return;
            if (n == 1) {
                pthread_mutex_lock(&lk);
                if (msg[0] == 'u' || msg[0] == 'a') want_law = msg[0] == 'a';
                else if (msg[0] == 'l' || msg[0] == 'c') want_lin = msg[0] == 'l';
                pthread_mutex_unlock(&lk);
            } else if (n == PUMP_BLOCK) {
                pthread_mutex_lock(&lk);
                /* an answer to a block read before the pump was restarted
                 * has no place in the new timing */
                if (fresh || pump_write(&pump, msg)) stale++;
                pthread_mutex_unlock(&lk);
            }
        }
    }
}

static void usage(void)
{
    fprintf(stderr, "usage: obiline [-v] [-s socket] [-S statsfile]\n");
    exit(2);
}

int main(int argc, char **argv)
{
    pthread_t at, st;
    struct sockaddr_un sa;
    int c, ls, fx1;
    while ((c = getopt(argc, argv, "vs:S:L:")) != -1) {
        switch (c) {
        case 'v': verbose = 1; break;
        case 'L': loopback = atoi(optarg); break;
        case 's': sock_path = optarg; break;
        case 'S': stats_path = optarg; break;
        default: usage();
        }
    }
    signal(SIGPIPE, SIG_IGN);
    if (mlockall(MCL_CURRENT | MCL_FUTURE) < 0) logm("mlockall: %s", strerror(errno));
    pump_lin16_table(0, lin16[0]);
    pump_lin16_table(1, lin16[1]);
    pump_start(&pump, idle_of(0), LATENCY);

    fx0.fd = open("/dev/fxs0", O_RDWR | O_CLOEXEC);
    fx1 = open("/dev/fxs1", O_RDWR | O_CLOEXEC);
    pcm_fd = open("/dev/pcm", O_RDWR | O_CLOEXEC);
    if (fx0.fd < 0 || fx1 < 0 || pcm_fd < 0) {
        logm("cannot open the telephony devices (is obiapp still running?): %s", strerror(errno));
        return 1;
    }
    bus0.xfer = fxs_xfer; bus0.msleep = fxs_msleep; bus0.ctx = &fx0;
    bus0.reg_rd = fxs_rd; bus0.reg_wr = fxs_wr;
    if (slic_family(&bus0) != SLIC_FAM_3226X)
        logm("warning: the part on /dev/fxs0 is not a Si3226x rev C (id %02x)", ps_rd(&bus0, PS_ID));

    if (pipe2(notify, O_CLOEXEC | O_NONBLOCK) < 0) { logm("pipe: %s", strerror(errno)); return 1; }

    /* the SLIC first, then the highway, then the line's own thread */
    slic_up = bring_up() == 0;
    if (slic_up) power_led();     /* the box is ready: as the maker's application shows it */
    pthread_mutex_lock(&lk); pump_restart_locked(0); pthread_mutex_unlock(&lk);
    ioctl(fx0.fd, FXS_TDM_PREP, 0); ioctl(fx1, FXS_TDM_PREP, 0);
    if (ioctl(fx0.fd, FXS_TDM_ON, 0) < 0 || ioctl(fx1, FXS_TDM_ON, 0) < 0) {
        logm("cannot start the TDM: %s", strerror(errno));
        return 1;
    }
    {
        /* small stacks: with mlockall every thread's stack is locked in full */
        pthread_attr_t ta;
        pthread_attr_init(&ta);
        pthread_attr_setstacksize(&ta, 128 * 1024);
        if (pthread_create(&at, &ta, audio_loop, NULL) || pthread_create(&st, &ta, slic_loop, NULL)) {
            logm("cannot start the threads");
            return 1;
        }
        pthread_attr_destroy(&ta);
    }
    helper_prio("socket");      /* this thread serves the daemon: see helper_prio */

    ls = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (ls < 0 || strlen(sock_path) >= sizeof sa.sun_path) { logm("socket: %s", strerror(errno)); return 1; }
    memset(&sa, 0, sizeof sa); sa.sun_family = AF_UNIX; strcpy(sa.sun_path, sock_path);
    unlink(sock_path);
    if (bind(ls, (struct sockaddr *)&sa, sizeof sa) < 0 || listen(ls, 1) < 0) {
        logm("%s: %s", sock_path, strerror(errno));
        return 1;
    }
    chmod(sock_path, 0600);
    logm("serving %s", sock_path);
    for (;;) {
        int cl = accept(ls, NULL, NULL);    /* no accept4 before Linux 2.6.36 on ARM */
        if (cl < 0) { if (errno == EINTR) continue; logm("accept: %s", strerror(errno)); sleep_ms(1000); continue; }
        fcntl(cl, F_SETFD, FD_CLOEXEC);
        if (verbose) logm("daemon connected");
        serve(cl);
        close(cl);
        if (verbose) logm("daemon gone");
    }
}
