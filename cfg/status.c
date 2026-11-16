/* v90d/cfg - status struct helpers and its HTML and JSON renderers.
 * See status.h. Everything goes through the bounded builder in sbuf.h. */
#include <string.h>
#include "status.h"
#include "sbuf.h"
#include "page.h"

#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* names: machine form for JSON, display form for HTML */
static const char *const line_names[]  = { "idle", "ringing", "negotiating", "connected", "hanging up" };
static const char *const mod_json[]    = { "none", "v90", "v34", "v32bis", "v22bis", "v22", "v21", "v92" };
static const char *const mod_html[]    = { "none", "V.90", "V.34", "V.32bis", "V.22bis", "V.22", "V.21", "V.92" };
static const char *const v42_json[]    = { "off", "detect", "lapm", "async" };
static const char *const v42_html[]    = { "off", "detecting", "LAPM", "async fallback" };
static const char *const ppp_names[]   = { "dead", "establish", "authenticate", "network",
                                           "running", "terminate" };
static const char *const end_json[]    = { "none", "caller_hangup", "carrier_lost", "train_failed",
                                           "auth_failed", "ppp_closed", "echo_timeout", "local",
                                           "error" };
static const char *const end_html[]    = { "-", "caller hung up", "carrier lost", "negotiation failed",
                                           "authentication failed", "closed by peer",
                                           "echo timeout", "cleared locally", "error" };
static const char *const tel_json[]    = { "answered", "busy", "wrong_number", "abandoned",
                                           "dropped", "session_limit", "idle" };
static const char *const tel_html[]    = { "answered", "busy", "wrong number", "abandoned",
                                           "line dropped", "session limit", "idle timeout" };

static const char *name_of(const char *const *names, int count, long v)
{
    return (v >= 0 && v < count) ? names[v] : "unknown";
}

void status_init(status_t *s)
{
    memset(s, 0, sizeof *s);
}

void status_add_call(status_t *s, const st_call_t *call)
{
    s->call_next %= STATUS_MAX_CALLS;
    if (s->n_calls > STATUS_MAX_CALLS) s->n_calls = STATUS_MAX_CALLS;
    s->calls[s->call_next] = *call;
    s->call_next = (s->call_next + 1u) % STATUS_MAX_CALLS;
    if (s->n_calls < STATUS_MAX_CALLS) s->n_calls++;
    s->calls_total++;
}

static unsigned kept(const status_t *s)
{
    return s->n_calls > STATUS_MAX_CALLS ? STATUS_MAX_CALLS : s->n_calls;
}

const st_call_t *status_call(const status_t *s, unsigned i)
{
    if (i >= kept(s)) return NULL;
    return &s->calls[(s->call_next % STATUS_MAX_CALLS + STATUS_MAX_CALLS - 1u - i) % STATUS_MAX_CALLS];
}

/* ---- pieces --------------------------------------------------------------- */

/* UTC date and time of t (seconds since 1970) with the given separators.
 * Day count to civil date by the era method: shift the epoch to 1 March
 * 0000 so that leap days fall at the end of a 400-year cycle. */
static void put_time(sbuf_t *b, uint32_t t, char sep, const char *tail)
{
    uint32_t days = t / 86400u, sod = t % 86400u;
    uint32_t z = days + 719468u;
    uint32_t era = z / 146097u;
    uint32_t doe = z - era * 146097u;                                   /* 0..146096 */
    uint32_t yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    uint32_t doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);          /* from 1 March */
    uint32_t mp = (5u * doy + 2u) / 153u;
    uint32_t d = doy - (153u * mp + 2u) / 5u + 1u;
    uint32_t m = mp < 10u ? mp + 3u : mp - 9u;
    uint32_t y = yoe + era * 400u + (m <= 2u ? 1u : 0u);

    sb_pad(b, y, 4); sb_putc(b, '-'); sb_pad(b, m, 2); sb_putc(b, '-'); sb_pad(b, d, 2);
    sb_putc(b, sep);
    sb_pad(b, sod / 3600u, 2); sb_putc(b, ':');
    sb_pad(b, sod / 60u % 60u, 2); sb_putc(b, ':');
    sb_pad(b, sod % 60u, 2);
    sb_puts(b, tail);
}

