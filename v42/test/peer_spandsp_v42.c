/* spandsp as an independent V.42 / V.42 bis peer for the interop tests. All
 * spandsp includes stay in this file. Uses only spandsp's public calls plus
 * its exposed state structures (spandsp/expose.h) to read the LAPM state and
 * to set the XID compression parameters, for which spandsp has no call. */
#include <string.h>
#include <spandsp.h>
#include <spandsp/expose.h>
#include "peer_spandsp_v42.h"

#define SP_LAPM_DATA 3          /* spandsp's internal "connected" state number */
#define FIFO 16384

static struct {
    v42_state_t v42;
    v42bis_state_t bis;
    sp_get_fn get; sp_put_fn put; void *user;
    int tx_comp, rx_comp, dirty;
    uint8_t fifo[FIFO]; int fifo_len;
    int n_conn, n_disc;
} w;

static void w_status(void *user, int status)
{
    if (status == SIG_STATUS_LINK_CONNECTED) w.n_conn++;
    if (status == SIG_STATUS_LINK_DISCONNECTED) w.n_disc++;
}

/* compressed octets from spandsp's encoder, waiting for its LAPM */
static void w_enc_out(void *user, const uint8_t *msg, int len)
{
    if (len <= 0 || msg == NULL) return;
    if (len > FIFO - w.fifo_len) len = FIFO - w.fifo_len;
    memcpy(w.fifo + w.fifo_len, msg, (size_t)len);
    w.fifo_len += len;
}

static void w_dec_out(void *user, const uint8_t *msg, int len)
{
    if (len > 0 && msg != NULL) w.put(w.user, msg, len);
}

static int w_get(void *user, uint8_t *msg, int max)
{
    int n, tries = 0;
    if (!w.tx_comp) return w.get(w.user, msg, max);
    while (w.fifo_len == 0 && tries++ < 64) {
        uint8_t tmp[64];
        n = w.get(w.user, tmp, (int)sizeof tmp);
        if (n > 0) {
            v42bis_compress(&w.bis, tmp, n);
            w.dirty = 1;
        } else {
            if (w.dirty) v42bis_compress_flush(&w.bis);
            w.dirty = 0;
            break;
        }
    }
    n = w.fifo_len < max ? w.fifo_len : max;
    memcpy(msg, w.fifo, (size_t)n);
    memmove(w.fifo, w.fifo + n, (size_t)(w.fifo_len - n));
    w.fifo_len -= n;
    return n;
}

static void w_put(void *user, const uint8_t *msg, int len)
{
    if (msg == NULL || len <= 0) return;        /* status indications */
    if (!w.rx_comp) { w.put(w.user, msg, len); return; }
    v42bis_decompress(&w.bis, msg, len);
    v42bis_decompress_flush(&w.bis);
}

void sp_v42_start(int calling, int detect, int p0, int p1, int p2,
                  sp_get_fn get, sp_put_fn put, void *user)
{
    memset(&w, 0, sizeof w);
    w.get = get; w.put = put; w.user = user;
    v42_init(&w.v42, calling, detect, w_get, w_put, NULL);
    v42_set_status_callback(&w.v42, w_status, NULL);
    if (p0 < 0) {
        w.v42.config.comp = 0;                  /* no compression group in its XID */
    } else if (p0) {
        /* P0 bit 1: initiator to responder; bit 2: responder to initiator */
        w.v42.config.comp = (uint8_t)p0;
        w.v42.config.comp_dict_size = p1;
        w.v42.config.comp_max_string = p2;
        w.tx_comp = calling ? (p0 & 1) : (p0 & 2);
        w.rx_comp = calling ? (p0 & 2) : (p0 & 1);
        v42bis_init(&w.bis, 3, p1, p2, w_enc_out, NULL, 256, w_dec_out, NULL, 256);
    }
    v42_restart(&w.v42);
}

void sp_v42_rx_bit(int bit) { v42_rx_bit(&w.v42, bit); }
int  sp_v42_tx_bit(void) { return v42_tx_bit(&w.v42) & 1; }
int  sp_v42_connected(void) { return w.v42.lapm.state == SP_LAPM_DATA; }
int  sp_v42_seen_connected(void) { return w.n_conn; }
int  sp_v42_seen_disconnected(void) { return w.n_disc; }
void sp_v42_stop(void) { v42_stop(&w.v42); }

/* ------------------------------------------------- stand-alone V.42 bis */
static v42bis_state_t sbis;

int sp_v42bis_open(int p1, int p2, int mode, sp_put_fn enc_out, sp_put_fn dec_out, void *user)
{
    if (v42bis_init(&sbis, 3, p1, p2, enc_out, user, 512, dec_out, user, 512) == NULL) return -1;
    v42bis_compression_control(&sbis, mode == 1 ? V42BIS_COMPRESSION_MODE_ALWAYS
                                    : mode == 2 ? V42BIS_COMPRESSION_MODE_NEVER
                                                : V42BIS_COMPRESSION_MODE_DYNAMIC);
    return 0;
}

int sp_v42bis_compress(const uint8_t *buf, int len) { return v42bis_compress(&sbis, buf, len); }
int sp_v42bis_compress_flush(void) { return v42bis_compress_flush(&sbis); }
int sp_v42bis_decompress(const uint8_t *buf, int len) { return v42bis_decompress(&sbis, buf, len); }
int sp_v42bis_decompress_flush(void) { return v42bis_decompress_flush(&sbis); }
