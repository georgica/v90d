/* v90arch - the FTP server side as a state machine fed with command lines.
 * It writes the reply text and returns what the caller (the socket loop)
 * has to do; things that need the proxy are answered later through the
 * ftp_reply_* functions. No sockets in here. */
#ifndef V90ARCH_FTP_H
#define V90ARCH_FTP_H
#include <stddef.h>
#include <stdint.h>

#define FTP_PATH_MAX  256
#define FTP_HOST_MAX  128
#define FTP_REPLY_MAX 400   /* room a reply buffer must have */

enum {                      /* what ftp_line asks for */
    FTP_NONE = 0,
    FTP_CLOSE,              /* send the reply, then close the session */
    FTP_DEFER,              /* busy: keep the line and give it again later */
    FTP_ABORT,              /* drop the transfer in progress (reply is written) */
    FTP_PASV,               /* open a listener, then ftp_reply_pasv */
    FTP_PORT,               /* port_ip/port_port are set; drop any listener */
    FTP_LIST, FTP_NLST,     /* fetch the directory f->path */
    FTP_RETR,               /* fetch the file f->path */
    FTP_CWD,                /* check the directory f->path, then ftp_reply_cwd */
    FTP_SIZE                /* ask for the size of f->path, then ftp_reply_size */
};
enum { FTP_DATA_NONE = 0, FTP_DATA_PASV, FTP_DATA_PORT };
enum {                      /* how a transfer ended, for ftp_reply_done */
    FTP_R_OK = 0, FTP_R_NOTFOUND, FTP_R_NOPROXY, FTP_R_NODATA, FTP_R_ABORTED
};

typedef struct {
    char     host[FTP_HOST_MAX];
    char     cwd[FTP_PATH_MAX];   /* "/" or "/a/b", no trailing slash */
    char     path[FTP_PATH_MAX];  /* object of the pending action */
    int      data;                /* FTP_DATA_* */
    int      busy;                /* the action being waited for, 0 idle */
    uint32_t peer_ip;             /* PORT must name this address; 0 = any */
    uint32_t port_ip;
    unsigned port_port;
} ftp_t;

void   ftp_init(ftp_t *f, const char *host, uint32_t peer_ip);
size_t ftp_banner(const ftp_t *f, char *reply, size_t cap);
/* One command line (no CR LF). The reply, possibly empty, is NUL terminated. */
int    ftp_line(ftp_t *f, const char *line, char *reply, size_t cap);
size_t ftp_reply_pasv(ftp_t *f, uint32_t ip, unsigned port, char *reply, size_t cap); /* port 0: failed */
size_t ftp_reply_cwd(ftp_t *f, int ok, char *reply, size_t cap);
size_t ftp_reply_size(ftp_t *f, long size, char *reply, size_t cap);                  /* size < 0: unknown */
size_t ftp_reply_open(const ftp_t *f, char *reply, size_t cap);                       /* the 150 */
size_t ftp_reply_done(ftp_t *f, int result, char *reply, size_t cap);
/* "ftp://host/path" with the path percent-encoded; dir adds the final "/" */
long   ftp_url(const ftp_t *f, int dir, char *out, size_t cap);
/* cwd + argument -> clean absolute path. 0 ok, -1 too long. */
int    ftp_resolve(const char *cwd, const char *arg, char *out, size_t cap);
#endif