/* H:MM:SS */
static void put_hms(sbuf_t *b, uint32_t sec, int pad_hours)
{
    if (pad_hours) sb_pad(b, sec / 3600u, 2); else sb_u64(b, sec / 3600u);
    sb_putc(b, ':'); sb_pad(b, sec / 60u % 60u, 2);
    sb_putc(b, ':'); sb_pad(b, sec % 60u, 2);
}

/* octets from outside, at most max of them, stopping at NUL */
static void html_text(sbuf_t *b, const char *z, size_t max)
{
    sb_html(b, z, max, 0);
}

/* The same as a JSON string with its quotes. Besides what JSON demands,
 * < > & are written as \u escapes so the text stays inert if somebody
 * pastes it into a page, and 8-bit octets become \u00HH (the name is an
 * octet string, not known to be UTF-8). */
static void json_text(sbuf_t *b, const char *z, size_t max)
{
    size_t i;
    sb_putc(b, '"');
    for (i = 0; i < max && z[i]; i++) {
        unsigned ch = (unsigned char)z[i];
        if (ch == '"' || ch == '\\') { sb_putc(b, '\\'); sb_putc(b, (char)ch); }
        else if (ch < 0x20 || ch > 0x7e || ch == '<' || ch == '>' || ch == '&') {
            sb_puts(b, "\\u00"); sb_hex2(b, ch);
        }
        else sb_putc(b, (char)ch);
    }
    sb_putc(b, '"');
}

/* ---- JSON ----------------------------------------------------------------- */

static void j_key(sbuf_t *b, const char *k)
{
    sb_putc(b, '"'); sb_puts(b, k); sb_puts(b, "\":");
}

static void j_num(sbuf_t *b, const char *k, uint64_t v, const char *tail)
{
    j_key(b, k); sb_u64(b, v); sb_puts(b, tail);
}

static void j_str(sbuf_t *b, const char *k, const char *v, const char *tail)
{
    j_key(b, k); sb_putc(b, '"'); sb_puts(b, v); sb_putc(b, '"'); sb_puts(b, tail);
}

static void j_ip(sbuf_t *b, const char *k, uint32_t a, const char *tail)
{
    j_key(b, k);
    if (a) { sb_putc(b, '"'); sb_ip(b, a); sb_putc(b, '"'); }
    else sb_puts(b, "null");
    sb_puts(b, tail);
}

static const char *const comp_json[] = { "none", "deflate", "mppc" };

