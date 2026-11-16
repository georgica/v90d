/* v90arch - FTP server command handling (see ftp.h). */
#include <stdio.h>
#include <string.h>
#include "ftp.h"

static int upper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }

static size_t say(char *reply, size_t cap, const char *text)
{
    int n;
    if (cap == 0) return 0;
    n = snprintf(reply, cap, "%s", text);
    return (n < 0) ? 0 : ((size_t)n >= cap ? cap - 1 : (size_t)n);
}

void ftp_init(ftp_t *f, const char *host, uint32_t peer_ip)
{
    size_t i;

    memset(f, 0, sizeof *f);
    /* the host goes into reply lines and URLs: keep it to plain characters */
    for (i = 0; host[i] && i < sizeof f->host - 1; i++) {
        int c = (unsigned char)host[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                 c == '.' || c == '-' || c == '_' || c == ':';
        f->host[i] = ok ? (char)c : '_';
    }
    f->cwd[0] = '/';
    f->peer_ip = peer_ip;
}

size_t ftp_banner(const ftp_t *f, char *reply, size_t cap)
{
    int n;
    if (cap == 0) return 0;
    n = snprintf(reply, cap, "220 Archive of %s via Protoweb (v90d archive mode, read only).\r\n", f->host);
    return (n < 0) ? 0 : ((size_t)n >= cap ? cap - 1 : (size_t)n);
}

int ftp_resolve(const char *cwd, const char *arg, char *out, size_t cap)
{
    size_t n = 0;
    const char *p = arg;

    if (cap < 2) return -1;
    if (arg[0] != '/') {
        n = strlen(cwd);
        if (n + 1 > cap) return -1;
        memmove(out, cwd, n);            /* out may be cwd itself */
        if (n == 1) n = 0;                   /* the root is the empty prefix */
    }
    while (*p) {
        size_t l = 0;
        while (*p == '/') p++;
        while (p[l] && p[l] != '/') l++;
        if (l == 0) break;
        if (l == 1 && p[0] == '.') {
            /* stay */
        } else if (l == 2 && p[0] == '.' && p[1] == '.') {
            while (n > 0 && out[n - 1] != '/') n--;
            if (n > 0) n--;
        } else {
            if (n + 1 + l + 1 > cap) return -1;
            out[n++] = '/';
            memcpy(out + n, p, l);
            n += l;
        }
        p += l;
    }
    if (n == 0) out[n++] = '/';
    out[n] = '\0';
    return 0;
}

long ftp_url(const ftp_t *f, int dir, char *out, size_t cap)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t n, i;
    int r = snprintf(out, cap, "ftp://%s", f->host);

    if (r < 0 || (size_t)r >= cap) return -1;
    n = (size_t)r;
    for (i = 0; f->path[i]; i++) {
        int c = (unsigned char)f->path[i];
        if (n + 5 > cap) return -1;
        if (c <= 32 || c >= 127 || c == '%' || c == '#' || c == '?' || c == '"' || c == '<' || c == '>') {
            out[n++] = '%';
            out[n++] = hex[c >> 4];
            out[n++] = hex[c & 15];
        } else {
            out[n++] = (char)c;
        }
    }
    if (dir && out[n - 1] != '/') out[n++] = '/';
    out[n] = '\0';
    return (long)n;
}

/* is the word one of the space separated words of the list */
static int one_of(const char *word, const char *list)
{
    size_t l = strlen(word);
    const char *p = list;

    while (*p) {
        const char *sp = strchr(p, ' ');
        size_t n = sp ? (size_t)(sp - p) : strlen(p);
        if (n == l && memcmp(p, word, l) == 0) return 1;
        p += n;
        if (*p == ' ') p++;
    }
    return 0;
}

static int parse_port(const char *arg, uint32_t *ip, unsigned *port)
{
    unsigned v[6];
    int i;

    for (i = 0; i < 6; i++) {
        unsigned x = 0, digits = 0;
        if (i > 0) {
            if (*arg != ',') return -1;
            arg++;
        }
        while (*arg == ' ') arg++;
        while (*arg >= '0' && *arg <= '9' && digits < 3) { x = x * 10 + (unsigned)(*arg++ - '0'); digits++; }
        if (digits == 0 || x > 255) return -1;
        v[i] = x;
    }
    while (*arg == ' ') arg++;
    if (*arg != '\0') return -1;
    *ip = ((uint32_t)v[0] << 24) | ((uint32_t)v[1] << 16) | ((uint32_t)v[2] << 8) | v[3];
    *port = (v[4] << 8) | v[5];
    return (*port == 0 || *ip == 0) ? -1 : 0;
}

