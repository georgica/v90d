/* v90d-bbsinfo GUIDE BBSLIST: the board details of the BBS list, as the
 * list "v90d-bbsinfo 1" (guide.h), on standard output. GUIDE is the Telnet
 * BBS Guide's full_<Mon>_<YY>.txt, BBSLIST the list of 1-800-BBS-DIAL
 * (srv/bbs.h); a board of the list gets the details of the guide's entry
 * with the same host and port, one the guide lacks gets none. Netstalgia's
 * server runs this after it builds the BBS list, and signs the output.
 * Ours; written for this project. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "guide.h"
#include "bbs.h"

#define MAX_BOARDS 4096

static bbs_entry boards[MAX_BOARDS];
static int n_boards;
static char line[4096], out[16384];

int main(int argc, char **argv)
{
    FILE *f;
    gd_t g;
    static gd_entry e;
    int i, found = 0, used = 0;

    if (argc != 3) { fprintf(stderr, "usage: v90d-bbsinfo full_<Mon>_<YY>.txt bbs.txt\n"); return 2; }
    if (!(f = fopen(argv[2], "r"))) { perror(argv[2]); return 1; }
    while (fgets(line, sizeof line, f) && n_boards < MAX_BOARDS) {
        size_t n = strcspn(line, "\n");
        if (bbs_line(line, n, &boards[n_boards]) == BBS_LINE_BBS) {
            char *h = boards[n_boards].host;
            for (; *h; h++) if (*h >= 'A' && *h <= 'Z') *h = (char)(*h + 32);
            n_boards++;
        }
    }
    fclose(f);
    if (!(f = fopen(argv[1], "r"))) { perror(argv[1]); return 1; }
    printf("# Board details for the BBS directory, from The Telnet BBS Guide\n"
           "# (https://www.telnetbbsguide.com, The BBS Corner): a free list, distributed as it asks.\n"
           "%s\n", BI_HEAD);
    gd_init(&g);
    for (;;) {
        int got, eof = !fgets(line, sizeof line, f);
        got = eof ? gd_end(&g, &e) : gd_line(&g, line, strcspn(line, "\n"), &e);
        if (got) {
            found++;
            for (i = 0; i < n_boards; i++)
                if (boards[i].port == e.port && strcmp(boards[i].host, e.host) == 0) {
                    bi_write(&e, boards[i].login, out, sizeof out);
                    fputs(out, stdout);
                    used++;
                    break;
                }
        }
        if (eof) break;
    }
    fclose(f);
    fprintf(stderr, "v90d-bbsinfo: %d boards in the guide, %d of the list's %d with details\n", found, used, n_boards);
    return found ? 0 : 1;
}
