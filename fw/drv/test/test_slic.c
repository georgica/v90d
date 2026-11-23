#include <stdio.h>
#include <string.h>
#include "test.h"
#include "../slic.h"
#include "chipsim.h"

/* what the model of the analogue side does */
static struct {
    int  vbat_mode;             /* 0 healthy, 1 short circuit, 2 never quite there */
    int  cal_never;             /* a calibration that does not finish: its CALR1 value, or -1 */
    long conv_ms;               /* time the converter has been running */
    long conv_ms_total;
    int  running;
    int  linefeed_at_start;     /* LINEFEED when the converter was started */
    int  cal_left;
    int  started_without_user, hi_ram_writes;
    int  jmpen_seen;
} M;

#define MV(x) ((uint32_t)((x) * 1074L))
static chipsim C; static ps_bus B;

static void on_ram(chipsim *c, unsigned a, uint32_t v)
{
    if (a >= 1024) M.hi_ram_writes++;
    if (a == 1538) {
        int run = v == 0x600000 || v == 0x400000;
        if (run && !M.running) { M.linefeed_at_start = c->reg[30]; M.conv_ms = 0; }
        M.running = run;
        if (!run) c->ram[3] = MV(1000);
    }
}
static void on_write(chipsim *c, uint8_t r, uint8_t v)
{
    if (r == 29 && (v & 0x80)) M.cal_left = (M.cal_never >= 0 && c->reg[27] == M.cal_never) ? 1 << 30 : 40;
    if (r == 81 && v) M.jmpen_seen = 1;
}
static void on_sleep(chipsim *c, unsigned ms)
{
    if (M.cal_left) { M.cal_left -= (int)ms; if (M.cal_left <= 0) { M.cal_left = 0; c->reg[29] &= 0x7F; } }
    if (M.running) {
        long target = M.vbat_mode == 1 ? 2000 : M.vbat_mode == 2 ? 50000 : 59000, v;
        M.conv_ms += ms; M.conv_ms_total += ms;
        v = target * M.conv_ms / 40; if (v > target) v = target;
        c->ram[3] = MV(v);
    }
}
static void fresh(void)
{
    chipsim_init(&C); chipsim_bus(&C, &B);
    memset(&M, 0, sizeof M); M.cal_never = -1; M.linefeed_at_start = -1;
    C.busy_polls = 1;
    C.on_ram = on_ram; C.on_write = on_write; C.on_sleep = on_sleep;
    C.reg[11] = 0x05; C.reg[47] = 0x10; C.reg[80] = 0x3F; C.reg[68] = 0xF0;
    C.ram[1538] = 0x900000; C.ram[1575] = 0x1EC3FDBC; C.ram[767] = 0x03958100;
}

static const slic_cfg CFG = { SLIC_PCM_LINEAR, 0, 0 };

static void test_healthy(void)
{
    slic_info i; int r;
    fresh();
    r = slic_init(&B, &CFG, &i);
    CHECK_EQ_I(r, SLIC_OK);
    CHECK_EQ_I(i.id, 0xB2);
    CHECK(i.vbat_mv > 55000 && i.vbat_mv < 62000);
    CHECK_EQ_I(C.bad, 0);
    /* no patch: the jump table stays off */
    CHECK_EQ_I(M.jmpen_seen, 0); CHECK_EQ_I(C.reg[81], 0);
    /* the converter was started with the line open and runs on its analogue loop */
    CHECK_EQ_I(M.linefeed_at_start, 0);
    CHECK_EQ_I((long)C.ram[1538], 0x400000);
    CHECK_EQ_I((long)C.ram[1555], 0x100000);        /* charge pump on (no gate driver on this board) */
    CHECK_EQ_I((long)C.ram[1553], 0);               /* switch drive not inverted */
    CHECK_EQ_I((long)C.ram[767], 0x03D70A20);       /* 60 V */
    CHECK_EQ_I((long)C.ram[1548], 0x01C00000);      /* over-current threshold of this converter */
    CHECK(M.hi_ram_writes > 20);                    /* user mode was on: they arrived */
    CHECK_EQ_I((long)(C.ram[1575] & 0x08000000), 0);
    /* line in service: forward active, PCM on, our format and slot, hook by polling */
    CHECK_EQ_I(C.reg[30], 1);
    CHECK_EQ_I(C.reg[11], 0x13);
    CHECK_EQ_I(C.reg[12], 0); CHECK_EQ_I(C.reg[13], 0); CHECK_EQ_I(C.reg[14], 0); CHECK_EQ_I(C.reg[15], 0);
    CHECK_EQ_I(C.reg[22], 0); CHECK_EQ_I(C.reg[23], 0); CHECK_EQ_I(C.reg[24], 0); CHECK_EQ_I(C.reg[25], 0);
    CHECK_EQ_I(C.reg[47], 0x00);                    /* no power saving: it would stop the PCM path */
    CHECK_EQ_I(C.reg[80], 0x3E);
    CHECK_EQ_I(C.reg[46], 0x04);
    CHECK_EQ_I(C.reg[98], 0x80);
    CHECK_EQ_I(C.reg[45], 0x75);
    CHECK_EQ_I((long)C.ram[640], 0x05A38633);       /* 48 V open circuit */
    CHECK_EQ_I((long)C.ram[656], 0x0FF66D00);       /* impedance synthesis loaded */
    CHECK(C.ms < 1500);
    printf("  healthy part: in service after %lu ms, %lu bus octets, battery %ld mV\n", C.ms, C.frames, i.vbat_mv);
}

