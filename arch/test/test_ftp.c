/* Tests for the FTP command state machine: fed with lines, checked by the
 * reply text and the action it asks the socket loop for. */
#include <string.h>
#include "test.h"
#include "ftp.h"

#define IP(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))
#define CHECK_EQ_S(got, want) do { if (strcmp((got), (want)) != 0) printf("    got:  %s\n    want: %s\n", (got), (want)); CHECK(strcmp((got), (want)) == 0); } while (0)
/* send a line, expect an action and a reply that starts with the text */
#define CMD(line, act, start) do { int _a = ftp_line(&f, (line), rep, sizeof rep); \
    CHECK_EQ_I(_a, (act)); \
    if (strncmp(rep, (start), strlen(start)) != 0) printf("    line: %s\n    got:  %s", (line), rep); \
    CHECK(strncmp(rep, (start), strlen(start)) == 0); \
    CHECK(rep[0] == '\0' || (strlen(rep) >= 2 && strcmp(rep + strlen(rep) - 2, "\r\n") == 0)); } while (0)

static ftp_t f;
static char rep[FTP_REPLY_MAX], url[1024];

static void test_resolve(void)
{
    char o[FTP_PATH_MAX];

    CHECK_EQ_I(ftp_resolve("/", "", o, sizeof o), 0);            CHECK_EQ_S(o, "/");
    CHECK_EQ_I(ftp_resolve("/", "pub", o, sizeof o), 0);         CHECK_EQ_S(o, "/pub");
    CHECK_EQ_I(ftp_resolve("/pub", "win16/", o, sizeof o), 0);   CHECK_EQ_S(o, "/pub/win16");
    CHECK_EQ_I(ftp_resolve("/pub", "/misc", o, sizeof o), 0);    CHECK_EQ_S(o, "/misc");
    CHECK_EQ_I(ftp_resolve("/pub/a", "..", o, sizeof o), 0);     CHECK_EQ_S(o, "/pub");
    CHECK_EQ_I(ftp_resolve("/pub/a", "../../..", o, sizeof o), 0); CHECK_EQ_S(o, "/");
    CHECK_EQ_I(ftp_resolve("/pub", "./a//b/./c/../d", o, sizeof o), 0); CHECK_EQ_S(o, "/pub/a/b/d");
    CHECK_EQ_I(ftp_resolve("/pub", "a b.txt", o, sizeof o), 0);  CHECK_EQ_S(o, "/pub/a b.txt");
    CHECK_EQ_I(ftp_resolve("/pub", "abcdefgh", o, 8), -1);
    {
        char longarg[600];
        memset(longarg, 'x', sizeof longarg - 1);
        longarg[sizeof longarg - 1] = '\0';
        CHECK_EQ_I(ftp_resolve("/", longarg, o, sizeof o), -1);
    }
}

