/* REAL-MODEM TEST RIG (not part of the shipped image): the answering modem
 * behind the STOCK firmware's SIP/RTP path, so a hardware modem on FXS port 1
 * can call it before our own SLIC driver exists. Mode "modem" (-m modem, the
 * default here) runs rig/modemterm on the RTP audio; the other modes are the
 * stage 0 loopback tools this file started from.
 *
 * Original header:
 * Stage 0b/0c: minimal on-box SIP/RTP endpoint for the HT802 feasibility gate.
 *
 * Answers any INVITE with PCMU only, then for every RTP packet received sends
 * exactly one RTP packet back (slaved to the far end's clock; no timers, no
 * jitter buffer). Logs packet timing so gaps, slips and payload-type changes in
 * the firmware's RTP stream are visible.
 *
 * TX payload modes:  echo     send the received payload back
 *                    silence  send 0xFF (mu-law +0)
 *                    file     play a raw mu-law file (stage 0c test patterns)
 * Optional: dump received payload bytes to a raw mu-law file.
 *
 * Portable C99 + POSIX sockets. No float, no malloc. Not a modem.
 */
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "modemterm.h"
#include "pppcall.h"

#define BUFSZ 4096
#define HDRSZ 2048

enum { MODE_ECHO, MODE_SILENCE, MODE_FILE, MODE_MODEM, MODE_PPP };

static mterm_t mt;
static int mt_level = 3000;
static unsigned mt_modes = V90M_MOD_V22 | V90M_MOD_V21;
static pppcall_t pc;
static const char *ppp_user, *ppp_pass;

static int mode = MODE_MODEM, loop_file = 0, verbose = 0;
static int sip_port = 5062, rtp_port = 40000, log_ms = 1000;
static const char *adv_ip = "127.0.0.1";
static int tx_fd = -1, rx_fd = -1;
static int sip_sock = -1, rtp_sock = -1;
static uint64_t t_start;

/* RTP state */
static uint16_t tx_seq;
static uint32_t tx_ts, tx_ssrc = 0x76393064u; /* "v90d" */
static int have_rx;
static uint16_t last_seq;
static uint32_t last_ts, last_ssrc;
static unsigned last_len;
static uint64_t last_arr, call_start;

struct stats {
    unsigned pkts, lost, misorder, dup, bad_pt, bad_len, ts_jump, marker, ssrc_chg, short_pkt;
    unsigned ia_min, ia_max;
    uint64_t ia_sum;
    unsigned hist[6]; /* <15, 15-19, 19-21, 21-25, 25-40, >=40 ms */
    int last_bad_pt;
    unsigned tx_file_frames, tx_file_wraps;
};
static struct stats iv, tot;
static uint64_t iv_start;

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)(ts.tv_nsec / 1000);
}

static void logts(void)
{
    uint64_t t = now_us() - t_start;
    printf("[%6lu.%03lu] ", (unsigned long)(t / 1000000u), (unsigned long)(t / 1000u % 1000u));
}

static void stats_reset(struct stats *s)
{
    memset(s, 0, sizeof(*s));
    s->ia_min = 0xffffffffu;
    s->last_bad_pt = -1;
}

static void stats_print(const char *tag, const struct stats *s, unsigned ms)
{
    unsigned n = s->pkts > 1 ? s->pkts - 1 : 1;
    logts();
    printf("%s %ums: pkts=%u (%u.%u/s) lost=%u misorder=%u dup=%u ts_jump=%u badpt=%u(last %d) "
           "badlen=%u marker=%u ssrc_chg=%u | interarrival us min=%u mean=%u max=%u | "
           "hist<15/15-19/19-21/21-25/25-40/>=40ms=%u/%u/%u/%u/%u/%u",
           tag, ms, s->pkts, ms ? s->pkts * 1000u / ms : 0, ms ? (s->pkts * 10000u / ms) % 10u : 0,
           s->lost, s->misorder, s->dup, s->ts_jump, s->bad_pt, s->last_bad_pt, s->bad_len,
           s->marker, s->ssrc_chg, s->ia_min == 0xffffffffu ? 0 : s->ia_min,
           (unsigned)(s->ia_sum / n), s->ia_max,
           s->hist[0], s->hist[1], s->hist[2], s->hist[3], s->hist[4], s->hist[5]);
    if (mode == MODE_FILE)
        printf(" | txfile frames=%u wraps=%u", s->tx_file_frames, s->tx_file_wraps);
    printf("\n");
    fflush(stdout);
}