int status_render_json(const status_t *s, char *buf, size_t cap)
{
    sbuf_t b;
    unsigned i, n = kept(s);

    sb_init(&b, buf, cap);
    sb_putc(&b, '{');
    j_num(&b, "uptime_s", s->uptime_s, ",");
    j_key(&b, "service"); sb_puts(&b, s->service == 1 ? "\"archive\"," : s->service == 2 ? "\"dreamcast\"," : "\"internet\",");
    j_key(&b, "v92"); sb_puts(&b, s->v92 ? "true," : "false,");
    j_key(&b, "ppp_compression"); sb_puts(&b, s->ppp_compression ? "true," : "false,");
    j_str(&b, "line", name_of(line_names, COUNT(line_names), (long)s->line), ",");
    j_str(&b, "modulation", name_of(mod_json, COUNT(mod_json), (long)s->modulation), ",");
    j_num(&b, "down_bps", s->down_bps, ",");
    j_num(&b, "up_bps", s->up_bps, ",");
    j_num(&b, "rtd_ms", s->rtd_ms, ",");
    j_key(&b, "rx_level_dbm0"); sb_fix1(&b, s->rx_level_x10); sb_putc(&b, ',');
    j_key(&b, "snr_db"); sb_fix1(&b, s->snr_x10); sb_putc(&b, ',');
    j_str(&b, "v42", name_of(v42_json, COUNT(v42_json), (long)s->v42), ",");
    j_key(&b, "compression"); sb_puts(&b, s->compression ? "true," : "false,");

    j_key(&b, "ppp"); sb_putc(&b, '{');
    j_str(&b, "phase", name_of(ppp_names, COUNT(ppp_names), (long)s->ppp), ",");
    j_key(&b, "peer_name"); json_text(&b, s->peer_name, sizeof s->peer_name); sb_putc(&b, ',');
    j_ip(&b, "peer_ip", s->peer_ip, ",");
    j_ip(&b, "local_ip", s->local_ip, ",");
    j_str(&b, "comp_tx", name_of(comp_json, COUNT(comp_json), (long)s->ppp_comp_tx), ",");
    j_str(&b, "comp_rx", name_of(comp_json, COUNT(comp_json), (long)s->ppp_comp_rx), ",");
    j_num(&b, "comp_tx_in", s->comp_tx_in, ",");
    j_num(&b, "comp_tx_out", s->comp_tx_out, ",");
    j_num(&b, "comp_rx_in", s->comp_rx_in, ",");
    j_num(&b, "comp_rx_out", s->comp_rx_out, "},");

    j_key(&b, "counters"); sb_putc(&b, '{');
    j_num(&b, "rx_octets", s->rx_octets, ",");
    j_num(&b, "tx_octets", s->tx_octets, ",");
    j_num(&b, "rx_packets", s->rx_packets, ",");
    j_num(&b, "tx_packets", s->tx_packets, ",");
    j_num(&b, "rx_errors", s->rx_errors, ",");
    j_num(&b, "tx_errors", s->tx_errors, "},");

    j_num(&b, "calls_total", s->calls_total, ",");
    j_key(&b, "calls"); sb_putc(&b, '[');
    for (i = 0; i < n; i++) {
        const st_call_t *c = status_call(s, i);
        if (i) sb_putc(&b, ',');
        sb_putc(&b, '{');
        j_key(&b, "start");
        if (c->start_time) { sb_putc(&b, '"'); put_time(&b, c->start_time, 'T', "Z\","); }
        else sb_puts(&b, "null,");
        j_num(&b, "start_unix", c->start_time, ",");
        j_num(&b, "duration_s", c->duration_s, ",");
        j_key(&b, "dialed"); json_text(&b, c->dialed, sizeof c->dialed); sb_putc(&b, ',');
        j_str(&b, "tel_result", name_of(tel_json, COUNT(tel_json), (long)c->tel_result), ",");
        j_str(&b, "modulation", name_of(mod_json, COUNT(mod_json), (long)c->modulation), ",");
        j_num(&b, "down_bps", c->down_bps, ",");
        j_num(&b, "up_bps", c->up_bps, ",");
        j_str(&b, "end", name_of(end_json, COUNT(end_json), (long)c->end), "}");
    }
    sb_puts(&b, "]}\n");
    return sb_end(&b);
}

/* ---- HTML ----------------------------------------------------------------- */

static void h_open(sbuf_t *b, const char *label)
{
    sb_puts(b, "<tr><th>"); sb_puts(b, label); sb_puts(b, "</th><td>");
}

static void h_close(sbuf_t *b) { sb_puts(b, "</td></tr>\n"); }

static void h_str(sbuf_t *b, const char *label, const char *v)
{
    h_open(b, label); sb_puts(b, v); h_close(b);
}

static void h_num(sbuf_t *b, const char *label, uint64_t v, const char *unit)
{
    h_open(b, label); sb_u64(b, v); sb_puts(b, unit); h_close(b);
}

/* One direction of PPP compression: the algorithm and, once something has
 * gone through it, what it made of it (plain octets, the octets on the
 * link, and the ratio cut to one decimal). */
