#include <string.h>
#include <spandsp.h>
#include "peer_spandsp.h"

static v8_state_t *sp;
static sp_v8_result res;

static void handler(void *u, v8_parms_t *r)
{
    (void)u;
    if (r->status != V8_STATUS_V8_CALL && r->status != V8_STATUS_FAILED) return;
    res.final = 1; res.ok = (r->status == V8_STATUS_V8_CALL);
    res.call_function = r->call_function; res.modulations = (int)r->modulations;
    res.protocol = r->protocol; res.pstn_access = r->pstn_access;
    res.pcm = r->pcm_modem_availability;
}

void sp_v8call_start(unsigned mods, int pcm, int send_ci)
{
    v8_parms_t p;
    memset(&p, 0, sizeof p);
    p.modem_connect_tone = MODEM_CONNECT_TONES_NONE;
    p.send_ci = send_ci; p.v92 = -1;
    p.call_function = V8_CALL_V_SERIES;
    p.modulations = mods;
    p.protocol = V8_PROTOCOL_LAPM_V42;
    p.pcm_modem_availability = pcm;
    p.pstn_access = 0; p.nsf = -1; p.t66 = -1;
    memset(&res, 0, sizeof res);
    sp = v8_init(NULL, 1, &p, handler, NULL);
    span_log_set_level(v8_get_logging_state(sp), 0);
}

void sp_v8call_tick(int16_t rx, int16_t *tx)
{
    v8_rx(sp, &rx, 1);
    if (v8_tx(sp, tx, 1) < 1) *tx = 0;
}

const sp_v8_result *sp_v8call_result(void) { return &res; }
void sp_v8call_stop(void) { v8_free(sp); sp = NULL; }

/* ---- V.22bis ---- */
static v22bis_state_t *v22;
static void (*v22_put)(void *, int);
static void *v22_user;
static int v22_ok, v22_n_ok, v22_n_prog, v22_n_fail;

static void v22_putbit(void *u, int bit)
{
    (void)u;
    if (bit < 0) {                                /* SIG_STATUS_* */
        if (bit == SIG_STATUS_TRAINING_SUCCEEDED) { v22_ok = 1; v22_n_ok++; }
        if (bit == SIG_STATUS_TRAINING_IN_PROGRESS) v22_n_prog++;
        if (bit == SIG_STATUS_TRAINING_FAILED) v22_n_fail++;
        if (bit == SIG_STATUS_TRAINING_FAILED || bit == SIG_STATUS_CARRIER_DOWN) v22_ok = 0;
        return;
    }
    v22_put(v22_user, bit);
}

void sp_v22_start(int bit_rate, int calling, int (*get_bit)(void *), void (*put_bit)(void *, int), void *user)
{
    v22_put = put_bit; v22_user = user; v22_ok = 0; v22_n_ok = v22_n_prog = v22_n_fail = 0;
    v22 = v22bis_init(NULL, bit_rate, V22BIS_GUARD_TONE_NONE, calling, get_bit, user, v22_putbit, NULL);
    v22bis_tx_power(v22, -14.0f);
    span_log_set_level(v22bis_get_logging_state(v22), 0);
}

void sp_v22_tick(int16_t rx, int16_t *tx)
{
    v22bis_rx(v22, &rx, 1);
    if (v22bis_tx(v22, tx, 1) < 1) *tx = 0;
}

int  sp_v22_rate(void) { return v22bis_get_current_bit_rate(v22); }
int  sp_v22_trained(void) { return v22_ok; }
int  sp_v22_retrain(int bit_rate) { return v22bis_request_retrain(v22, bit_rate); }
int  sp_v22_count(int what) { return what == 0 ? v22_n_ok : what == 1 ? v22_n_prog : v22_n_fail; }
void sp_v22_stop(void) { v22bis_free(v22); v22 = NULL; }
