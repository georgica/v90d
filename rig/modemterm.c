#include <stdio.h>
#include <string.h>
#include "modemterm.h"

/* G.711 mu-law on the V.90 Table 1 scale. Decoding is the library's; the
 * encoder truncates where the library's rounds to the nearest codeword,
 * and is kept as it was so that this rig sends what it always sent. */
static uint8_t linear_to_ulaw(int32_t v)
{
    int pos = v >= 0;
    uint32_t m = (uint32_t)(pos ? v : -v);
    unsigned seg = 0, u;
    m >>= 2;                 /* to the Table 1 pre-scale */
    if (m > 8158u) m = 8158u;
    m += 33u;                /* add bias */
    while ((m >> (seg + 6)) != 0 && seg < 7) seg++;
    u = (seg << 4) | ((m >> (seg + 1)) & 15u);
    return (uint8_t)((pos ? 0xFFu : 0x7Fu) - (u & 0x7Fu));
}

static void say(mterm_t *m, const char *s)
{
    m->tx_chars += (unsigned long)v90m_async_write(m->dte, (const uint8_t *)s, strlen(s));
}

void mterm_init(mterm_t *m, int level, unsigned modulations, void (*log)(const char *))
{
    v90m_cfg cfg;
    m->dte = v90m_async_init(m->dte_mem.b, sizeof m->dte_mem.b, NULL);
    v90m_cfg_default(&cfg);
    cfg.tx_level = level; cfg.modulations = modulations;
    cfg.echo_delay = 0;      /* the modem's defaults as they always were here */
    cfg.getbit = v90m_async_getbit; cfg.putbit = v90m_async_putbit; cfg.user = m->dte;
    m->ans = v90m_init(m->ans_mem.b, sizeof m->ans_mem.b, &cfg, NULL);
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

    v90m_state st;

    for (i = 0; i < n; i++) {
        int16_t rx = v90m_g711_decode(0, in[i]), tx;
        v90m_process_linear(m->ans, &rx, &tx, 1);
        out[i] = linear_to_ulaw(tx);
    }
    m->frames++;
    v90m_get_state(m->ans, &st);

    if (st.connected && !m->was_connected) {
        m->was_connected = 1;
        snprintf(l, sizeof l, "CONNECT %d (%s)", st.tx_bps, modname[st.modulation & 3]);
        if (m->log) m->log(l);
        snprintf(l, sizeof l, "\r\nv90d CONNECT %d %s\r\ntype something, it comes back\r\n",
                 st.tx_bps, modname[st.modulation & 3]);
        say(m, l);
    }
    if (st.end != V90M_END_NONE && !m->reported_fail) {
        m->reported_fail = 1;
        if (m->log) m->log(m->was_connected ? "carrier lost" : "no connection");
    }
    while ((k = (int)v90m_async_read(m->dte, buf, sizeof buf)) > 0) {
        m->rx_chars += (unsigned long)k;
        m->tx_chars += (unsigned long)v90m_async_write(m->dte, buf, (size_t)k);      /* echo */
        for (i = 0; i < k; i++) {
            if (buf[i] == '\r' || buf[i] == '\n') { flush_line(m); continue; }
            m->linebuf[m->linelen++] = (buf[i] >= 32 && buf[i] < 127) ? (char)buf[i] : '.';
            if (m->linelen >= (int)sizeof m->linebuf - 1) flush_line(m);
        }
    }
}