static void h_comp(sbuf_t *b, const char *label, int alg, uint64_t plain, uint64_t packed, const char *verb)
{
    h_open(b, label);
    if (alg != ST_COMP_DEFLATE && alg != ST_COMP_MPPC) {
        sb_puts(b, "no");
    } else {
        sb_puts(b, alg == ST_COMP_DEFLATE ? "Deflate" : "MPPC");
        if (plain && packed) {
            uint64_t r = plain / packed >= 1000u ? 9999u : plain * 10u / packed;
            sb_puts(b, ": "); sb_u64(b, plain); sb_puts(b, " octets "); sb_puts(b, verb);
            sb_puts(b, " as "); sb_u64(b, packed);
            sb_puts(b, " ("); sb_fix1(b, (int32_t)r); sb_puts(b, " to 1)");
        }
    }
    h_close(b);
}

static void h_fix(sbuf_t *b, const char *label, int32_t v, const char *unit)
{
    h_open(b, label); sb_fix1(b, v); sb_puts(b, unit); h_close(b);
}

static void h_ip(sbuf_t *b, const char *label, uint32_t a)
{
    h_open(b, label);
    if (a) sb_ip(b, a); else sb_putc(b, '-');
    h_close(b);
}

/* a tile of the dashboard: label, then the figure in <b> (left open) */
static void tile_open(sbuf_t *b, const char *label)
{
    sb_puts(b, "<div class=\"k\"><small>"); sb_puts(b, label); sb_puts(b, "</small><b>");
}

/* ... closed by a bar that shows v as a part of max */
static void tile_bar(sbuf_t *b, uint64_t v, uint64_t max)
{
    sb_puts(b, "<div class=\"m\"><i style=\"width:");
    sb_u64(b, v >= max ? 100u : v * 100u / max);
    sb_puts(b, "%\"></i></div></div>\n");
}

/* the state of a line at a glance: a badge, and one line about the call;
 * with a heading when there is more than one line */
static void hero(sbuf_t *bp, const status_t *s, const char *heading)
{
    sb_puts(bp, "<section class=\"hero\">");
    if (heading) { sb_puts(bp, "<h2>"); sb_puts(bp, heading); sb_puts(bp, "</h2>\n"); }
    sb_puts(bp, "<span class=\"b s");
    sb_u64(bp, (uint64_t)(s->line < (unsigned)COUNT(line_names) ? s->line : 0));
    sb_puts(bp, "\">"); sb_puts(bp, name_of(line_names, COUNT(line_names), (long)s->line));
    sb_puts(bp, "</span>\n<p class=\"sum\">");
    if (s->modulation) {
        sb_puts(bp, name_of(mod_html, COUNT(mod_html), (long)s->modulation));
        sb_puts(bp, " &middot; "); sb_u64(bp, s->down_bps);
        sb_puts(bp, " / "); sb_u64(bp, s->up_bps); sb_puts(bp, " bit/s");
        if (s->peer_name[0]) {
            sb_puts(bp, " &middot; "); html_text(bp, s->peer_name, sizeof s->peer_name);
        }
    } else {
        sb_puts(bp, s->line ? "Call being set up" : "No call in progress");
    }
    sb_puts(bp, "</p></section>\n");
}

static int render(const status_t *s, const status_t *const *ports, int sel, char *buf, size_t cap);

