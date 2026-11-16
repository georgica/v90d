#include <stdio.h>
#include <string.h>
#include "modemterm.h"
#include "../sim/g711.h"

static void say(mterm_t *m, const char *s)
{
    m->tx_chars += (unsigned long)v14_write(&m->dte, (const uint8_t *)s, (int)strlen(s));
}

void mterm_init(mterm_t *m, int level, unsigned modes, void (*log)(const char *))
{
    answer_cfg cfg;
    v14_init(&m->dte);
    memset(&cfg, 0, sizeof cfg);
    cfg.level = level; cfg.modes = modes;
    cfg.getbit = v14_getbit; cfg.putbit = v14_putbit; cfg.user = &m->dte;
    answer_init(&m->ans, &cfg);
    m->was_connected = 0; m->reported_fail = 0;
    m->rx_chars = m->tx_chars = m->frames = 0;
    m->log = log; m->linelen = 0;
}

static void flush_line(mterm_t *m)
{
    char l[128];
    if (!m->linelen) return;
    m->linebuf[m->linelen] = 0;
    snprintf(l, sizeof l, "rx: %s", m->linebuf);
    if (m->log) m->log(l);
    m->linelen = 0;
}

void mterm_frame(mterm_t *m, const uint8_t *in, uint8_t *out, int n)
{
    static const char *modname[4] = { "none", "V.21", "V.22", "V.22bis" };
    uint8_t buf[64];
    char l[128];
    int i, k;

    for (i = 0; i < n; i++)
        out[i] = linear_to_ulaw(answer_tick(&m->ans, (int16_t)ulaw_to_linear(in[i])));
    m->frames++;

    if (m->ans.connected && !m->was_connected) {
        m->was_connected = 1;
        snprintf(l, sizeof l, "CONNECT %d (%s)", m->ans.bitrate, modname[m->ans.modulation & 3]);
        if (m->log) m->log(l);
        snprintf(l, sizeof l, "\r\nv90d CONNECT %d %s\r\ntype something, it comes back\r\n",
                 m->ans.bitrate, modname[m->ans.modulation & 3]);
        say(m, l);
    }
    if (m->ans.phase == ANS_FAILED && !m->reported_fail) {
        m->reported_fail = 1;
        if (m->log) m->log(m->was_connected ? "carrier lost" : "no connection");
    }
    while ((k = v14_read(&m->dte, buf, sizeof buf)) > 0) {
        m->rx_chars += (unsigned long)k;
        m->tx_chars += (unsigned long)v14_write(&m->dte, buf, k);      /* echo */
        for (i = 0; i < k; i++) {
            if (buf[i] == '\r' || buf[i] == '\n') { flush_line(m); continue; }
            m->linebuf[m->linelen++] = (buf[i] >= 32 && buf[i] < 127) ? (char)buf[i] : '.';
            if (m->linelen >= (int)sizeof m->linebuf - 1) flush_line(m);
        }
    }
}
