/* The time service on the box's lines (actsvc.h). Ours; written for this project. */
#include <string.h>
#include <time.h>
#include "daemon.h"
#include "actsvc.h"
#include "acts.h"

static struct {
    int      on;
    acts_t   a;
    uint8_t  pend[256];     /* handed out by acts_poll, not yet taken by the line */
    size_t   pn, pa;
} A[PORTS];

/* the box's clock (NTP), microseconds since the Unix epoch */
static int64_t utc_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

void actsvc_from_modem(void *ctx, const uint8_t *oct, size_t n)
{
    int k = (int)(((srv_t *)ctx) - S);
    if (k < 0 || k >= PORTS || !A[k].on) return;
    acts_rx(&A[k].a, oct, n, utc_us());
}

void actsvc_poll(void)
{
    int k;
    for (k = 0; k < n_ports; k++) {
        uint32_t addr; int online, kind = srv_link(&S[k], &addr, &online);
        int64_t now, next;
        if (kind != SRV_LINK_ACTS) {
            if (A[k].on) A[k].on = 0;
            continue;
        }
        if (!online) continue;
        now = utc_us();
        if (!A[k].on) {
            acts_cfg c;
            memset(&c, 0, sizeof c);
            dce_status_t ds;
            dce_get_status(&S[k].dce, &ds);         /* now: S[k].ds is refreshed once a second */
            c.bps = ds.bitrate > 0 ? ds.bitrate : 300;
            acts_init(&A[k].a, &c, now);
            A[k].on = 1; A[k].pn = A[k].pa = 0;
            logf_(CFG_LOG_INFO, "port %d: time service at %d bit/s", k + 1, c.bps);
        }
        for (;;) {
            size_t took;
            if (A[k].pa == A[k].pn) { A[k].pa = 0; A[k].pn = acts_poll(&A[k].a, now, A[k].pend, sizeof A[k].pend, &next); }
            if (A[k].pa == A[k].pn) break;
            took = srv_send_raw(&S[k], A[k].pend + A[k].pa, A[k].pn - A[k].pa);
            if (!took) break;
            A[k].pa += took;
        }
        if (acts_done(&A[k].a) && A[k].pa == A[k].pn) {
            logf_(CFG_LOG_INFO, "port %d: time service: done; msADV %d.%d ms, %s", k + 1,
                  A[k].a.adv_us / 1000, A[k].a.adv_us / 100 % 10,
                  A[k].a.agree >= ACTS_AGREE_N ? "measured from the caller's echoes (#)" :
                  A[k].a.agree ? "echoes that did not agree three times" : "no echoes");
            A[k].on = 0;
            srv_hangup(&S[k]);
        }
    }
}