/* one row of a table of calls; port: 1 or 2 in a first cell, 0 for none */
static void put_call(sbuf_t *b, const st_call_t *c, int port)
{
    sb_puts(b, "<tr><td>");
    if (port) { sb_u64(b, (uint64_t)port); sb_puts(b, "</td><td>"); }
    if (c->start_time) put_time(b, c->start_time, ' ', ""); else sb_putc(b, '-');
    sb_puts(b, "</td><td>");
    put_hms(b, c->duration_s, 0);
    sb_puts(b, "</td><td>");
    if (c->dialed[0]) html_text(b, c->dialed, sizeof c->dialed); else sb_putc(b, '-');
    sb_puts(b, "</td><td>");
    sb_puts(b, name_of(tel_html, COUNT(tel_html), (long)c->tel_result));
    sb_puts(b, "</td><td>");
    sb_puts(b, name_of(mod_html, COUNT(mod_html), (long)c->modulation));
    sb_puts(b, "</td><td>");
    sb_u64(b, c->down_bps);
    sb_puts(b, "</td><td>");
    sb_u64(b, c->up_bps);
    sb_puts(b, "</td><td>");
    sb_puts(b, name_of(end_html, COUNT(end_html), (long)c->end));
    sb_puts(b, "</td></tr>\n");
}

int status_render_calls(const status_t *const ports[2], char *buf, size_t cap)
{
    sbuf_t b;
    unsigned at[2] = { 0, 0 }, have[2], rows;

    have[0] = kept(ports[0]);
    have[1] = ports[1] ? kept(ports[1]) : 0;
    sb_init(&b, buf, cap);
    page_top(&b, "calls", "<meta http-equiv=\"refresh\" content=\"15\">\n", PAGE_TAB_CALLS, NULL);
    sb_puts(&b, "<section><h2>Call history</h2>\n<div class=\"x\"><table>\n"
                "<tr><th>Port</th><th>Start (UTC)</th><th>Duration</th><th>Dialed</th><th>Result</th>"
                "<th>Modulation</th><th>Down bit/s</th><th>Up bit/s</th><th>Ended</th></tr>\n");
    /* each port's calls are newest first already: take the newer head */
    for (rows = 0; rows < STATUS_HISTORY && (at[0] < have[0] || at[1] < have[1]); rows++) {
        int p;
        if (at[0] >= have[0]) p = 1;
        else if (at[1] >= have[1]) p = 0;
        else p = status_call(ports[1], at[1])->start_time > status_call(ports[0], at[0])->start_time;
        put_call(&b, status_call(ports[p], at[p]), p + 1);
        at[p]++;
    }
    if (rows == 0) sb_puts(&b, "<tr><td colspan=\"9\">none yet</td></tr>\n");
    sb_puts(&b, "</table></div>\n<p>The last ");
    sb_u64(&b, STATUS_HISTORY);
    sb_puts(&b, " calls of both ports, newest first. They are kept in memory: a restart of the box empties the list.</p>"
                "</section>\n" PAGE_END);
    return sb_end(&b);
}

int status_render_html(const status_t *s, char *buf, size_t cap)
{
    return render(s, NULL, 0, buf, cap);
}

int status_render_ports(const status_t *const ports[2], int sel, char *buf, size_t cap)
{
    sel = sel == 1;
    return render(ports[sel], ports, sel, buf, cap);
}

