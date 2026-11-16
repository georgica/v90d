/* V.32bis start-up of the answering modem (6.2, Figure 3) against the calling
 * test peer: order and length of every segment, the 64 T turn-round, the
 * round-trip measurement, the rate signals. */
#include "test.h"
#include "calltest.h"

static call_t k;
#define T (10.0 / 3)                 /* samples per symbol */

static const char *sname[] = { "AC1", "CA", "AC2", "SILENCE", "S1", "SBAR1", "TRN1", "R1", "WAIT", "RXTRAIN",
                               "S2", "SBAR2", "TRN2", "R3", "E", "B1", "DATA", "CLEAR", "FAILED" };

static void startup(int delay, unsigned a_rates, unsigned c_rates, int want_rate)
{
    v32line_cfg lc;
    v32_state last = V32S_AC1;
    long t0 = 0;
    int ok, rt;
    double turn, peer_turn;

    line_defaults(&lc);
    lc.delay = delay;
    lc.near_db = 15; lc.far_db = 20; lc.c_near_db = 15; lc.c_far_db = 20;
    call_init(&k, &lc, a_rates, c_rates, 3000, 3000);
    rt = k.line.oneway + k.line.oneway_back;
    printf("  one-way delay %d samples (%.1f ms), rates offered %02x / %02x\n", k.line.oneway, k.line.oneway / 8.0, a_rates, c_rates);
    ok = 0;
    while (k.n < 8000L * 20) {
        call_tick(&k);
        if (k.m.state != last) {
            if (getenv("V32_DEBUG"))
                printf("    %6ld  %-8s %5ld samples = %7.1f symbols\n", k.n, sname[last], k.n - t0, (k.n - t0) / T);
            last = k.m.state; t0 = k.n;
        }
        if (k.m.connected && k.c.connected) { ok = 1; break; }
        if (k.m.state == V32S_FAILED || k.c.state == VCS_FAILED) break;
    }
    CHECK(ok);
    if (!ok) { printf("  FAILED: answerer state %s error %d, caller state %d\n", sname[k.m.state], k.m.error, k.c.state); return; }

    /* --- 6.2: AC for an even number of symbols >= 128, CA for an even number --- */
    CHECK(k.m.seg_len[V32S_AC1] >= 128); CHECK_EQ_I(k.m.seg_len[V32S_AC1] & 1, 0);
    CHECK_EQ_I(k.m.seg_len[V32S_CA] & 1, 0);
    /* --- the turn-round: our CA-AC transition leaves 64 +- 2 T after the
     * caller's AA-CC transition arrived (pulse centres at our line terminals) --- */
    turn = ((double)k.m.t_ac2 - ((double)k.c.t_cc + k.line.peer_lag + k.line.oneway_back)) / T;
    /* --- and the caller's, as a check of the peer: its AA-CC after our AC-CA --- */
    peer_turn = ((double)k.c.t_cc + k.line.peer_lag - ((double)k.m.t_ca + k.line.oneway)) / T;
    printf("  turn-round at the line: answerer %.2f T, caller %.2f T (64 +- 2)\n", turn, peer_turn);
    CHECK_NEAR(turn, 64.0, 2.0);
    CHECK_NEAR(peer_turn, 64.0, 2.0);
    /* --- round trip: MT and NT include the far end's 64 T --- */
    printf("  round trip %.1f T: MT %.1f T, NT %.1f T (each less the other's turn-round: %.1f, %.1f)\n",
           rt / T, k.m.mt / T, k.c.nt / T, k.m.mt / T - peer_turn, k.c.nt / T - turn);
    CHECK_NEAR(k.m.mt / T - peer_turn, rt / T, 1.5);
    CHECK_NEAR(k.c.nt / T - turn, rt / T, 1.5);
    CHECK_NEAR((double)k.m.ec.bulk, rt > 96 + 28 ? rt - 28.0 : 96.0, 10.0);
    /* --- 16 T of silence, then S 256 T, S-bar 16 T, TRN >= 1280 T (<= 8192) --- */
    CHECK_EQ_I(k.m.seg_len[V32S_SILENCE], 16);
    CHECK_EQ_I(k.m.seg_len[V32S_S1], 256);
    CHECK_EQ_I(k.m.seg_len[V32S_SBAR1], 16);
    CHECK(k.m.seg_len[V32S_TRN1] >= 1280 && k.m.seg_len[V32S_TRN1] <= 8192);
    /* --- R1: whole 16-bit sequences are not required of R1 (it is cut when S
     * arrives), but it must have run from TRN until then --- */
    CHECK(k.m.seg_len[V32S_R1] >= 24);
    /* --- silent for at least MT after the caller's S --- */
    CHECK(k.m.seg_len[V32S_WAIT] * T >= k.m.mt);
    /* --- second conditioning signal and the rest --- */
    CHECK_EQ_I(k.m.seg_len[V32S_S2], 256);
    CHECK_EQ_I(k.m.seg_len[V32S_SBAR2], 16);
    CHECK(k.m.seg_len[V32S_TRN2] >= 1280 && k.m.seg_len[V32S_TRN2] <= 8192);
    CHECK_EQ_I(k.m.seg_len[V32S_R3] % 8, 0);         /* whole rate sequences before E */
    CHECK_EQ_I(k.m.seg_len[V32S_E], 8);
    CHECK_EQ_I(k.m.seg_len[V32S_B1], 128);
    /* --- what the caller saw of it: S-bar to the rate signal is 16 T + TRN
     * + the rate sequences its detector needs (24 to 56 T), twice --- */
    printf("  caller saw: S-bar -> R1 %.1f T (TRN %d), S-bar -> R3 %.1f T (TRN %d)\n",
           (k.c.t_rate[0] - k.c.t_sbar[0]) / T, (int)k.m.seg_len[V32S_TRN1],
           (k.c.t_rate[1] - k.c.t_sbar[1]) / T, (int)k.m.seg_len[V32S_TRN2]);
    CHECK_NEAR((k.c.t_rate[0] - k.c.t_sbar[0]) / T, 16 + k.m.seg_len[V32S_TRN1] + 40, 16);
    CHECK_NEAR((k.c.t_rate[1] - k.c.t_sbar[1]) / T, 16 + k.m.seg_len[V32S_TRN2] + 40, 16);
    /* --- rate signals --- */
    CHECK_EQ_I(k.c.r1, v32_rate_word(a_rates));                 /* R1: all we have */
    CHECK_EQ_I(k.m.r2_rates, a_rates & c_rates);                /* R2: within R1 */
    CHECK_EQ_I(k.c.r3, v32_rate_word(1u << want_rate));         /* R3: the one rate */
    CHECK_EQ_I(k.m.rx.e_word, v32_rate_word(1u << want_rate) | V32_RW_E);
    CHECK_EQ_I(k.c.e_word, v32_rate_word(1u << want_rate) | V32_RW_E);
    CHECK_EQ_I(k.m.bitrate, V32_BITRATE(want_rate));
    CHECK_EQ_I(k.c.bitrate, V32_BITRATE(want_rate));
    printf("  echo attenuation after TRN: answerer %.1f dB, caller %.1f dB; connected at %d bit/s after %.2f s\n",
           k.m.erle_db10 / 10.0, k.c.erle_db, k.m.bitrate, k.n / 8000.0);
    CHECK(k.m.erle_db10 >= 350);
    /* --- and data flows both ways --- */
    call_run(&k, 8000 * 3);
    printf("  3 s of data: caller -> answerer %ld bits %ld errors, answerer -> caller %ld bits %ld errors\n",
           k.a_chk.bits, k.a_chk.errs, k.c_chk.bits, k.c_chk.errs);
    CHECK(k.a_chk.bits > 2 * V32_BITRATE(want_rate)); CHECK_EQ_I(k.a_chk.errs, 0);
    CHECK(k.c_chk.bits > 2 * V32_BITRATE(want_rate)); CHECK_EQ_I(k.c_chk.errs, 0);
}

int main(void)
{
    startup(40, V32_RALL, V32_RALL, 4);
#ifndef __arm__
    startup(400, V32_RALL, V32_RALL, 4);                        /* 100 ms round trip */
    startup(2400, V32_RALL, V32_R4800 | V32_R7200 | V32_R9600, 2);   /* a satellite hop; a V.32-rate caller */
    startup(8, V32_R4800 | V32_R7200, V32_RALL, 1);
#endif
    T_REPORT("test_startup");
}