static void test_session(void)
{
    ftp_init(&f, "ftp.inode.com", IP(10, 64, 64, 2));
    CHECK(ftp_banner(&f, rep, sizeof rep) > 0);
    CHECK(strncmp(rep, "220 ", 4) == 0);
    CHECK(strstr(rep, "ftp.inode.com") != NULL);
    CHECK(strstr(rep, "Protoweb") != NULL);
    CHECK(strcmp(rep + strlen(rep) - 2, "\r\n") == 0);

    CMD("USER anonymous", FTP_NONE, "331 ");
    CMD("PASS mozilla@", FTP_NONE, "230 ");
    CMD("user someone", FTP_NONE, "331 ");          /* any name, any case */
    CMD("pass x", FTP_NONE, "230 ");
    CMD("SYST", FTP_NONE, "215 UNIX Type: L8\r\n");
    CMD("PWD", FTP_NONE, "257 \"/\" ");
    CMD("XPWD", FTP_NONE, "257 \"/\" ");
    CMD("TYPE I", FTP_NONE, "200 ");
    CMD("TYPE A", FTP_NONE, "200 ");
    CMD("type a n", FTP_NONE, "200 ");
    CMD("TYPE L 8", FTP_NONE, "200 ");
    CMD("TYPE E", FTP_NONE, "504 ");
    CMD("MODE S", FTP_NONE, "200 ");
    CMD("MODE B", FTP_NONE, "504 ");
    CMD("STRU F", FTP_NONE, "200 ");
    CMD("STRU R", FTP_NONE, "504 ");
    CMD("NOOP", FTP_NONE, "200 ");
    CMD("FEAT", FTP_NONE, "211 ");
    CMD("REST 0", FTP_NONE, "350 ");
    CMD("REST 100", FTP_NONE, "502 ");
    CMD("ABOR", FTP_NONE, "226 ");
    /* read only */
    CMD("STOR x", FTP_NONE, "550 ");
    CMD("DELE x", FTP_NONE, "550 ");
    CMD("MKD x", FTP_NONE, "550 ");
    CMD("RNFR x", FTP_NONE, "550 ");
    CMD("APPE x", FTP_NONE, "550 ");
    CMD("RMD x", FTP_NONE, "550 ");
    CHECK(strstr(rep, "ead only") != NULL);
    /* the rest */
    CMD("SITE CHMOD 777 x", FTP_NONE, "502 ");
    CMD("EPSV", FTP_NONE, "502 ");
    CMD("MDTM x", FTP_NONE, "502 ");
    CMD("", FTP_NONE, "500 ");
    CMD("   ", FTP_NONE, "500 ");
    CMD("\xff\xf4\xff\xf2" "NOOP", FTP_NONE, "200 ");   /* telnet interrupt marks in front */
    CMD("NOOPX", FTP_NONE, "502 ");

    /* CWD: asked of the archive, the directory changes when it says yes */
    CMD("CWD pub", FTP_CWD, "");
    CHECK_EQ_S(f.path, "/pub");
    CHECK_EQ_I(ftp_url(&f, 1, url, sizeof url), 24);
    CHECK_EQ_S(url, "ftp://ftp.inode.com/pub/");
    CMD("PWD", FTP_DEFER, "");                      /* busy until answered */
    CHECK(ftp_reply_cwd(&f, 1, rep, sizeof rep) > 0);
    CHECK(strncmp(rep, "250 ", 4) == 0);
    CMD("PWD", FTP_NONE, "257 \"/pub\" ");
    CMD("CWD nowhere", FTP_CWD, "");
    ftp_reply_cwd(&f, 0, rep, sizeof rep);
    CHECK(strncmp(rep, "550 ", 4) == 0);
    CMD("PWD", FTP_NONE, "257 \"/pub\" ");
    CMD("XCWD /pub/win16/", FTP_CWD, "");
    ftp_reply_cwd(&f, 1, rep, sizeof rep);
    CMD("XPWD", FTP_NONE, "257 \"/pub/win16\" ");
    CMD("CDUP", FTP_NONE, "250 ");                  /* up needs no asking */
    CMD("PWD", FTP_NONE, "257 \"/pub\" ");
    CMD("XCUP", FTP_NONE, "250 ");
    CMD("CDUP", FTP_NONE, "250 ");
    CMD("PWD", FTP_NONE, "257 \"/\" ");
    CMD("CWD /", FTP_NONE, "250 ");                 /* nor does the root */
    CMD("CWD", FTP_NONE, "501 ");

    /* no data connection set up yet */
    CMD("LIST", FTP_NONE, "425 ");
    CMD("RETR welcome.msg", FTP_NONE, "425 ");

    /* PASV, LIST */
    CMD("PASV", FTP_PASV, "");
    CHECK(ftp_reply_pasv(&f, IP(198, 18, 0, 1), 50001, rep, sizeof rep) > 0);
    CHECK_EQ_S(rep, "227 Entering Passive Mode (198,18,0,1,195,81)\r\n");
    CHECK_EQ_I(f.data, FTP_DATA_PASV);
    CMD("LIST", FTP_LIST, "");
    CHECK_EQ_S(f.path, "/");
    ftp_url(&f, 1, url, sizeof url);
    CHECK_EQ_S(url, "ftp://ftp.inode.com/");
    CHECK(ftp_reply_open(&f, rep, sizeof rep) > 0);
    CHECK(strncmp(rep, "150 ", 4) == 0);
    CMD("NOOP", FTP_DEFER, "");
    CHECK(ftp_reply_done(&f, FTP_R_OK, rep, sizeof rep) > 0);
    CHECK(strncmp(rep, "226 ", 4) == 0);
    CHECK_EQ_I(f.data, FTP_DATA_NONE);              /* one transfer per PASV */
    CMD("NOOP", FTP_NONE, "200 ");
    CMD("LIST", FTP_NONE, "425 ");
    /* a passive port could not be had */
    CMD("PASV", FTP_PASV, "");
    ftp_reply_pasv(&f, IP(198, 18, 0, 1), 0, rep, sizeof rep);
    CHECK(strncmp(rep, "425 ", 4) == 0);
    CHECK_EQ_I(f.data, FTP_DATA_NONE);

    /* PORT, NLST with options and a path */
    CMD("PORT 10,64,64,2,4,1", FTP_PORT, "200 ");
    CHECK(f.port_ip == IP(10, 64, 64, 2));
    CHECK_EQ_I(f.port_port, 1025);
    CHECK_EQ_I(f.data, FTP_DATA_PORT);
    CMD("NLST -la pub", FTP_NLST, "");
    CHECK_EQ_S(f.path, "/pub");
    ftp_reply_done(&f, FTP_R_NOTFOUND, rep, sizeof rep);
    CHECK(strncmp(rep, "550 ", 4) == 0);
    /* PORT to somebody else, or nonsense */
    CMD("PORT 10,64,64,3,4,1", FTP_NONE, "5");
    CHECK_EQ_I(f.data, FTP_DATA_NONE);
    CMD("PORT 10,64,64,2,4", FTP_NONE, "501 ");
    CMD("PORT 10,64,64,2,4,256", FTP_NONE, "501 ");
    CMD("PORT 10,64,64,2,0,0", FTP_NONE, "501 ");
    CMD("PORT a,b,c,d,e,f", FTP_NONE, "501 ");
    CMD("PORT", FTP_NONE, "501 ");

    /* RETR, with a name that needs escaping in the URL */
    CMD("PORT 10,64,64,2,4,2", FTP_PORT, "200 ");
    CMD("CWD pub", FTP_CWD, "");
    ftp_reply_cwd(&f, 1, rep, sizeof rep);
    CHECK_EQ_I(f.data, FTP_DATA_PORT);              /* CWD does not use up the PORT */
    CMD("RETR read me #1 100%.txt", FTP_RETR, "");
    CHECK_EQ_S(f.path, "/pub/read me #1 100%.txt");
    ftp_url(&f, 0, url, sizeof url);
    CHECK_EQ_S(url, "ftp://ftp.inode.com/pub/read%20me%20%231%20100%25.txt");
    ftp_reply_open(&f, rep, sizeof rep);
    CHECK(strncmp(rep, "150 ", 4) == 0);
    /* ABOR in the middle: the transfer is dropped, both replies */
    CMD("\xff\xf4\xff\xf2" "ABOR", FTP_ABORT, "426 ");
    CHECK(strstr(rep, "\r\n226 ") != NULL);
    CHECK_EQ_I(f.busy, 0);
    CMD("RETR x", FTP_NONE, "425 ");
    CMD("RETR", FTP_NONE, "501 ");
    /* the ways a transfer can end */
    CMD("PASV", FTP_PASV, "");
    ftp_reply_pasv(&f, IP(198, 18, 0, 1), 50000, rep, sizeof rep);
    CMD("RETR /welcome.msg", FTP_RETR, "");
    CHECK_EQ_S(f.path, "/welcome.msg");
    ftp_reply_done(&f, FTP_R_NOPROXY, rep, sizeof rep);
    CHECK(strncmp(rep, "4", 1) == 0);
    CHECK(strstr(rep, "archive") != NULL);
    CHECK_EQ_I(f.busy, 0);
    f.data = FTP_DATA_PASV; CMD("LIST /pub", FTP_LIST, "");
    ftp_reply_done(&f, FTP_R_NODATA, rep, sizeof rep);
    CHECK(strncmp(rep, "425 ", 4) == 0);
    f.data = FTP_DATA_PASV; CMD("LIST", FTP_LIST, "");
    ftp_reply_done(&f, FTP_R_ABORTED, rep, sizeof rep);
    CHECK(strncmp(rep, "426 ", 4) == 0);

    /* SIZE */
    CMD("SIZE /welcome.msg", FTP_SIZE, "");
    ftp_url(&f, 0, url, sizeof url);
    CHECK_EQ_S(url, "ftp://ftp.inode.com/welcome.msg");
    ftp_reply_size(&f, 1131, rep, sizeof rep);
    CHECK_EQ_S(rep, "213 1131\r\n");
    CMD("SIZE nothing", FTP_SIZE, "");
    ftp_reply_size(&f, -1, rep, sizeof rep);
    CHECK(strncmp(rep, "550 ", 4) == 0);
    CMD("SIZE", FTP_NONE, "501 ");

    /* QUIT while busy closes too */
    f.data = FTP_DATA_PASV; CMD("LIST", FTP_LIST, "");
    CMD("QUIT", FTP_CLOSE, "221 ");
    ftp_init(&f, "ftp.inode.com", 0);
    CMD("quit", FTP_CLOSE, "221 ");
}

