/* Known problems in the libraries under dce/, each shown by a check that
 * FAILS for as long as the problem is there. Not part of "make test": run
 * with "make test-findings". The DCE works around 1 and 2 (dce_cfg_t
 * .fixed_timers = 0); the tests here switch the workaround off. 3 to 6 are
 * in the modem and have no workaround. When a check here starts passing,
 * the finding is fixed and its test can move to test_call.c. */
#include <string.h>
#include "test.h"
#include "harness.h"

/* 1. v42: the default T401 (1 s) is shorter than an acknowledgement takes at
 * 2400 bit/s when full I frames (N401 = 128) flow both ways: own frame
 * 0.45 s + the frame the far end is busy with + the frame that carries the
 * N(R). The timer runs out again and again on a clean line; each time the
 * sender stops and polls. Costs about 6% throughput at 2400 bit/s and gets
 * worse as the line gets slower (at 1200 bit/s one frame alone is 0.9 s). */
static void finding_t401(void)
{
    dce_cfg_t sc; caller_cfg_t cc;
    dce_status_t *s;
    long r[2];

    printf("1. v42: default T401 against the frame time at 2400 bit/s\n");
    server_cfg(&sc); client_cfg(&cc);
    sc.fixed_timers = 1; cc.fixed_timers = 1;
    call_begin(&sc, &cc, &line_clean);
    CHECK(run_until(p_ip_up, 30000) > 0);
    CHECK(traffic(RANDOM, 20, 20, 120000, r));
    s = status();
    printf("    clean line, both ways: down %ld up %ld octets/s, server T401 expiries %lu, caller %lu\n",
           r[DN], r[UP], s->lapm_t401_expiries, C.v42.lapm.t401_expiries);
    CHECK_EQ_I(s->lapm_t401_expiries, 0);
    CHECK_EQ_I(C.v42.lapm.t401_expiries, 0);
}

/* 2. ppp: ppp_fsm gives every retransmitted Configure-Request a new
 * identifier (RFC 1661 allows keeping it). At 300 bit/s a request and its
 * Ack take longer than the 3 s Restart timer, so every Ack answers a
 * request that has already been replaced and is discarded: LCP never
 * opens and the call ends with PPP_END_NEGOTIATION after 30 s. Shown with
 * the server at its default timer and a patient client, as a real client
 * that keeps its identifiers would be. */
static void finding_restart(void)
{
    dce_cfg_t sc; caller_cfg_t cc;
    dce_status_t *s;
    int t;

    printf("2. ppp: default Restart timer at 300 bit/s\n");
    server_cfg(&sc); client_cfg(&cc);
    cc.modes = V8_MOD_V21;
    cc.use_v42 = 0;
    sc.fixed_timers = 1;                   /* the server keeps its 3 s */
    call_begin(&sc, &cc, &line_clean);
    t = run_until(p_ip_up, 90000);
    s = status();
    printf("    IP up after %d ms (-1 = never); state %d, end: %s, PPP end %d\n", t, (int)s->state,
           dce_end_name(s->end_reason), (int)s->ppp_end);
    CHECK(t > 0);
    CHECK_EQ_I(s->end_reason, DCE_END_NONE);
}

/* 3. modem: on a line with nobody calling but with hybrid echo (0.2 of our
 * own transmit signal coming back, an ordinary amount) the answer modem
 * ends up "connected" at V.21: after ANSam and the legacy V.22bis wait it
 * falls back to V.21 and takes the echo of its own 1650/1850 Hz channel for
 * the caller's 980/1180 Hz carrier. The call then dies in PPP half a minute
 * later instead of ending with "no carrier". */
static void finding_v21_echo(void)
{
    dce_cfg_t sc; caller_cfg_t cc;
    dce_status_t *s;
    int t;

    printf("3. modem: silent line with hybrid echo\n");
    server_cfg(&sc); client_cfg(&cc);
    call_begin(&sc, &cc, &line_clean);
    caller_hangup(&C);                     /* the caller never makes a sound */
    t = run_until(p_ended, 180000);
    s = status();
    printf("    ended after %.1f s: %s; modulation %d, bitrate %d, time online %u ms\n", t / 1000.0,
           dce_end_name(s->end_reason), s->modulation, s->bitrate, (unsigned)s->online_ms);
    CHECK_EQ_I(s->end_reason, DCE_END_NO_CARRIER);
    CHECK_EQ_I(s->modulation, ANS_MOD_NONE);
    CHECK_EQ_I(s->online_ms, 0);
}

/* 4. modem: the V.22bis receiver can be knocked out for good by strong
 * noise bursts. Most 12 ms bursts only cost a few LAPM frames, but sooner
 * or later one makes the equaliser / carrier loop diverge, and then the
 * receiver never finds its way back: the error stays near the full
 * constellation size although the line is clean again, the carrier
 * indicator stays on, nothing triggers a retrain, and LAPM gives up after
 * N400 retransmissions. Bursts are applied in rounds of ten until a
 * receiver is still off 3 s after a round; then the line is left clean. */