static void stats_ia(struct stats *s, unsigned ia)
{
    unsigned ms = ia / 1000u;
    if (ia < s->ia_min) s->ia_min = ia;
    if (ia > s->ia_max) s->ia_max = ia;
    s->ia_sum += ia;
    s->hist[ms < 15 ? 0 : ms < 19 ? 1 : ms < 21 ? 2 : ms < 25 ? 3 : ms < 40 ? 4 : 5]++;
}

static void mt_log(const char *line)
{
    logts(); printf("MODEM %s\n", line); fflush(stdout);
}

/* ------------------------------------------------------------------ RTP */

static void fill_tx(uint8_t *out, const uint8_t *rx, unsigned len)
{
    unsigned got = 0;
    if (mode == MODE_MODEM) {
        mterm_frame(&mt, rx, out, (int)len);
        return;
    }
    if (mode == MODE_PPP) {
        pppcall_frame(&pc, rx, out, (int)len);
        return;
    }
    if (mode == MODE_ECHO) {
        memcpy(out, rx, len);
        return;
    }
    memset(out, 0xFF, len);
    if (mode != MODE_FILE || tx_fd < 0)
        return;
    while (got < len) {
        ssize_t r = read(tx_fd, out + got, len - got);
        if (r > 0) {
            got += (unsigned)r;
        } else if (loop_file && lseek(tx_fd, 0, SEEK_SET) == 0) {
            iv.tx_file_wraps++; tot.tx_file_wraps++;
        } else {
            break; /* EOF: rest of frame and all later frames are silence */
        }
    }
    if (got) { iv.tx_file_frames++; tot.tx_file_frames++; }
}