/* the page about line *s; ports: both lines of a two-port box, or NULL */
static int render(const status_t *s, const status_t *const *ports, int sel, char *buf, size_t cap)
{
    sbuf_t b;

    sb_init(&b, buf, cap);
    page_top(&b, "status", "<meta http-equiv=\"refresh\" content=\"5\">\n", 0, NULL);

    if (ports) {
        sb_puts(&b, "<div class=\"g\">\n");
        hero(&b, ports[0], "Port 1");
        hero(&b, ports[1], "Port 2");
        sb_puts(&b, "</div>\n<p class=\"sw\">");
        sb_puts(&b, sel ? "<a href=\"/\">Port 1</a> <b>Port 2</b>" : "<b>Port 1</b> <a href=\"/port2\">Port 2</a>");
        sb_puts(&b, "</p>\n");
    } else {
        hero(&b, s, NULL);
    }

    /* the tiles */
    sb_puts(&b, "<div class=\"t\">\n");
    tile_open(&b, "Downstream"); sb_u64(&b, s->down_bps); sb_puts(&b, "</b> bit/s");
    tile_bar(&b, s->down_bps, 56000u);
    tile_open(&b, "Upstream"); sb_u64(&b, s->up_bps); sb_puts(&b, "</b> bit/s");
    /* the scale is the modulation's ceiling: 48000 with PCM upstream (V.92 5.2) */
    tile_bar(&b, s->up_bps, s->modulation == ST_MOD_V92 ? 48000u : 33600u);
    tile_open(&b, "Signal to noise"); sb_fix1(&b, s->snr_x10); sb_puts(&b, "</b> dB");
    tile_bar(&b, s->snr_x10 > 0 ? (uint64_t)s->snr_x10 : 0u, 450u);
    tile_open(&b, "Uptime");
    sb_u64(&b, s->uptime_s / 86400u); sb_puts(&b, "d ");
    put_hms(&b, s->uptime_s % 86400u, 1);
    sb_puts(&b, "</b></div>\n");
    tile_open(&b, "Calls"); sb_u64(&b, s->calls_total); sb_puts(&b, "</b></div>\n");
    sb_puts(&b, "</div>\n");

    sb_puts(&b, "<section><h2>Line</h2>\n<table>\n");
    h_str(&b, "State", name_of(line_names, COUNT(line_names), (long)s->line));
    h_str(&b, "Modulation", name_of(mod_html, COUNT(mod_html), (long)s->modulation));
    h_num(&b, "Downstream", s->down_bps, " bit/s");
    h_num(&b, "Upstream", s->up_bps, " bit/s");
    h_num(&b, "Round-trip delay", s->rtd_ms, " ms");
    h_fix(&b, "Receive level", s->rx_level_x10, " dBm0");
    h_fix(&b, "Receive SNR", s->snr_x10, " dB");
    h_str(&b, "V.42", name_of(v42_html, COUNT(v42_html), (long)s->v42));
    h_str(&b, "Compression", s->compression ? "on" : "off");
    sb_puts(&b, "</table></section>\n");

    sb_puts(&b, "<div class=\"g\">\n<section><h2>PPP</h2>\n<table>\n");
    h_str(&b, "Phase", name_of(ppp_names, COUNT(ppp_names), (long)s->ppp));
    h_open(&b, "Peer name"); html_text(&b, s->peer_name, sizeof s->peer_name); h_close(&b);
    h_ip(&b, "Peer address", s->peer_ip);
    h_ip(&b, "Local address", s->local_ip);
    h_comp(&b, "Compressed to caller", s->ppp_comp_tx, s->comp_tx_in, s->comp_tx_out, "sent");
    h_comp(&b, "Compressed from caller", s->ppp_comp_rx, s->comp_rx_out, s->comp_rx_in, "received");
    h_num(&b, "Calls", s->calls_total, "");
    h_str(&b, "Service", s->service == 1 ? "Archive (Protoweb)" : s->service == 2 ? "Dreamcast" : "Internet");
    h_str(&b, "V.92", s->v92 ? "on" : "off");
    h_str(&b, "PPP compression", s->ppp_compression ? "on" : "off");
    h_open(&b, "Uptime");
    sb_u64(&b, s->uptime_s / 86400u); sb_puts(&b, "d ");
    put_hms(&b, s->uptime_s % 86400u, 1);
    h_close(&b);
    sb_puts(&b, "</table></section>\n");

    sb_puts(&b, "<section><h2>Counters</h2>\n<table>\n");
    h_num(&b, "Octets received", s->rx_octets, "");
    h_num(&b, "Octets sent", s->tx_octets, "");
    h_num(&b, "Packets received", s->rx_packets, "");
    h_num(&b, "Packets sent", s->tx_packets, "");
    h_num(&b, "Receive errors", s->rx_errors, "");
    h_num(&b, "Transmit errors", s->tx_errors, "");
    sb_puts(&b, "</table></section>\n</div>\n");

    sb_puts(&b, PAGE_END);
    return sb_end(&b);
}