static void finding_burst(void)
{
    static const line_t bursty = { -55.0, 700, 12, 6000, 0.2 };
    dce_cfg_t sc; caller_cfg_t cc;
    dce_status_t *s;
    long r[2];
    int round, hit = 0, ok;

    printf("4. modem: V.22bis after noise bursts\n");
    server_cfg(&sc); client_cfg(&cc);
    call_begin(&sc, &cc, &bursty);
    CHECK(run_until(p_ip_up, 30000) > 0);
    printf("    before: receiver error (4096 = one constellation unit squared) server %d, caller %d\n",
           (int)D.modem.v22.rx.mse, (int)C.modem.v22.rx.mse);
    for (round = 1; round <= 60 && !hit; round++) {
        bursts_on = 1;
        traffic(RANDOM, 4, 4, 7000, r);
        bursts_on = 0;
        run_ms(3000);
        hit = D.modem.v22.rx.mse > 1000 || C.modem.v22.rx.mse > 1000;
    }
    if (!hit) {
        printf("    no receiver was knocked out by %ld bursts\n", n_bursts);
        return;
    }
    printf("    3 s after burst %ld: receiver error server %d, caller %d\n", n_bursts,
           (int)D.modem.v22.rx.mse, (int)C.modem.v22.rx.mse);
    run_ms(10000);
    printf("    10 s of clean line later: server %d (carrier %d), caller %d (carrier %d)\n",
           (int)D.modem.v22.rx.mse, D.modem.v22.rx.carrier, (int)C.modem.v22.rx.mse, C.modem.v22.rx.carrier);
    CHECK(D.modem.v22.rx.mse < 400);
    CHECK(C.modem.v22.rx.mse < 400);
    ok = traffic(TEXT, 5, 5, 60000, r);
    s = status();
    printf("    then: %d of 5 packets down, %d of 5 up; state %d, end: %s (V.42 reason %d)\n",
           F[DN].n_rx, F[UP].n_rx, (int)s->state, dce_end_name(s->end_reason), s->v42_reason);
    CHECK(ok);
    CHECK_EQ_I(s->end_reason, DCE_END_NONE);
}

/* 5. modem: with steady noise at -40 dBFS (transmit level 3000, about
 * 19 dB SNR over the 4 kHz band) the start-up never completes on the
 * answering side, while at -41 dBFS it completes and the data phase is
 * all but error free. The start-up is far more fragile than the data mode. */
static void finding_noise_startup(void)
{
    static const line_t n40 = { -40.0, 0, 0, 0, 0.2 };
    dce_cfg_t sc; caller_cfg_t cc;
    dce_status_t *s;
    int t;

    printf("5. modem: start-up in steady noise of -40 dBFS\n");
    server_cfg(&sc); client_cfg(&cc);
    call_begin(&sc, &cc, &n40);
    t = run_until(p_ip_up, 60000);
    s = status();
    printf("    IP up after %d ms (-1 = never); end: %s; answer phase %d (V.8 result %d, selected %d, V.22 state %d); caller connected %d\n",
           t, dce_end_name(s->end_reason), (int)D.modem.phase, (int)D.modem.v8.result, (int)D.modem.v8.selected,
           (int)D.modem.v22.state, C.modem.connected);
    CHECK(t > 0);
}

/* 6. modem: the V.90 upstream (V.34) receiver makes bit errors in data mode
 * on a clean line, in bursts some seconds apart, from about 10 s after the
 * connection on. Measured on the modem alone (answer_tick_pcm against
 * v8call + p2a + p34a through sim/channel, pseudo-random data, 60 s):
 *   noise -55 dBFS, echo 0.2:  26400 bit/s, 742 of 1396758 bits wrong
 *   noise -60 dBFS, echo 0.1:  28800 bit/s, 1579 of 1523736 bits wrong
 *   no noise, no echo (40 s):  28800 bit/s, 113 of 955368 bits wrong
 * (test/test_answer_v90.c scenario 5 runs the same set-up for 16 s and
 * about 40000 bits only, which is before the first burst.) The receiver
 * recovers by itself each time; the downstream is not affected. Either the
 * upstream rate is chosen without margin or one of the receiver's loops
 * wanders (modem/p34d.c RX_DATA: qamrx_update with equaliser, carrier and
 * timing all adapting on tentative decisions). With LAPM the cost is a few
 * retransmissions; without error control every burst costs PPP frames.
 * Shown here as LAPM frames from the caller that arrive damaged while it
 * sends for about a minute. */
static void finding_v90_upstream(void)
{
    dce_cfg_t sc; caller_cfg_t cc;
    dce_status_t *s;
    long r[2];

    printf("6. modem: V.90 upstream bit errors on a clean line\n");
    server_cfg(&sc); client_cfg(&cc);
    sc.modes |= ANS_MODE_V90; cc.v90 = 1;
    call_begin(&sc, &cc, &line_clean);
    CHECK(run_until(p_ip_up, 60000) > 0);
    CHECK(traffic(RANDOM, 0, 500, 200000, r));
    CHECK_EQ_I(F[UP].n_bad, 0);
    s = status();
    printf("    %d bit/s up, %ld octets in %lu I frames at %ld octets/s: %lu frames damaged on the line, %lu sent again\n",
           s->rx_bitrate, F[UP].bytes_rx, s->lapm_rx_i, r[UP], s->lapm_rx_bad, C.v42.lapm.tx_i_retx);
    CHECK_EQ_I(s->lapm_rx_bad, 0);
    CHECK_EQ_I(C.v42.lapm.tx_i_retx, 0);
}

/* "linear" as the argument runs the calls through the linear dce_tick()
 * instead of dce_tick_pcm() (not finding 6: V.90 needs the octets) */
int main(int argc, char **argv)
{
    harness_linear = argc > 1 && strcmp(argv[1], "linear") == 0;
    finding_t401();
    finding_restart();
    finding_v21_echo();
    finding_burst();
    finding_noise_startup();
    harness_linear = 0;
    finding_v90_upstream();
    T_REPORT("findings (failures are open findings)");
}