int ftp_line(ftp_t *f, const char *line, char *reply, size_t cap)
{
    char cmd[8];
    const char *arg;
    size_t n = 0;

    if (cap > 0) reply[0] = '\0';
    /* telnet marks (an ABOR comes with them) and other junk in front */
    while (*line && ((unsigned char)*line >= 127 || (unsigned char)*line <= 32)) line++;
    while (line[n] && line[n] != ' ' && n < sizeof cmd - 1) { cmd[n] = (char)upper((unsigned char)line[n]); n++; }
    cmd[n] = '\0';
    arg = line + n;
    if (*arg && *arg != ' ') cmd[0] = '?';   /* a word too long to be a command */
    while (*arg == ' ') arg++;

    if (f->busy) {
        if (strcmp(cmd, "ABOR") == 0) {
            f->busy = 0;
            f->data = FTP_DATA_NONE;
            say(reply, cap, "426 Transfer aborted.\r\n226 Abort done.\r\n");
            return FTP_ABORT;
        }
        if (strcmp(cmd, "QUIT") == 0) {
            f->busy = 0;
            say(reply, cap, "221 Goodbye.\r\n");
            return FTP_CLOSE;
        }
        return FTP_DEFER;
    }

    if (cmd[0] == '\0') { say(reply, cap, "500 Empty command.\r\n"); return FTP_NONE; }
    if (strcmp(cmd, "USER") == 0) { say(reply, cap, "331 Any password will do.\r\n"); return FTP_NONE; }
    if (strcmp(cmd, "PASS") == 0) { say(reply, cap, "230 Logged in to the archive (read only).\r\n"); return FTP_NONE; }
    if (strcmp(cmd, "SYST") == 0) { say(reply, cap, "215 UNIX Type: L8\r\n"); return FTP_NONE; }
    if (strcmp(cmd, "NOOP") == 0) { say(reply, cap, "200 OK.\r\n"); return FTP_NONE; }
    if (strcmp(cmd, "FEAT") == 0) { say(reply, cap, "211 No features.\r\n"); return FTP_NONE; }
    if (strcmp(cmd, "ABOR") == 0) { say(reply, cap, "226 Nothing to abort.\r\n"); return FTP_NONE; }
    if (strcmp(cmd, "QUIT") == 0) { say(reply, cap, "221 Goodbye.\r\n"); return FTP_CLOSE; }
    if (strcmp(cmd, "TYPE") == 0) {
        int t = upper((unsigned char)arg[0]);
        if (t == 'A' || t == 'I' || t == 'L') say(reply, cap, "200 Type set.\r\n");
        else say(reply, cap, "504 Type not supported.\r\n");
        return FTP_NONE;
    }
    if (strcmp(cmd, "MODE") == 0) {
        say(reply, cap, upper((unsigned char)arg[0]) == 'S' ? "200 Stream mode.\r\n" : "504 Stream mode only.\r\n");
        return FTP_NONE;
    }
    if (strcmp(cmd, "STRU") == 0) {
        say(reply, cap, upper((unsigned char)arg[0]) == 'F' ? "200 File structure.\r\n" : "504 File structure only.\r\n");
        return FTP_NONE;
    }
    if (strcmp(cmd, "REST") == 0) {
        say(reply, cap, strcmp(arg, "0") == 0 ? "350 Starting at 0.\r\n" : "502 No restart in the archive.\r\n");
        return FTP_NONE;
    }
    if (strcmp(cmd, "PWD") == 0 || strcmp(cmd, "XPWD") == 0) {
        char q[2 * FTP_PATH_MAX];
        size_t i, k = 0;
        for (i = 0; f->cwd[i]; i++) {
            if (f->cwd[i] == '"') q[k++] = '"';
            q[k++] = f->cwd[i];
        }
        q[k] = '\0';
        if (cap > 0) snprintf(reply, cap, "257 \"%s\" is the current directory.\r\n", q);
        if (cap > 2 && strlen(reply) == cap - 1) memcpy(reply + cap - 3, "\r\n", 2);
        return FTP_NONE;
    }
    if (strcmp(cmd, "CDUP") == 0 || strcmp(cmd, "XCUP") == 0) {
        ftp_resolve(f->cwd, "..", f->cwd, sizeof f->cwd);
        say(reply, cap, "250 Directory changed.\r\n");
        return FTP_NONE;
    }
    if (strcmp(cmd, "CWD") == 0 || strcmp(cmd, "XCWD") == 0) {
        if (arg[0] == '\0') { say(reply, cap, "501 Which directory?\r\n"); return FTP_NONE; }
        if (ftp_resolve(f->cwd, arg, f->path, sizeof f->path) != 0) {
            say(reply, cap, "550 Path too long.\r\n");
            return FTP_NONE;
        }
        if (strcmp(f->path, "/") == 0) {
            strcpy(f->cwd, "/");
            say(reply, cap, "250 Directory changed.\r\n");
            return FTP_NONE;
        }
        f->busy = FTP_CWD;
        return FTP_CWD;
    }
    if (strcmp(cmd, "PASV") == 0) {
        f->data = FTP_DATA_NONE;
        return FTP_PASV;                     /* answered at once, so not busy */
    }
    if (strcmp(cmd, "PORT") == 0) {
        f->data = FTP_DATA_NONE;
        if (parse_port(arg, &f->port_ip, &f->port_port) != 0) {
            say(reply, cap, "501 Bad PORT.\r\n");
            return FTP_NONE;
        }
        if (f->peer_ip != 0 && f->port_ip != f->peer_ip) {
            say(reply, cap, "500 PORT to another address refused.\r\n");
            return FTP_NONE;
        }
        f->data = FTP_DATA_PORT;
        say(reply, cap, "200 PORT command successful.\r\n");
        return FTP_PORT;
    }
    if (strcmp(cmd, "LIST") == 0 || strcmp(cmd, "NLST") == 0) {
        /* options like -la are for a real ls: skipped */
        while (arg[0] == '-') {
            while (*arg && *arg != ' ') arg++;
            while (*arg == ' ') arg++;
        }
        if (f->data == FTP_DATA_NONE) { say(reply, cap, "425 Use PASV or PORT first.\r\n"); return FTP_NONE; }
        if (ftp_resolve(f->cwd, arg, f->path, sizeof f->path) != 0) {
            say(reply, cap, "550 Path too long.\r\n");
            return FTP_NONE;
        }
        f->busy = (cmd[0] == 'L') ? FTP_LIST : FTP_NLST;
        return f->busy;
    }
    if (strcmp(cmd, "RETR") == 0 || strcmp(cmd, "SIZE") == 0) {
        int retr = (cmd[0] == 'R');
        if (arg[0] == '\0') { say(reply, cap, "501 Which file?\r\n"); return FTP_NONE; }
        if (retr && f->data == FTP_DATA_NONE) { say(reply, cap, "425 Use PASV or PORT first.\r\n"); return FTP_NONE; }
        if (ftp_resolve(f->cwd, arg, f->path, sizeof f->path) != 0 || strcmp(f->path, "/") == 0) {
            say(reply, cap, "550 No such file.\r\n");
            return FTP_NONE;
        }
        f->busy = retr ? FTP_RETR : FTP_SIZE;
        return f->busy;
    }
    if (one_of(cmd, "STOR STOU APPE DELE RMD XRMD MKD XMKD RNFR RNTO")) {
        say(reply, cap, "550 Read only archive.\r\n");
        return FTP_NONE;
    }
    say(reply, cap, "502 Command not implemented.\r\n");
    return FTP_NONE;
}