static void test_companded_second_port(void)
{
    slic_info i; slic_cfg c = { SLIC_PCM_ULAW, 16, 0 };
    fresh();
    CHECK_EQ_I(slic_init(&B, &c, &i), SLIC_OK);
    CHECK_EQ_I(C.reg[11], 0x11);
    CHECK_EQ_I(C.reg[12], 16); CHECK_EQ_I(C.reg[14], 16);
    c.pcm_format = SLIC_PCM_ALAW; fresh();
    CHECK_EQ_I(slic_init(&B, &c, &i), SLIC_OK);
    CHECK_EQ_I(C.reg[11], 0x10);
}

static void test_wrong_part(void)
{
    slic_info i;
    fresh(); C.reg[0] = 0xB1;                       /* rev B */
    CHECK_EQ_I(slic_init(&B, &CFG, &i), SLIC_E_ID);
    CHECK_EQ_I(i.id, 0xB1); CHECK_EQ_I(M.linefeed_at_start, -1); CHECK_EQ_I((long)C.ram[1538], 0x900000);
    fresh(); C.reg[0] = 0xFF;                       /* nothing there */
    CHECK_EQ_I(slic_init(&B, &CFG, &i), SLIC_E_ID);
    CHECK(C.frames < 40);
}

/* a Si3226x rev C (ID revision code 3, e.g. 0xB3): accepted as its own family
 * and brought up with the Si3226x general values, no Si3217x balance reload */
static void test_si3226x(void)
{
    slic_info i;
    fresh(); C.reg[0] = 0xB3;
    CHECK_EQ_I(slic_init(&B, &CFG, &i), SLIC_OK);
    CHECK_EQ_I(i.id, 0xB3);
    CHECK_EQ_I(i.family, SLIC_FAM_3226X);
    CHECK(i.vbat_mv > 55000 && i.vbat_mv < 62000);          /* talk battery up */
    CHECK_EQ_I((long)C.ram[1547], 0x00500000);              /* DCDC_OVTHRESH: the Si3226x value, not the Si3217x 0x01300000 */
    CHECK_EQ_I((long)C.ram[768], 0x051EB82A);               /* VBATR_EXPECT: 80 V, the Si3226x value */
}

/* With defer_service the line is left open at the end of slic_init (so both
 * channels of a dual part can be calibrated before either is in service), and
 * slic_service() puts it in service afterwards. */
static void test_deferred_service(void)
{
    slic_cfg c = { SLIC_PCM_LINEAR, 0, 1 };
    slic_info i;
    fresh();
    CHECK_EQ_I(slic_init(&B, &c, &i), SLIC_OK);
    CHECK(i.vbat_mv > 55000 && i.vbat_mv < 62000);      /* converter + calibration done */
    CHECK_EQ_I(C.reg[30], 0);                           /* LINEFEED: line still open */
    slic_service(&B);
    CHECK_EQ_I(C.reg[30], 1);                           /* now forward active */
}

/* The second channel of a dual part is addressed with the CID in the control
 * octet (channel 1 = 0x30 write / 0x70 read); the bus must reach it with no
 * protocol error. */
