/* Tests for the listing converter, on the real pages saved from the
 * Protoweb proxy (test/protoweb_ftp_listing.html is the root of
 * ftp.inode.com with its banner, test/protoweb_ftp_pub.html is /pub). */
#include <string.h>
#include "test.h"
#include "listing.h"

#define CHECK_EQ_S(got, want) do { if (strcmp((got), (want)) != 0) printf("    got:  %s\n    want: %s\n", (got), (want)); CHECK(strcmp((got), (want)) == 0); } while (0)

static char page[8192], out[8192];

static size_t load(const char *path)
{
    FILE *f = fopen(path, "rb");
    size_t n;
    if (!f) { printf("  cannot open %s (run from arch/)\n", path); return 0; }
    n = fread(page, 1, sizeof page - 1, f);
    fclose(f);
    page[n] = '\0';
    return n;
}

/* run a whole page through in pieces of the given size, with a small
 * output buffer so the back-pressure path is used too */
static void convert(size_t n, size_t piece, int names, int year, int mon)
{
    lst_t l;
    char small[LST_OUT_MAX];
    size_t pos = 0, o = 0, got;

    lst_init(&l, names, year, mon);
    while (pos < n) {
        size_t take = n - pos < piece ? n - pos : piece;
        size_t used = lst_feed(&l, page + pos, take, small, sizeof small, &got);
        memcpy(out + o, small, got); o += got;
        pos += used;
        CHECK(used > 0 || got > 0);
        if (used == 0 && got == 0) break;
    }
    got = lst_finish(&l, small, sizeof small);
    memcpy(out + o, small, got); o += got;
    out[o] = '\0';
}

static void test_size(void)
{
    CHECK_EQ_I(lst_size("-", 1), 0);
    CHECK_EQ_I(lst_size("825", 3), 825);
    CHECK_EQ_I(lst_size("1.2K", 4), 1229);
    CHECK_EQ_I(lst_size("1K", 2), 1024);
    CHECK_EQ_I(lst_size("3.4M", 4), 3565158);
    CHECK_EQ_I(lst_size("12.25k", 6), 12544);
    CHECK(lst_size("2.5G", 4) == 2684354560ULL);
    CHECK_EQ_I(lst_size("", 0), 0);
    CHECK_EQ_I(lst_size("abc", 3), 0);
    CHECK(lst_size("99999999999999999999999999", 26) > 0);   /* saturates, no overflow trap */
}