static void on_rtp(void)
{
    uint8_t in[BUFSZ], out[BUFSZ];
    struct sockaddr_in src;
    socklen_t sl = sizeof(src);
    ssize_t n = recvfrom(rtp_sock, in, sizeof(in), 0, (struct sockaddr *)&src, &sl);
    uint64_t arr = now_us();
    unsigned hdr, len, pt, cc;
    uint16_t seq;
    uint32_t ts, ssrc;

    if (n < 12 || (in[0] & 0xC0) != 0x80) {
        iv.short_pkt++; tot.short_pkt++;
        return;
    }
    cc = in[0] & 0x0F;
    hdr = 12 + 4 * cc;
    if (in[0] & 0x10) { /* extension */
        if ((unsigned)n < hdr + 4) return;
        hdr += 4 + 4 * (((unsigned)in[hdr + 2] << 8) | in[hdr + 3]);
    }
    if ((unsigned)n < hdr) return;
    len = (unsigned)n - hdr;
    pt = in[1] & 0x7F;
    seq = (uint16_t)((in[2] << 8) | in[3]);
    ts = ((uint32_t)in[4] << 24) | ((uint32_t)in[5] << 16) | ((uint32_t)in[6] << 8) | in[7];
    ssrc = ((uint32_t)in[8] << 24) | ((uint32_t)in[9] << 16) | ((uint32_t)in[10] << 8) | in[11];

    iv.pkts++; tot.pkts++;
    if (in[1] & 0x80) { iv.marker++; tot.marker++; }
    if (pt != 0) {
        iv.bad_pt++; tot.bad_pt++; iv.last_bad_pt = tot.last_bad_pt = (int)pt;
        if (verbose) { logts(); printf("RTP pt=%u len=%u seq=%u\n", pt, len, seq); }
    } else if (len != 160) {
        iv.bad_len++; tot.bad_len++;
        if (verbose) { logts(); printf("RTP len=%u seq=%u\n", len, seq); }
    }
    if (have_rx) {
        uint16_t d = (uint16_t)(seq - last_seq);
        unsigned ia = (unsigned)(arr - last_arr);
        stats_ia(&iv, ia); stats_ia(&tot, ia);
        if (ssrc != last_ssrc) { iv.ssrc_chg++; tot.ssrc_chg++; }
        if (d == 0) { iv.dup++; tot.dup++; }
        else if (d > 0x8000u) { iv.misorder++; tot.misorder++; }
        else if (d > 1) {
            iv.lost += d - 1u; tot.lost += d - 1u;
            logts(); printf("RTP GAP: seq %u -> %u (%u missing), interarrival %u us\n", last_seq, seq, d - 1u, ia);
        }
        if (d == 1 && ts - last_ts != last_len) {
            iv.ts_jump++; tot.ts_jump++;
            logts(); printf("RTP TS JUMP: seq %u ts delta %ld, expected %u\n", seq, (long)(int32_t)(ts - last_ts), last_len);
        }
        if (ia >= 40000u && verbose) { logts(); printf("RTP late: %u us before seq %u\n", ia, seq); }
    } else {
        logts(); printf("RTP first packet from %s:%u pt=%u len=%u ssrc=%08lx\n", inet_ntoa(src.sin_addr),
                        ntohs(src.sin_port), pt, len, (unsigned long)ssrc);
        iv_start = call_start = arr;
        if (mode == MODE_MODEM) mterm_init(&mt, mt_level, mt_modes, mt_log);   /* a new call: answer it */
        if (mode == MODE_PPP) {
            pppcall_cfg c;
            memset(&c, 0, sizeof c);
            c.level = mt_level; c.modulations = mt_modes; c.user = ppp_user; c.pass = ppp_pass; c.log = mt_log;
            pppcall_init(&pc, &c);
        }
    }
    have_rx = 1; last_seq = seq; last_ts = ts; last_ssrc = ssrc; last_arr = arr; last_len = len;

    if (pt == 0 && len > 0 && rx_fd >= 0 && write(rx_fd, in + hdr, len) != (ssize_t)len) {
        logts(); printf("rx dump write failed, closing dump\n");
        close(rx_fd); rx_fd = -1;
    }

    /* One packet out per packet in. Non-audio packets (CN, telephone-event)
     * still clock one silent audio frame out so the stream never slips. */
    if (pt != 0 || len == 0 || len > 1024) len = 160;
    out[0] = 0x80; out[1] = 0;
    out[2] = (uint8_t)(tx_seq >> 8); out[3] = (uint8_t)tx_seq;
    out[4] = (uint8_t)(tx_ts >> 24); out[5] = (uint8_t)(tx_ts >> 16); out[6] = (uint8_t)(tx_ts >> 8); out[7] = (uint8_t)tx_ts;
    out[8] = (uint8_t)(tx_ssrc >> 24); out[9] = (uint8_t)(tx_ssrc >> 16); out[10] = (uint8_t)(tx_ssrc >> 8); out[11] = (uint8_t)tx_ssrc;
    if (pt != 0 && mode == MODE_MODEM) {
        static uint8_t quiet[1024];
        memset(quiet, 0xFF, len);
        fill_tx(out + 12, quiet, len);
    } else if (pt != 0 && mode == MODE_ECHO)
        memset(out + 12, 0xFF, len);
    else
        fill_tx(out + 12, in + hdr, len);
    sendto(rtp_sock, out, 12 + len, 0, (struct sockaddr *)&src, sl);
    tx_seq++; tx_ts += len;

    if (arr - iv_start >= (uint64_t)log_ms * 1000u) {
        stats_print("RTP", &iv, (unsigned)((arr - iv_start) / 1000u));
        stats_reset(&iv);
        iv_start = arr;
    }
}

/* ------------------------------------------------------------------ SIP */

static int has_tag(const char *line, size_t ll)
{
    size_t i;
    for (i = 0; i + 5 <= ll; i++)
        if (memcmp(line + i, ";tag=", 5) == 0) return 1;
    return 0;
}

static int hdr_is(const char *line, const char *name, char compact)
{
    size_t n = strlen(name);
    if (strncasecmp(line, name, n) == 0 && (line[n] == ':' || line[n] == ' '))
        return 1;
    return compact && (line[0] == compact || line[0] == (compact ^ 0x20)) && line[1] == ':';
}

static void call_end(const char *why)
{
    if (!have_rx) return;
    logts(); printf("call end (%s)\n", why);
    stats_print("RTP TOTAL", &tot, (unsigned)((last_arr - call_start) / 1000u));
    have_rx = 0;
    stats_reset(&iv); stats_reset(&tot);
    if (tx_fd >= 0) lseek(tx_fd, 0, SEEK_SET);
}