static void test_second_channel_cid(void)
{
    fresh();
    B.cid = 1;
    ps_wr(&B, PS_PCMRXLO, 0x5A);
    CHECK_EQ_I(C.ch, 1);
    CHECK_EQ_I(ps_rd(&B, PS_PCMRXLO), 0x5A);
    CHECK_EQ_I(C.ch, 1);
    CHECK_EQ_I((int)C.bad, 0);
    B.cid = 0;
    ps_wr(&B, PS_PCMRXLO, 0xA5);
    CHECK_EQ_I(C.ch, 0);
}

/* Bringing up channel 1 of a dual part while its shared converter is already
 * running (channel 0 started it): slic_init must leave the converter alone,
 * not re-arm it, and still bring the channel into service. */
/* the vendor's start-up step leaves a converter that is already running (its
 * PD_DCDC bit 20 clear) alone. After the driver's reset this does not happen;
 * kept as the vendor has it. (Each channel of a Si3226x has a converter of its
 * own: test_dual_order is what keeps them apart.) */
static void test_running_converter_left_alone(void)
{
    slic_info i;
    fresh(); C.reg[0] = 0xB3; B.cid = 1;
    C.ram[1538] = 0x400000;             /* PD_DCDC: converter already running (bit 20 clear) */
    C.ram[3] = MV(59000);               /* battery already up */
    M.running = 1; M.conv_ms = 1000;    /* so the model holds the battery there through the cal sleeps */
    CHECK_EQ_I(slic_init(&B, &CFG, &i), SLIC_OK);
    CHECK_EQ_I(i.family, SLIC_FAM_3226X);
    CHECK(i.vbat_mv > 55000 && i.vbat_mv < 62000);
    CHECK_EQ_I(M.linefeed_at_start, -1);        /* never (re)started the converter */
    CHECK_EQ_I((long)C.ram[1538], 0x400000);    /* left running, not re-armed */
    CHECK_EQ_I((int)C.bad, 0);
    B.cid = 0;
}

/* an empty footprint: the bus floats and reads whatever it was left at */
static void floats(chipsim *c, uint8_t reg, uint8_t v) { (void)v; c->reg[reg] = 0xB2; }
static void test_present(void)
{
    fresh(); CHECK_EQ_I(slic_present(&B), 1);
    CHECK_EQ_I(C.reg[14], 0);                       /* the register it tried is left as it was */
    CHECK_EQ_I((long)C.ram[1538], 0x900000); CHECK_EQ_I(M.linefeed_at_start, -1); CHECK_EQ_I(C.bad, 0);
    fresh(); C.reg[0] = 0xB1; CHECK_EQ_I(slic_present(&B), 1);    /* rev B is a part too */
    fresh(); C.reg[0] = 0xFF; CHECK_EQ_I(slic_present(&B), 0);
    fresh(); C.reg[0] = 0x00; CHECK_EQ_I(slic_present(&B), 0);
    fresh(); C.reg[0] = 0x32; CHECK_EQ_I(slic_present(&B), 0);    /* not a ProSLIC */
    /* something that reads like an identity but keeps nothing written to it */
    fresh(); C.reg[14] = 0xB2; C.on_write = floats; CHECK_EQ_I(slic_present(&B), 0);
}

static void test_dead_ram(void)
{
    slic_info i;
    fresh(); C.dead = 1;
    CHECK_EQ_I(slic_init(&B, &CFG, &i), SLIC_E_BUS);
    CHECK_EQ_I(M.linefeed_at_start, -1);
}

static void test_short_circuit(void)
{
    slic_info i;
    fresh(); M.vbat_mode = 1;
    CHECK_EQ_I(slic_init(&B, &CFG, &i), SLIC_E_SHORT);
    CHECK_EQ_I((long)C.ram[1538], 0x300000);        /* converter off */
    CHECK_EQ_I(C.reg[30], 0);                       /* line open */
    CHECK(M.conv_ms_total <= 60);                   /* it ran 50 ms, no more */
    CHECK(i.vbat_mv < 30000);
    printf("  short circuit: converter stopped after %ld ms at %ld mV\n", M.conv_ms_total, i.vbat_mv);
}

static void test_battery_never_up(void)
{
    slic_info i;
    fresh(); M.vbat_mode = 2;
    CHECK_EQ_I(slic_init(&B, &CFG, &i), SLIC_E_VBAT);
    CHECK_EQ_I((long)C.ram[1538], 0x900000);
    CHECK_EQ_I(C.reg[30], 0);
    CHECK(M.conv_ms_total >= 2000 && M.conv_ms_total <= 2300);
}