static void test_entry(void)
{
    lst_t l;
    char o[LST_OUT_MAX];
    const char *s;
    size_t n;

    lst_init(&l, 0, 2026, 11);
    s = "<A HREF=\"ftp://ftp.inode.com/pub/00_index.txt\"><IMG ALIGN=absbottom BORDER=0 SRC=\"http://www.inode.com/icons/internal-gopher-text.gif\" ALT=\"[[TXT]]\"> 00_index.txt</A>            2024-09-13 00:00    825   This file";
    n = lst_entry(&l, s, strlen(s), o, sizeof o); o[n] = '\0';
    CHECK_EQ_S(o, "-rw-r--r--   1 ftp      ftp             825 Sep 13  2024 00_index.txt\r\n");
    /* recent (within six months back): the time instead of the year */
    s = "<a href=\"ftp://h/pub/new%20file.zip\">x</a> 2026-07-04 09:05 3.4M";
    n = lst_entry(&l, s, strlen(s), o, sizeof o); o[n] = '\0';
    CHECK_EQ_S(o, "-rw-r--r--   1 ftp      ftp         3565158 Jul  4 09:05 new file.zip\r\n");
    s = "<A HREF=\"ftp://h/a/b/\"> b/</A> 2026-05-31 23:59 -";
    n = lst_entry(&l, s, strlen(s), o, sizeof o); o[n] = '\0';
    CHECK_EQ_S(o, "drwxr-xr-x   1 ftp      ftp               0 May 31  2026 b\r\n");
    /* a date in the future: year form */
    s = "<A HREF=\"ftp://h/f\">f</A> 2027-01-01 00:00 1K";
    n = lst_entry(&l, s, strlen(s), o, sizeof o); o[n] = '\0';
    CHECK_EQ_S(o, "-rw-r--r--   1 ftp      ftp            1024 Jan  1  2027 f\r\n");
    /* names only */
    lst_init(&l, 1, 2026, 11);
    s = "<A HREF=\"ftp://h/a/b/\"> b/</A> 2026-05-31 23:59 -";
    n = lst_entry(&l, s, strlen(s), o, sizeof o); o[n] = '\0';
    CHECK_EQ_S(o, "b\r\n");
    /* not entries */
    lst_init(&l, 0, 2026, 11);
    s = "<PRE><A HREF=\"ftp://ftp.inode.com/\">Up to higher level directory</A>";
    CHECK_EQ_I(lst_entry(&l, s, strlen(s), o, sizeof o), 0);
    s = "    Welcome to <A HREF=\"ftp://h/x\">x</A> 2026-05-31 23:59 -";
    CHECK_EQ_I(lst_entry(&l, s, strlen(s), o, sizeof o), 0);
    s = "<A HREF=\"ftp://h/x\">x</A> 2026-13-31 23:59 -";       /* no such month */
    CHECK_EQ_I(lst_entry(&l, s, strlen(s), o, sizeof o), 0);
    s = "<A HREF=\"ftp://h/x\">x</A> yesterday";
    CHECK_EQ_I(lst_entry(&l, s, strlen(s), o, sizeof o), 0);
    s = "<A HREF=\"ftp://h/\">x</A> 2026-05-31 23:59 -";        /* no name */
    CHECK_EQ_I(lst_entry(&l, s, strlen(s), o, sizeof o), 0);
    s = "<A HREF=\"ftp://h/a%2Fb\">x</A> 2026-05-31 23:59 -";   /* a slash in a name */
    CHECK_EQ_I(lst_entry(&l, s, strlen(s), o, sizeof o), 0);
    s = "<A HREF=\"ftp://h/a%0d%0aX\">x</A> 2026-05-31 23:59 -"; /* line ends in a name */
    CHECK_EQ_I(lst_entry(&l, s, strlen(s), o, sizeof o), 0);
    s = "<A HREF=\"ftp://h/x";
    CHECK_EQ_I(lst_entry(&l, s, strlen(s), o, sizeof o), 0);
    s = "<A HREF=\"ftp://h/x\">x</A> 2026-05-31 23:59 -";
    CHECK_EQ_I(lst_entry(&l, s, strlen(s), o, 10), 0);          /* no room: nothing, no overrun */
    CHECK_EQ_I(lst_entry(&l, "", 0, o, sizeof o), 0);
}

static const char *root_want =
    "drwxr-xr-x   1 ftp      ftp               0 Dec 16  2023 misc\r\n"
    "drwxr-xr-x   1 ftp      ftp               0 Jul 26  2025 pub\r\n"
    "-rw-r--r--   1 ftp      ftp            1229 Jul 18  2024 welcome.msg\r\n";

