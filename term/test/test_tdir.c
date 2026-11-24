/* tdir.h: the directory filled from the BBS list and the details, searched
 * a slice at a time, a view per caller. */
#include <stdio.h>
#include <string.h>
#include "test.h"
#include "tdir.h"

static tdir_t D;

static void bbs(const char *s) { tdir_bbs_line(&D, s, strlen(s)); }
static void info(const char *s) { tdir_info_line(&D, s, strlen(s)); }

int main(void)
{
    static tdir_view v, w;
    int steps;
    tdir_init(&D);
    bbs("v90d-bbs 2");
    bbs("bbs alcatraz 2272522872 alcatrazbbs.ddns.net:9000 Alcatraz Prison BBS\r");
    bbs("bbs zruspa 2279787720 bbs.zruspas.org Zruspa's BBS");
    bbs("bbs 13thfloor 2271384356 13th.hoyvision.com:6400 13th Floor BBS");
    bbs("bbs alcatraz 2272522872 dup.example Again");          /* a login twice: the first stays */
    bbs("bbs bad 123 x.example Short number");
    bbs("bbs nohost 2270000000");
    CHECK_EQ_I(D.n, 3);
    info("v90d-bbsinfo 1");
    info("board zruspa");
    info("loc Madrid, Spain");
    info("future 1");
    info("d Spanish board.");
    info("sw Synchronet");                                     /* after a d line: the description moves */
    info("d Second line.");
    info("d");
    info("d A paragraph of its own.");
    info("board unknown");
    info("loc Nowhere");
    info("board alcatraz");
    info("sw BBS Express Pro");
    CHECK_EQ_I(D.with_info, 2);
    CHECK(!strcmp(tdir_get(&D, 1, TD_LOC), "Madrid, Spain"));
    CHECK(!strcmp(tdir_get(&D, 1, TD_SW), "Synchronet"));
    CHECK(!strcmp(tdir_get(&D, 1, TD_DESC) + 27, "\n\nA paragraph of its own."));
    CHECK(!strncmp(tdir_get(&D, 1, TD_DESC), "Spanish board. Second line.", 27));   /* one paragraph, as wrapped for the list */
    CHECK(!strcmp(tdir_get(&D, 0, TD_NAME), "Alcatraz Prison BBS") && !strcmp(tdir_get(&D, 0, TD_NUMBER), "2272522872"));
    CHECK(!strcmp(tdir_get(&D, 2, TD_LOC), "") && !strcmp(tdir_get(&D, 9, TD_NAME), ""));

    /* every board, then a search, a slice at a time; two views apart */
    tdir_search(&v, "");
    CHECK(tdir_step(&D, &v, 100) && v.nres == 3);
    CHECK(v.res[0] == 0 && v.res[1] == 1 && v.res[2] == 2);      /* the list's order until sorted */
    tdir_sort(&D);
    tdir_search(&v, "");
    tdir_step(&D, &v, 100);
    CHECK(v.res[0] == 2 && v.res[1] == 0 && v.res[2] == 1);      /* 13th, Alcatraz, Zruspa's */
    tdir_search(&w, "MADRID");
    for (steps = 0; !tdir_step(&D, &w, 1); steps++) ;
    CHECK_EQ_I(steps, 2);
    CHECK(w.nres == 1 && w.res[0] == 1 && v.nres == 3);
    tdir_search(&w, "express");                                /* software */
    tdir_step(&D, &w, 99); CHECK(w.nres == 1 && w.res[0] == 0);
    tdir_search(&w, "second line");                            /* descriptions */
    tdir_step(&D, &w, 99); CHECK(w.nres == 1 && w.res[0] == 1);
    tdir_search(&w, "ddns");                                   /* hosts are not searched: not kept */
    tdir_step(&D, &w, 99); CHECK_EQ_I(w.nres, 0);
    CHECK_EQ_I(tdir_letter(&D, &v, 'z'), 2);
    CHECK_EQ_I(tdir_letter(&D, &v, '#'), 0);
    CHECK_EQ_I(tdir_letter(&D, &v, 'q'), -1);
    CHECK(tdir_file_as("13th") == '#' && tdir_file_as("abc") == 'A');
    T_REPORT("tdir");
}