static void test_calibration_stuck(void)
{
    slic_info i;
    /* the first one (monitor ADC), before the converter */
    fresh(); M.cal_never = 0x00;
    CHECK_EQ_I(slic_init(&B, &CFG, &i), SLIC_E_CAL);
    CHECK_EQ_I(M.linefeed_at_start, -1);            /* converter never started */
    /* the second one, with the converter running: it is stopped again */
    fresh(); M.cal_never = 0xC0;
    CHECK_EQ_I(slic_init(&B, &CFG, &i), SLIC_E_CAL);
    CHECK_EQ_I((long)C.ram[1538], 0x900000); CHECK_EQ_I(C.reg[30], 0);
    CHECK(!M.running);
}

static void test_hook_and_shutdown(void)
{
    slic_info i;
    fresh();
    CHECK_EQ_I(slic_init(&B, &CFG, &i), SLIC_OK);
    C.reg[34] = 0x00; CHECK_EQ_I(slic_offhook(&B), 0);
    C.reg[34] = 0x02; CHECK_EQ_I(slic_offhook(&B), 1);
    C.reg[34] = 0x05; CHECK_EQ_I(slic_offhook(&B), 0);
    CHECK(slic_vbat_mv(&B) > 55000);
    C.ram[3] = 0x1FFFFFFF & (uint32_t)(-(long)MV(3));        /* negative readings are sign extended */
    CHECK(slic_vbat_mv(&B) <= -2 && slic_vbat_mv(&B) >= -4);
    slic_shutdown(&B);
    CHECK_EQ_I(C.reg[30], 0); CHECK_EQ_I((long)C.ram[1538], 0x900000); CHECK(!M.running);
}

/* the gain knobs of the bench: what slic_init() loads, and dB to RAM word */
static void test_gain_helpers(void)
{
    uint32_t v = 0; double want;
    int a;
    CHECK(slic_default_ram(SLIC_TXACGAIN, &v) == 1); CHECK(v == 0x0A5DB3A8ul);
    CHECK(slic_default_ram(SLIC_RXACGAIN, &v) == 1); CHECK(v == 0x018AC298ul);
    CHECK(slic_default_ram(SLIC_RXACGAIN_SAVE, &v) == 1); CHECK(v == 0x018AC298ul);
    CHECK(slic_default_ram(SLIC_ECFIR_C2, &v) == 1); CHECK(v == 0x001E4E80ul);
    CHECK(slic_default_ram(SLIC_ECIIR_B0 + 3, &v) == 1); CHECK(v == 0x0558D480ul);
    CHECK(slic_default_ram(SLIC_LINEAR_OS, &v) == 0);
    for (a = 540; a <= 549; a++) CHECK(slic_default_ram((unsigned)a, &v) == 1);
    for (a = 563; a <= 574; a++) CHECK(slic_default_ram((unsigned)a, &v) == 1);
    /* what the part was loaded with is in its RAM after slic_init() */
    fresh();
    { slic_info info; CHECK_EQ_I(slic_init(&B, &CFG, &info), SLIC_OK); }
    CHECK(C.ram[544] == 0x0A5DB3A8ul); CHECK(C.ram[545] == 0x018AC298ul); CHECK(C.ram[906] == 0x018AC298ul);

    CHECK(slic_gain_scale(0x018AC298ul, 0) == 0x018AC298ul);
    want = 0x018AC298 * pow(10.0, -6.0 / 20); CHECK_NEAR(slic_gain_scale(0x018AC298ul, -60), want, want * 1e-5);
    want = 0x018AC298 * pow(10.0, -0.1 / 20); CHECK_NEAR(slic_gain_scale(0x018AC298ul, -1), want, 2);
    want = 0x0A5DB3A8 * pow(10.0, 3.0 / 20);  CHECK_NEAR(slic_gain_scale(0x0A5DB3A8ul, 30), want, want * 1e-5);
    /* a negative coefficient (29-bit two's complement) keeps its sign */
    want = (0x20000000 - 0x1FFDB9F8) * pow(10.0, 2.5 / 20);
    CHECK_NEAR(0x20000000ul - slic_gain_scale(0x1FFDB9F8ul, 25), want, want * 1e-4 + 2);
    /* the largest positive word is the ceiling */
    CHECK(slic_gain_scale(0x0A5DB3A8ul, 60) == 0x0FFFFFFFul);
    CHECK(slic_gain_scale(0, 30) == 0);
}