static void on_sip(void)
{
    static char in[BUFSZ], out[BUFSZ], hdrs[HDRSZ], body[512];
    char method[16], contact[300] = "";
    struct sockaddr_in src;
    socklen_t sl = sizeof(src);
    ssize_t n = recvfrom(sip_sock, in, sizeof(in) - 1, 0, (struct sockaddr *)&src, &sl);
    char *line, *next, *sdp;
    size_t hl = 0;
    int code = 200, with_sdp = 0, i, olen;
    const char *reason = "OK";

    if (n <= 0) return;
    in[n] = 0;
    if (strncmp(in, "SIP/2.0", 7) == 0) return; /* a response; we never send requests */
    for (i = 0; i < 15 && in[i] && in[i] != ' '; i++) method[i] = in[i];
    method[i] = 0;

    sdp = strstr(in, "\r\n\r\n");
    logts(); printf("SIP <- %s from %s:%u\n", method, inet_ntoa(src.sin_addr), ntohs(src.sin_port));
    if (verbose || strcmp(method, "INVITE") == 0) { printf("%s\n", in); fflush(stdout); }
    if (sdp) { *sdp = 0; sdp += 4; }

    hdrs[0] = 0;
    for (line = strstr(in, "\r\n"); line; line = next) {
        size_t ll;
        line += 2;
        next = strstr(line, "\r\n");
        ll = next ? (size_t)(next - line) : strlen(line);
        if (ll == 0) break;
        if (hdr_is(line, "Via", 'v') || hdr_is(line, "From", 'f') || hdr_is(line, "Call-ID", 'i') ||
            hdr_is(line, "CSeq", 0) || hdr_is(line, "Record-Route", 0) || hdr_is(line, "To", 't')) {
            int is_to = hdr_is(line, "To", 't');
            int add_tag = is_to && !has_tag(line, ll);
            if (hl + ll + 16 >= sizeof(hdrs)) return;
            memcpy(hdrs + hl, line, ll); hl += ll;
            if (add_tag) { memcpy(hdrs + hl, ";tag=v90d", 9); hl += 9; }
            memcpy(hdrs + hl, "\r\n", 2); hl += 2;
            hdrs[hl] = 0;
        } else if (hdr_is(line, "Contact", 'm') && ll < sizeof(contact) - 1) {
            memcpy(contact, line, ll); contact[ll] = 0;
        }
    }

    if (strcmp(method, "ACK") == 0) return;
    if (strcmp(method, "INVITE") == 0) {
        if (sdp && strstr(sdp, "m=image") && !strstr(sdp, "m=audio")) {
            code = 488; reason = "Not Acceptable Here";
            logts(); printf("re-INVITE offers image/T.38 only: refusing with 488\n");
        } else {
            with_sdp = 1;
            if (sdp && !strstr(sdp, " 0") ) { logts(); printf("WARNING: offer may not contain PCMU\n"); }
        }
    } else if (strcmp(method, "BYE") == 0 || strcmp(method, "CANCEL") == 0) {
        call_end(method);
    }

    body[0] = 0;
    if (with_sdp)
        snprintf(body, sizeof(body),
                 "v=0\r\no=v90d 1 1 IN IP4 %s\r\ns=v90d\r\nc=IN IP4 %s\r\nt=0 0\r\n"
                 "m=audio %d RTP/AVP 0\r\na=rtpmap:0 PCMU/8000\r\na=ptime:20\r\na=sendrecv\r\n",
                 adv_ip, adv_ip, rtp_port);

    olen = snprintf(out, sizeof(out), "SIP/2.0 %d %s\r\n%s", code, reason, hdrs);
    if (strcmp(method, "REGISTER") == 0 && contact[0])
        olen += snprintf(out + olen, sizeof(out) - (size_t)olen, "%s\r\nExpires: 3600\r\n", contact);
    else
        olen += snprintf(out + olen, sizeof(out) - (size_t)olen, "Contact: <sip:v90d@%s:%d>\r\n", adv_ip, sip_port);
    olen += snprintf(out + olen, sizeof(out) - (size_t)olen,
                     "Allow: INVITE, ACK, BYE, CANCEL, OPTIONS\r\nUser-Agent: v90d-stage0\r\n%s"
                     "Content-Length: %u\r\n\r\n%s",
                     with_sdp ? "Content-Type: application/sdp\r\n" : "", (unsigned)strlen(body), body);
    if (olen <= 0 || (size_t)olen >= sizeof(out)) return;
    sendto(sip_sock, out, (size_t)olen, 0, (struct sockaddr *)&src, sl);
    logts(); printf("SIP -> %d %s\n", code, reason);
    fflush(stdout);
}