static void test_fixtures(void)
{
    size_t n = load("test/protoweb_ftp_listing.html"), piece;

    CHECK(n > 1000);
    /* the banner is not listed, the three entries are; the same result
     * whatever the size of the pieces it arrives in */
    for (piece = 1; piece <= 2100; piece = piece * 3 + 1) {
        convert(n, piece, 0, 2026, 11);
        CHECK_EQ_S(out, root_want);
    }
    convert(n, 100, 1, 2026, 11);
    CHECK_EQ_S(out, "misc\r\npub\r\nwelcome.msg\r\n");
    /* six months back: pub (2025-07-26) is recent when now is 2025-12 */
    convert(n, 4096, 0, 2025, 12);
    CHECK(strstr(out, " Jul 26 20:06 pub\r\n") != NULL);
    /* a clock that was never set: always the year form */
    convert(n, 4096, 0, 1970, 1);
    CHECK_EQ_S(out, root_want);

    n = load("test/protoweb_ftp_pub.html");
    CHECK(n > 3000);
    convert(n, 333, 0, 2026, 11);
    {
        const char *first = "drwxr-xr-x   1 ftp      ftp               0 Sep 11  2022 aol\r\n";
        CHECK(strncmp(out, first, strlen(first)) == 0);
    }
    CHECK(strstr(out, "\r\n-rw-r--r--   1 ftp      ftp             825 Sep 13  2024 00_index.txt\r\n") != NULL);
    CHECK(strstr(out, "Up to") == NULL);
    /* the whole page in one piece gives more output than the output buffer
     * holds at a time: nothing is lost, the result is the same */
    {
        static char first_way[sizeof out];
        strcpy(first_way, out);
        convert(n, 8192, 0, 2026, 11);
        CHECK_EQ_S(out, first_way);
    }
    {
        int lines = 0;
        const char *p;
        for (p = out; *p; p++) if (*p == '\n') lines++;
        CHECK_EQ_I(lines, 14);      /* 15 anchors, one of them the way up */
    }
}

static void test_stream_edges(void)
{
    lst_t l;
    char o[LST_OUT_MAX * 2];
    static char big[3000];
    size_t got, used, n;

    /* an entry in a last line without a newline comes out at the end */
    lst_init(&l, 1, 0, 0);
    strcpy(big, "<A HREF=\"ftp://h/x\">x</A> 2026-05-31 23:59 -");
    used = lst_feed(&l, big, strlen(big), o, sizeof o, &got);
    CHECK_EQ_I(used, strlen(big));
    CHECK_EQ_I(got, 0);
    got = lst_finish(&l, o, sizeof o); o[got] = '\0';
    CHECK_EQ_S(o, "x\r\n");
    CHECK_EQ_I(lst_finish(&l, o, sizeof o), 0);
    /* CR LF line ends */
    lst_init(&l, 1, 0, 0);
    strcpy(big, "<A HREF=\"ftp://h/x\">x</A> 2026-05-31 23:59 -\r\n<A HREF=\"ftp://h/y\">y</A> 2026-05-31 23:59 -\r\n");
    used = lst_feed(&l, big, strlen(big), o, sizeof o, &got); o[got] = '\0';
    CHECK_EQ_S(o, "x\r\ny\r\n");
    /* a line longer than the line buffer is skipped whole, and what follows
     * it is still seen */
    lst_init(&l, 1, 0, 0);
    n = (size_t)snprintf(big, sizeof big, "<A HREF=\"ftp://h/");
    while (n < 2500) big[n++] = 'z';
    n += (size_t)snprintf(big + n, sizeof big - n, "\">z</A> 2026-05-31 23:59 -\n<A HREF=\"ftp://h/ok\">ok</A> 2026-05-31 23:59 -\n");
    used = lst_feed(&l, big, n, o, sizeof o, &got); o[got] = '\0';
    CHECK_EQ_I(used, n);
    CHECK_EQ_S(o, "ok\r\n");
    /* the longest name that fits a line is passed, and fits LST_OUT_MAX */
    lst_init(&l, 0, 0, 0);
    n = (size_t)snprintf(big, sizeof big, "<A HREF=\"ftp://h/");
    while (n < 17 + 255) big[n++] = 'n';
    n += (size_t)snprintf(big + n, sizeof big - n, "\">n</A> 2026-05-31 23:59 9.9G\n");
    used = lst_feed(&l, big, n, o, LST_OUT_MAX, &got);
    CHECK_EQ_I(used, n);
    CHECK(got > 255 && got <= LST_OUT_MAX);
}

int main(void)
{
    test_size();
    test_entry();
    test_fixtures();
    test_stream_edges();
    T_REPORT("test_listing");
}