/* ---- a dual part (Si3226x): two channels, each its own simulated register
 * set, one clock, and one log of the steps whose order matters ---- */
enum { EV_MADC, EV_CONV, EV_CAL4, EV_CAL5, EV_SERVICE };
static struct { int ch, ev; } L[64];
static int nL;
typedef struct {
    chipsim c; ps_bus b; int ch;
    int vbat_mode, running, cal_left, stall, wrong_cid; long conv_ms;
} chan_t;
static chan_t D[2];

static void d_log(int ch, int ev) { if (nL < 64) { L[nL].ch = ch; L[nL].ev = ev; nL++; } }
static void d_on_ram(chipsim *c, unsigned a, uint32_t v)
{
    chan_t *k = c->user_ctx;
    if (a == 1538) {
        int run = v == 0x600000 || v == 0x400000;
        if (v == 0x600000) { d_log(k->ch, EV_CONV); if (k->stall) k->stall = 2; }
        if (run && !k->running) k->conv_ms = 0;
        k->running = run;
        if (!run) c->ram[3] = MV(1000);
    }
}
static void d_on_write(chipsim *c, uint8_t r, uint8_t v)
{
    chan_t *k = c->user_ctx;
    if (c->ch != k->ch) k->wrong_cid++;
    if (r == 10 && k->stall == 2) { c->busy = 63; k->stall = 0; }     /* sent, but never seen done */
    if (r == 29 && (v & 0x80)) {
        d_log(k->ch, c->reg[27] == 0xC0 ? EV_CAL4 : c->reg[27] == 0x40 ? EV_CAL5 : EV_MADC);
        k->cal_left = 40;
    }
    if (r == 30 && v == 1) d_log(k->ch, EV_SERVICE);
}
/* time passes for both channels whichever one the driver sleeps on */
static void d_on_sleep(chipsim *c, unsigned ms)
{
    int n;
    (void)c;
    for (n = 0; n < 2; n++) {
        chan_t *k = &D[n];
        if (k->cal_left) { k->cal_left -= (int)ms; if (k->cal_left <= 0) { k->cal_left = 0; k->c.reg[29] &= 0x7F; } }
        if (k->running) {
            long target = k->vbat_mode == 1 ? 2000 : 59000, v;
            k->conv_ms += ms;
            v = target * k->conv_ms / 40; if (v > target) v = target;
            k->c.ram[3] = MV(v);
        }
    }
}
static void d_fresh(void)
{
    int n;
    nL = 0;
    for (n = 0; n < 2; n++) {
        chan_t *k = &D[n];
        memset(k, 0, sizeof *k);
        chipsim_init(&k->c); chipsim_bus(&k->c, &k->b);
        k->b.cid = (uint8_t)n; k->ch = n;
        k->c.user_ctx = k; k->c.on_ram = d_on_ram; k->c.on_write = d_on_write; k->c.on_sleep = d_on_sleep;
        k->c.busy_polls = 1;
        k->c.reg[0] = 0xB3;
        k->c.reg[11] = 0x05; k->c.reg[47] = 0x10; k->c.reg[80] = 0x3F; k->c.reg[68] = 0xF0;
        k->c.ram[1538] = 0x900000; k->c.ram[1575] = 0x1EC3FDBC; k->c.ram[767] = 0x03958100;
    }
}
static int d_first(int ev, int ch)
{
    int i;
    for (i = 0; i < nL; i++) if (L[i].ev == ev && (ch < 0 || L[i].ch == ch)) return i;
    return -1;
}
static int d_last(int ev, int ch)
{
    int i;
    for (i = nL - 1; i >= 0; i--) if (L[i].ev == ev && (ch < 0 || L[i].ch == ch)) return i;
    return -1;
}

static const slic_cfg C0 = { SLIC_PCM_ULAW, 0, 0 }, C1 = { SLIC_PCM_ULAW, 16, 0 };

/* the order that brought both ports of the issue #4 unit up: both setups,
 * then both converters back to back (channel 0 first), and nothing that
 * loads a line -- the line calibrations, the line in service -- before both
 * converters are running; both lines in service only after both are done */
