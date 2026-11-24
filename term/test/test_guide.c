/* guide.h: the Telnet BBS Guide read a line at a time, dates, wrapping, the
 * board details list written and read. */
#include <stdio.h>
#include <string.h>
#include "test.h"
#include "guide.h"

static const char *GUIDE[] = {
    "              The BBS Corner TELNET BBS GUIDE - November 2026",
    "            --------------------------------------------------",
    "                          Telnet: sbbs.dmine.net:24",
    "===========================================================================",
    "* = NEW listing since last edition",
    "===========================================================================",
    "--------------------------------------------------------------------------",
    "",
    "   ALCATRAZ PRISON BBS",
    "   Last Updated: April 24, 2020 \r",
    "",
    "   Telnet: AlcatrazBBS.ddns.net:9000",
    "      SSH: ",
    "      WEB: alcatraz.example",
    " Location: Madrid, , Spain",
    "  Dial-Up: ",
    " Software: BBS Express Pro     Total Nodes: 5     Login: NEW",
    "",
    "To properly enjoy this BBS use",
    "Syncterm.",
    "",
    "Second paragraph.",
    "--------------------------------------------------------------------------",
    "",
    "   * NEWER BBS",
    "   Last Updated: May 11, 2025",
    "   Telnet: newer.example",
    " Software: Mystic              Total Nodes:       Login: ",
    "--------------------------------------------------------------------------",
    "Added in the past 30 days: 41",
    NULL
};

static char lines[16][128]; static int nl;
static void got(void *ctx, const char *s, size_t n) { (void)ctx; if (nl < 16) { memcpy(lines[nl], s, n); lines[nl][n] = 0; nl++; } }

int main(void)
{
    gd_t g; static gd_entry e[4]; int n = 0, i; char d[11], out[4096];
    const char *k, *v; size_t kn, vn;

    CHECK(gd_date("May 11, 2025", d) && !strcmp(d, "2025-05-11"));
    CHECK(gd_date("December 1, 1999", d) && !strcmp(d, "1999-12-01"));
    CHECK(!gd_date("Mai 11, 2025", d) && d[0] == 0);
    CHECK(!gd_date("May 32, 2025", d));

    gd_init(&g);
    for (i = 0; GUIDE[i]; i++) if (gd_line(&g, GUIDE[i], strlen(GUIDE[i]), &e[n])) n++;
    if (gd_end(&g, &e[n])) n++;
    CHECK_EQ_I(n, 2);                                   /* not the header, not the tail */
    CHECK(!strcmp(e[0].name, "ALCATRAZ PRISON BBS"));
    CHECK(!strcmp(e[0].host, "alcatrazbbs.ddns.net") && e[0].port == 9000);
    CHECK(!strcmp(e[0].loc, "Madrid, Spain"));
    CHECK(!strcmp(e[0].sw, "BBS Express Pro") && !strcmp(e[0].nodes, "5") && !strcmp(e[0].login, "NEW"));
    CHECK(!strcmp(e[0].web, "alcatraz.example") && !strcmp(e[0].upd, "2020-04-24"));
    CHECK(!strcmp(e[0].desc, "To properly enjoy this BBS use Syncterm.\n\nSecond paragraph."));
    CHECK(!strcmp(e[1].name, "NEWER BBS") && e[1].port == 23 && !e[1].nodes[0] && !e[1].login[0] && !e[1].desc[0]);

    /* wrapping: at spaces, paragraphs kept, a long word cut, a limit */
    nl = 0;
    CHECK_EQ_I(gd_wrap("one two three four\n\nfive", 9, 0, got, NULL), 5);
    CHECK(!strcmp(lines[0], "one two") && !strcmp(lines[1], "three") && !strcmp(lines[2], "four") && !lines[3][0] && !strcmp(lines[4], "five"));
    nl = 0;
    CHECK_EQ_I(gd_wrap("abcdefghijkl", 5, 0, got, NULL), 3);
    CHECK(!strcmp(lines[0], "abcde") && !strcmp(lines[2], "kl"));
    nl = 0;
    CHECK_EQ_I(gd_wrap("a b c d e f", 1, 2, got, NULL), 2);
    nl = 0;                                             /* UTF-8 counts as one column a character */
    CHECK_EQ_I(gd_wrap("\xc3\xa9\xc3\xa9\xc3\xa9 x", 5, 0, got, NULL), 1);
    nl = 0;
    CHECK_EQ_I(gd_wrap("\xc3\xa9\xc3\xa9\xc3\xa9 x", 4, 0, got, NULL), 2);

    /* the list: written, read back */
    bi_write(&e[0], "alcatraz", out, sizeof out);
    CHECK(!strcmp(out, "board alcatraz\nloc Madrid, Spain\nsw BBS Express Pro\nnodes 5\nnew NEW\nweb alcatraz.example\n"
                       "upd 2020-04-24\nd To properly enjoy this BBS use Syncterm.\nd\nd Second paragraph.\n"));
    CHECK(!strstr(out, "ddns") && !strstr(out, "9000"));          /* never the host or port */
    CHECK_EQ_I(bi_line("v90d-bbsinfo 1\r", 15, &k, &kn, &v, &vn), BI_HEAD_LINE);
    CHECK_EQ_I(bi_line("# credit", 8, &k, &kn, &v, &vn), BI_NONE);
    CHECK_EQ_I(bi_line("board alcatraz", 14, &k, &kn, &v, &vn), BI_BOARD);
    CHECK(vn == 8 && !memcmp(v, "alcatraz", 8));
    CHECK_EQ_I(bi_line("board Bad!", 10, &k, &kn, &v, &vn), BI_BAD);
    CHECK_EQ_I(bi_line("sw Mystic", 9, &k, &kn, &v, &vn), BI_FIELD);
    CHECK(kn == 2 && vn == 6);
    CHECK_EQ_I(bi_line("future some value", 17, &k, &kn, &v, &vn), BI_FIELD);   /* a key from later: read, passed over by tdir */
    CHECK_EQ_I(bi_line("d", 1, &k, &kn, &v, &vn), BI_FIELD);
    CHECK(vn == 0);
    CHECK_EQ_I(bi_line("Sw x", 4, &k, &kn, &v, &vn), BI_BAD);
    T_REPORT("guide");
}