static void test_bounds(void)
{
    char longline[700], small[8];
    int i, a;

    ftp_init(&f, "ftp.inode.com", 0);
    /* a path that does not fit is refused, the state stays sane */
    memcpy(longline, "CWD ", 4);
    memset(longline + 4, 'd', sizeof longline - 5);
    longline[sizeof longline - 1] = '\0';
    CMD(longline, FTP_NONE, "5");
    CMD("PWD", FTP_NONE, "257 \"/\" ");
    for (i = 0; i < 100; i++) {                     /* deep, until it no longer fits */
        a = ftp_line(&f, "CWD deeper", rep, sizeof rep);
        if (a != FTP_CWD) break;
        ftp_reply_cwd(&f, 1, rep, sizeof rep);
    }
    CHECK(i > 10 && i < 100);
    CHECK(strlen(f.cwd) < FTP_PATH_MAX);
    CHECK(ftp_url(&f, 1, url, sizeof url) > 0);
    CHECK_EQ_I(ftp_url(&f, 1, url, 30), -1);
    /* quotes in a directory name are doubled in the 257 */
    ftp_init(&f, "h", 0);
    CMD("CWD a\"b", FTP_CWD, "");
    ftp_reply_cwd(&f, 1, rep, sizeof rep);
    CMD("PWD", FTP_NONE, "257 \"/a\"\"b\" ");
    /* any PORT address when no peer is known (not used by the daemon) */
    CMD("PORT 1,2,3,4,5,6", FTP_PORT, "200 ");
    /* tiny reply buffers are not overrun */
    memset(small, 'Z', sizeof small);
    ftp_line(&f, "SYST", small, 4);
    CHECK(small[3] == '\0' && small[4] == 'Z');
    ftp_banner(&f, small, 4);
    CHECK(small[3] == '\0' && small[4] == 'Z');
    /* a host name with characters that do not belong in a reply line */
    ftp_init(&f, "evil\r\n500 x", 0);
    ftp_banner(&f, rep, sizeof rep);
    CHECK(strchr(rep, '\n') == rep + strlen(rep) - 1);
}

int main(void)
{
    test_resolve();
    test_session();
    test_bounds();
    T_REPORT("test_ftp");
}