static void test_dual_order(void)
{
    slic_info i0, i1; int r[2], conv0, conv1;

    d_fresh();
    slic_dual_init(&D[0].b, &D[1].b, &C0, &C1, &i0, &i1, r);
    CHECK_EQ_I(r[0], SLIC_OK); CHECK_EQ_I(r[1], SLIC_OK);
    CHECK_EQ_I(i0.family, SLIC_FAM_3226X); CHECK_EQ_I(i1.family, SLIC_FAM_3226X);
    conv0 = d_first(EV_CONV, 0); conv1 = d_first(EV_CONV, 1);
    CHECK(conv0 >= 0 && conv1 > conv0);
    CHECK(d_last(EV_MADC, 0) >= 0 && d_last(EV_MADC, 1) >= 0);
    CHECK(d_last(EV_MADC, -1) < conv0);
    CHECK(d_first(EV_CAL4, -1) > conv1);
    CHECK(d_first(EV_CAL5, -1) > conv1);
    CHECK(d_first(EV_SERVICE, -1) > conv1);
    CHECK(d_first(EV_SERVICE, -1) > d_last(EV_CAL5, -1));
    CHECK(d_first(EV_SERVICE, 0) >= 0 && d_first(EV_SERVICE, 1) >= 0);
    /* each channel spoken to on its own CID, with its own PCM slot */
    CHECK_EQ_I(D[0].wrong_cid, 0); CHECK_EQ_I(D[1].wrong_cid, 0);
    CHECK_EQ_I(D[0].c.reg[PS_PCMTXLO], 0); CHECK_EQ_I(D[1].c.reg[PS_PCMTXLO], 16);
    CHECK_EQ_I(D[0].c.reg[PS_LINEFEED], 1); CHECK_EQ_I(D[1].c.reg[PS_LINEFEED], 1);
    CHECK_EQ_I((int)D[0].c.bad, 0); CHECK_EQ_I((int)D[1].c.bad, 0);
}

/* one channel failing leaves it off and the other in service; only a part
 * that does not answer at all stops both */
static void test_dual_failures(void)
{
    slic_info i0, i1; int r[2];

    d_fresh(); D[1].vbat_mode = 1;                  /* port 2's line shorted */
    slic_dual_init(&D[0].b, &D[1].b, &C0, &C1, &i0, &i1, r);
    CHECK_EQ_I(r[0], SLIC_OK); CHECK_EQ_I(r[1], SLIC_E_SHORT);
    CHECK_EQ_I(D[0].c.reg[PS_LINEFEED], 1);
    CHECK_EQ_I(D[1].c.reg[PS_LINEFEED], 0); CHECK_EQ_I((long)D[1].c.ram[1538], 0x300000);

    d_fresh(); D[0].vbat_mode = 1;                  /* port 1's line shorted */
    slic_dual_init(&D[0].b, &D[1].b, &C0, &C1, &i0, &i1, r);
    CHECK_EQ_I(r[0], SLIC_E_SHORT); CHECK_EQ_I(r[1], SLIC_OK);
    CHECK_EQ_I(D[0].c.reg[PS_LINEFEED], 0); CHECK_EQ_I((long)D[0].c.ram[1538], 0x300000);
    CHECK_EQ_I(D[1].c.reg[PS_LINEFEED], 1);

    d_fresh(); D[0].c.reg[0] = 0xFF;                /* nothing answers: channel 1 is not tried */
    slic_dual_init(&D[0].b, &D[1].b, &C0, &C1, &i0, &i1, r);
    CHECK_EQ_I(r[0], SLIC_E_ID); CHECK_EQ_I(r[1], SLIC_E_ID);
    CHECK_EQ_I((long)D[1].c.frames, 0);
}

/* the start of the converter was sent but never seen done: the converter is
 * turned off before the bus error is reported, not left starting */
static void test_converter_bus_error(void)
{
    slic_info i; int r;
    d_fresh(); D[0].stall = 1;
    r = slic_init(&D[0].b, &C0, &i);
    CHECK_EQ_I(r, SLIC_E_BUS);
    CHECK_EQ_I(i.stage, 3);
    CHECK_EQ_I((long)D[0].c.ram[1538], 0x900000);   /* DCDC_OFF */
    CHECK_EQ_I(D[0].c.reg[PS_LINEFEED], 0);
}

int main(void)
{
    test_gain_helpers();
    test_healthy();
    test_companded_second_port();
    test_wrong_part();
    test_si3226x();
    test_deferred_service();
    test_second_channel_cid();
    test_running_converter_left_alone();
    test_dual_order();
    test_dual_failures();
    test_converter_bus_error();
    test_present();
    test_dead_ram();
    test_short_circuit();
    test_battery_never_up();
    test_calibration_stuck();
    test_hook_and_shutdown();
    T_REPORT("slic");
}