size_t ftp_reply_pasv(ftp_t *f, uint32_t ip, unsigned port, char *reply, size_t cap)
{
    char text[64];

    if (port == 0) {
        f->data = FTP_DATA_NONE;
        return say(reply, cap, "425 Cannot open a passive port.\r\n");
    }
    f->data = FTP_DATA_PASV;
    snprintf(text, sizeof text, "227 Entering Passive Mode (%u,%u,%u,%u,%u,%u)\r\n",
             (unsigned)(ip >> 24), (unsigned)((ip >> 16) & 255), (unsigned)((ip >> 8) & 255),
             (unsigned)(ip & 255), (port >> 8) & 255, port & 255);
    return say(reply, cap, text);
}

size_t ftp_reply_cwd(ftp_t *f, int ok, char *reply, size_t cap)
{
    f->busy = 0;
    if (!ok) return say(reply, cap, "550 No such directory in the archive.\r\n");
    memcpy(f->cwd, f->path, sizeof f->cwd);
    return say(reply, cap, "250 Directory changed.\r\n");
}

size_t ftp_reply_size(ftp_t *f, long size, char *reply, size_t cap)
{
    char text[40];

    f->busy = 0;
    if (size < 0) return say(reply, cap, "550 Size not known.\r\n");
    snprintf(text, sizeof text, "213 %ld\r\n", size);
    return say(reply, cap, text);
}

size_t ftp_reply_open(const ftp_t *f, char *reply, size_t cap)
{
    return say(reply, cap, f->busy == FTP_RETR ? "150 Opening data connection for the file.\r\n"
                                               : "150 Opening data connection for the listing.\r\n");
}

size_t ftp_reply_done(ftp_t *f, int result, char *reply, size_t cap)
{
    f->busy = 0;
    f->data = FTP_DATA_NONE;                 /* a PASV or PORT serves one transfer */
    switch (result) {
    case FTP_R_OK:       return say(reply, cap, "226 Transfer complete.\r\n");
    case FTP_R_NOTFOUND: return say(reply, cap, "550 Not found in the archive.\r\n");
    case FTP_R_NOPROXY:  return say(reply, cap, "451 Archive mode could not reach the archive.\r\n");
    case FTP_R_NODATA:   return say(reply, cap, "425 Cannot open the data connection.\r\n");
    default:             return say(reply, cap, "426 Transfer aborted.\r\n");
    }
}
