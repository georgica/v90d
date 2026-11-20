/* Tests for the update checker (srv/update.c): the version compare, and the
 * daily check's outcome driven by a stand-in command in place of the GitHub
 * query. No network, no flash. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "test.h"
#include "update.h"

static void nap_ms(long ms) { struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L }; nanosleep(&ts, NULL); }

/* update.c logs through the daemon's logger; the test does not need it */
void logf_(int level, const char *fmt, ...) { (void)level; (void)fmt; }

/* run the check with a stand-in command, let the state machine settle, then
 * report whether an update is on offer (and which). */
static int run_check(const char *cmd, const char *running, char *ver, size_t cap)
{
    int i;
    update_set_cmd(cmd);
    update_config(1, running);
    for (i = 0; i < 80; i++) { update_poll((uint32_t)(i * 10)); nap_ms(5); }
    return update_available(ver, cap);
}

int main(void)
{
    char ver[32];

    CHECK(update_ver_cmp("0.5.2", "0.5.2") == 0);
    CHECK(update_ver_cmp("0.5.1", "0.5.2") < 0);
    CHECK(update_ver_cmp("0.6.0", "0.5.9") > 0);
    CHECK(update_ver_cmp("0.5", "0.5.1") < 0);
    CHECK(update_ver_cmp("v0.5.2", "0.5.2") == 0);
    CHECK(update_ver_cmp("1.0", "0.9.9") > 0);
    CHECK(update_ver_cmp("0.10.0", "0.9.0") > 0);
    CHECK(update_ver_cmp("0.5.2", "0.5.10") < 0);

    update_set_state("build/test_update.state");
    unlink("build/test_update.state");

    /* a newer release is offered, by version */
    CHECK(run_check("printf 'version=0.9.0\\n'", "0.5.2", ver, sizeof ver) && strcmp(ver, "0.9.0") == 0);

    /* turning the check off drops the offer and the result file */
    update_config(0, "0.5.2");
    CHECK(!update_available(ver, sizeof ver));

    /* the same version as ours: nothing to offer */
    CHECK(!run_check("printf 'version=0.5.2\\n'", "0.5.2", ver, sizeof ver));
    /* older than ours: nothing to offer */
    CHECK(!run_check("printf 'version=0.1.0\\n'", "0.5.2", ver, sizeof ver));
    /* a reply that is not a version: nothing offered, no crash */
    CHECK(!run_check("printf 'not json\\n'", "0.5.2", ver, sizeof ver));
    /* the command fails outright: nothing offered */
    CHECK(!run_check("exit 1", "0.5.2", ver, sizeof ver));

    update_stop();
    unlink("build/test_update.state");
    T_REPORT("update");
}