static int udp_bind(int port)
{
    struct sockaddr_in a;
    int s = socket(AF_INET, SOCK_DGRAM, 0), one = 1;
    if (s < 0) { perror("socket"); exit(1); }
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((uint16_t)port);
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0) { perror("bind"); exit(1); }
    return s;
}

int main(int argc, char **argv)
{
    int c;
    const char *txf = NULL, *rxf = NULL;
    while ((c = getopt(argc, argv, "p:r:a:m:f:w:l:g:M:u:Lv")) != -1) {
        switch (c) {
        case 'p': sip_port = atoi(optarg); break;
        case 'r': rtp_port = atoi(optarg); break;
        case 'a': adv_ip = optarg; break;
        case 'm': mode = !strcmp(optarg, "echo") ? MODE_ECHO : !strcmp(optarg, "file") ? MODE_FILE :
                         !strcmp(optarg, "modem") ? MODE_MODEM : !strcmp(optarg, "ppp") ? MODE_PPP : MODE_SILENCE; break;
        case 'u': {                                           /* user:password for PPP (mode ppp) */
            static char up[130]; char *colon;
            strncpy(up, optarg, sizeof up - 1);
            colon = strchr(up, ':');
            if (colon) { *colon = 0; ppp_pass = colon + 1; }
            ppp_user = up;
            break;
        }
        case 'g': mt_level = atoi(optarg); break;             /* modem transmit RMS */
        case 'M': mt_modes = !strcmp(optarg, "v21") ? V90M_MOD_V21 : !strcmp(optarg, "v22") ? V90M_MOD_V22
                           : !strcmp(optarg, "v90") ? (V90M_MOD_V90 | V90M_MOD_V22 | V90M_MOD_V21)
                           : (V90M_MOD_V22 | V90M_MOD_V21); break;
        case 'f': txf = optarg; mode = MODE_FILE; break;
        case 'w': rxf = optarg; break;
        case 'l': log_ms = atoi(optarg); break;
        case 'L': loop_file = 1; break;
        case 'v': verbose = 1; break;
        default:
            fprintf(stderr, "usage: %s [-p sipport] [-r rtpport] [-a advertised-ip] [-m modem|ppp|echo|silence|file]\n"
                            "          [-g tx-rms (modem, default 3000)] [-M v21|v22|all|v90 (v90: mode ppp only)]\n"
                            "          [-u user:password (mode ppp; default: no authentication)]\n"
                            "          [-f tx.ulaw] [-L] [-w rxdump.ulaw] [-l log-ms] [-v]\n", argv[0]);
            return 2;
        }
    }
    if (txf && (tx_fd = open(txf, O_RDONLY)) < 0) { perror(txf); return 1; }
    if (rxf && (rx_fd = open(rxf, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0) { perror(rxf); return 1; }
    if (log_ms < 100) log_ms = 100;
    sip_sock = udp_bind(sip_port);
    rtp_sock = udp_bind(rtp_port);
    t_start = now_us();
    stats_reset(&iv); stats_reset(&tot);
    logts();
    printf("v90d rig endpoint: SIP udp/%d RTP udp/%d advertise %s mode %s%s\n", sip_port, rtp_port, adv_ip,
           mode == MODE_MODEM ? "modem" : mode == MODE_PPP ? "ppp" : mode == MODE_ECHO ? "echo" : mode == MODE_FILE ? "file" : "silence", loop_file ? " (loop)" : "");
    fflush(stdout);

    for (;;) {
        fd_set fds;
        struct timeval tv = { 2, 0 };
        int mx = sip_sock > rtp_sock ? sip_sock : rtp_sock;
        FD_ZERO(&fds); FD_SET(sip_sock, &fds); FD_SET(rtp_sock, &fds);
        if (select(mx + 1, &fds, NULL, NULL, &tv) < 0) continue;
        if (FD_ISSET(rtp_sock, &fds)) on_rtp();
        if (FD_ISSET(sip_sock, &fds)) on_sip();
        if (have_rx && now_us() - last_arr > 2000000u) { /* logging only; never drives audio */
            logts(); printf("RTP STALLED: nothing for 2 s\n");
            call_end("rtp timeout");
        }
    }
}
