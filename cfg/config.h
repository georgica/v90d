/* v90d/cfg - configuration file format and loader.
 *
 * FILE FORMAT
 *
 * Plain ASCII text, one item per line, at most CFG_LINE_MAX characters per
 * line and CFG_FILE_MAX bytes per file. Line ends are LF or CR LF.
 *
 *   # comment            a line whose first non-blank character is # or ;
 *   [section]            opens a section: network, ppp, modem, modem2, line,
 *                        line2, status, log
 *   key = value          sets a key of the open section
 *
 * Blanks around section names, keys and values are ignored. There are no
 * trailing comments: everything after the = belongs to the value. Every key
 * may appear once, except `user`, which may appear up to CFG_MAX_USERS
 * times. Unknown sections, unknown keys, repeated keys and malformed values
 * are errors; nothing is silently ignored. A key that is absent keeps its
 * default (shown below).
 *
 *   [network]
 *   lan = dhcp                dhcp | static. With dhcp, address and netmask
 *                             are kept but not used.
 *   address = 192.168.1.2     LAN address of the box (static mode)
 *   netmask = 255.255.255.0   LAN netmask (static mode), contiguous, /1../30
 *   gateway = none            default route (static mode): an address in the
 *                             LAN's network, or `none`
 *   ppp_local = 10.64.64.1    our end of the PPP link
 *   ppp_peer = 10.64.64.2     address handed to the dial-in peer on port 1
 *   ppp_peer2 = 10.64.64.3    the same for port 2; a different address
 *   dns1 = 9.9.9.9            DNS servers offered in IPCP; `none` to omit
 *   dns2 = 1.1.1.1
 *   nat = on                  on | off: masquerade the peer behind the LAN
 *
 *   [ppp]
 *   auth = none               none | pap | chap. pap and chap need a user.
 *   mru = 1500                68..1500
 *   echo_interval = 30        seconds between LCP echo requests, 0 = off,
 *                             at most 3600
 *   compression = on          on | off: PPP compression (CCP) with callers
 *                             that ask for it, Deflate or MPPC
 *   user = NAME SECRET        one line per user. NAME is 1..63 octets,
 *                             SECRET 0..64 octets.
 *
 *   NAME and SECRET are either bare words (printable ASCII without blanks,
 *   double quote or backslash) or double-quoted strings. Inside quotes the
 *   escapes are \" \\ and \xHH (two hex digits, not 00), so any octet
 *   except NUL can be written. The bare word <redacted> is refused as a
 *   secret: it is what the redacted dump prints.
 *
 *   [modem]
 *   modulations = v90, v34, v32bis, v22bis, v22, v21
 *                             comma separated, no repeats; v32bis
 *                             covers V.32 as well
 *   max_downstream = 56000    bit/s, 300..56000
 *   max_upstream = 33600      bit/s, 300..48000; more than 33600 is only
 *                             reached with V.92 (v92 = on)
 *   tx_level = -12            transmit level in dBm0, -30..-6
 *   v42 = on                  on | off
 *   v42bis = on               on | off; on needs v42 = on
 *   v92 = off                 on | off: offer V.92 PCM upstream to callers
 *                             whose modem asks for it. Has an effect only
 *                             with v90 in modulations.
 *   rings = 1                 rings before answer, 1..10
 *   law = ulaw                ulaw | alaw
 *
 *   [line]
 *   number = any              the number callers must dial: 1..20 characters
 *                             from 0-9 * #, or `any` (every number connects)
 *   tones = us                us | eu: dial tone, ringback and busy cadences
 *   busy_chance = 0           0..100, percent of calls that get a busy signal
 *                             instead of an answer
 *   drop_mean = 0             0..10080, mean minutes of connect time before
 *                             the line drops at random, 0 = never
 *   session_limit = 0         0..10080 minutes, hard limit of one call,
 *                             0 = none
 *   idle_timeout = 0          0..10080 minutes without IP traffic before we
 *                             hang up, 0 = none
 *
 *   [modem2] and [line2]
 *   The box has two telephone ports. [modem] and [line] are port 1;
 *   [modem2] and [line2] take the same keys with the same defaults for
 *   port 2. Everything else (network, ppp with its users, status, log) is
 *   shared by both ports.
 *
 *   [status]
 *   listen = lan              lan | off: HTTP status server on the LAN side
 *   port = 80                 1..65535
 *   admin_password = ""       0..64 octets, one bare word or quoted string
 *                             as for a user secret. Empty (also written as
 *                             nothing after the =): the settings page is
 *                             open to anyone on the LAN. Otherwise it asks
 *                             for user `admin` and this password.
 *
 *   [log]
 *   level = info              error | warn | info | debug
 *
 * Numbers are plain decimal without leading zeros. Addresses are dotted
 * quads without leading zeros.
 *
 * No allocation: cfg_t is a fixed-size struct owned by the caller. Loading
 * reads the file with blocking read() and is meant for start-up, before the
 * sample clock runs. Saving blocks too (write, fsync, rename). */
#ifndef V90D_CFG_CONFIG_H
#define V90D_CFG_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#define CFG_MAX_USERS   16
#define CFG_NAME_MAX    63     /* longest user name (same as PPP_NAME_MAX) */
#define CFG_SECRET_MAX  64     /* longest secret (same as PPP_SECRET_MAX) and
                                  longest admin password */
#define CFG_NUMBER_MAX  20     /* longest phone number */
#define CFG_PATH_MAX    255    /* longest path cfg_save takes */
#define CFG_LINE_MAX    640    /* longest line, without the line end */
#define CFG_FILE_MAX    16384  /* largest file cfg_load reads */
#define CFG_ERR_MAX     200    /* an error buffer of this size never truncates
                                  for file names up to 60 characters */
/* cfg_write output never exceeds these (including the NUL) */
#define CFG_TEXT_MAX          10752  /* with secrets */
#define CFG_TEXT_REDACTED_MAX 6656   /* secrets redacted */

/* return values */
#define CFG_OK        0
#define CFG_DEFAULTS  1     /* cfg_load: file does not exist, defaults used */
#define CFG_ERR      (-1)

/* modulation bits */
/* modulation bits, fastest first; the order of the list in the file */
#define CFG_MOD_V90     1u
#define CFG_MOD_V34     2u
#define CFG_MOD_V32BIS  4u     /* V.32bis and V.32 */
#define CFG_MOD_V22BIS  8u
#define CFG_MOD_V22    16u
#define CFG_MOD_V21    32u
#define CFG_MOD_ALL    63u
#define CFG_MOD_COUNT   6

enum { CFG_AUTH_NONE = 0, CFG_AUTH_PAP = 1, CFG_AUTH_CHAP = 2 }; /* = PPP_AUTH_* */
enum { CFG_LAW_ULAW = 0, CFG_LAW_ALAW = 1 };
enum { CFG_TONES_US = 0, CFG_TONES_EU = 1 };
/* service: what a caller on the port gets.
 *   internet   the Internet as it is
 *   archive    web, FTP and names go to the gateway on this box (arch/): the
 *              web as it was, from Protoweb
 *   dreamcast  for a Sega Dreamcast: the port's number and the PPP login as
 *              always, but no PPP compression and no V.92, the DNS servers
 *              of the game revival projects are handed out and the packets
 *              are mended on the way (srv/console.h). Not tried with a real
 *              console. */
enum { CFG_SERVICE_INTERNET = 0, CFG_SERVICE_ARCHIVE = 1, CFG_SERVICE_DREAMCAST = 2, CFG_SERVICE_COUNT = 3 };
enum { CFG_LISTEN_OFF = 0, CFG_LISTEN_LAN = 1 };
enum { CFG_LOG_ERROR = 0, CFG_LOG_WARN = 1, CFG_LOG_INFO = 2, CFG_LOG_DEBUG = 3 };

typedef struct {
    char name[CFG_NAME_MAX + 1];      /* NUL terminated, never empty */
    char secret[CFG_SECRET_MAX + 1];  /* NUL terminated, may be empty */
} cfg_user_t;

/* what port 2 has of its own: [modem2], [line2] and ppp_peer2 */
typedef struct {
    uint32_t   ppp_peer;
    unsigned   modulations;
    int        max_down, max_up, tx_level, v42, v42bis, v92, rings, law;
    char       line_number[CFG_NUMBER_MAX + 1];
    int        line_tones, busy_chance, drop_mean_min, session_limit_min, idle_timeout_min;
    int        service;
    int        dc_now, dc_lan, dc_upnp;    /* 0/1, for the dreamcast service: report to Dreamcast Now; the console
                                            * as a host on the LAN; ask the router for the game ports (UPnP) */
} cfg_port_t;

#define CFG_PORTS 2

/* IPv4 addresses are in host byte order: 192.168.1.2 is 0xC0A80102. */
typedef struct {
    /* [network] */
    int        lan_dhcp;        /* 1 = DHCP client, 0 = static */
    uint32_t   lan_addr;
    uint32_t   lan_netmask;
    uint32_t   lan_gateway;     /* static mode; 0 = none */
    uint32_t   ppp_local;
    uint32_t   ppp_peer;
    uint32_t   dns[2];          /* 0 = none */
    int        nat;
    /* [ppp] */
    int        auth;            /* CFG_AUTH_* */
    int        mru;
    int        echo_interval;   /* seconds, 0 = off */
    int        compression;     /* 1 = PPP compression (CCP: Deflate, MPPC) is negotiated */
    int        n_users;
    cfg_user_t users[CFG_MAX_USERS];
    /* [modem] */
    unsigned   modulations;     /* CFG_MOD_* bits */
    int        max_down;        /* bit/s */
    int        max_up;          /* bit/s */
    int        tx_level;        /* dBm0 */
    int        v42;
    int        v42bis;
    int        v92;             /* 1 = offer V.92 PCM upstream to V.92 callers (needs v90 in modulations to matter) */
    int        rings;
    int        law;             /* CFG_LAW_* */
    /* [line] */
    char       line_number[CFG_NUMBER_MAX + 1];  /* "" = any number connects */
    int        line_tones;      /* CFG_TONES_* */
    int        busy_chance;     /* percent, 0..100 */
    int        drop_mean_min;   /* minutes, 0 = never */
    int        session_limit_min;   /* minutes, 0 = none */
    int        idle_timeout_min;    /* minutes, 0 = none */
    int        service;         /* CFG_SERVICE_* */
    int        dc_now, dc_lan, dc_upnp;    /* 0/1, for the dreamcast service: report to Dreamcast Now; the console
                                            * as a host on the LAN; ask the router for the game ports (UPnP) */
    /* [status] */
    int        status_listen;   /* CFG_LISTEN_* */
    int        status_port;
    char       admin_password[CFG_SECRET_MAX + 1];  /* "" = settings open */
    /* [log] */
    int        log_level;       /* CFG_LOG_* */
    /* port 2; the [modem] and [line] fields above and ppp_peer are port 1 */
    cfg_port_t port2;
} cfg_t;

/* Fill *c with the defaults. Zeroes the whole struct first, so two configs
 * with equal settings compare equal with memcmp. */
void cfg_default(cfg_t *c);

/* Check ranges and cross-field rules of a config however it was built.
 * 0 if valid; CFG_ERR and a message in err otherwise. */
int cfg_validate(const cfg_t *c, char *err, size_t errsz);

/* Parse len bytes of config text. `name` is only used in messages. Starts
 * from the defaults. Returns CFG_OK, or CFG_ERR with err holding
 * "name:line: what is wrong" (or "name: what is wrong" for a rule broken by
 * defaults alone). On error *c holds the defaults, never a half-read file.
 * err may be NULL if errsz is 0. */
int cfg_parse(cfg_t *c, const char *name, const char *text, size_t len,
              char *err, size_t errsz);

/* Read and parse a file. A file that does not exist is not an error:
 * *c gets the defaults and CFG_DEFAULTS is returned. Any other failure
 * (unreadable, too large, invalid) returns CFG_ERR with a message. */
int cfg_load(cfg_t *c, const char *path, char *err, size_t errsz);

/* Write the effective config in canonical form: every key, fixed order,
 * loadable by cfg_parse when with_secrets is nonzero. With with_secrets 0
 * each user secret and a non-empty admin password are printed as
 * <redacted> (and the text is deliberately not loadable). Never writes more
 * than cap bytes, always NUL terminates when cap > 0. Returns the length
 * without the NUL, or -1 if cap is too small. */
int cfg_write(const cfg_t *c, int with_secrets, char *buf, size_t cap);

/* Store the config in a file so that a crash or power cut leaves either
 * the old file or the new one: the canonical text with secrets goes to
 * "<path>.tmp" (mode 0600), is synced, and is renamed over path. A config
 * that does not pass cfg_validate is not written. path is at most
 * CFG_PATH_MAX characters. Returns CFG_OK, or CFG_ERR with "path: what is
 * wrong" in err; the temporary is removed on failure and path is left as
 * it was. Blocks on the file system: not for the sample loop. */
int cfg_save(const cfg_t *c, const char *path, char *err, size_t errsz);

/* The configuration as one port's server wants it: *out is *c with, for
 * port 1 (the second port; port 0 is the first), the [modem2], [line2] and
 * ppp_peer2 values in the fields of [modem], [line] and ppp_peer. */
void cfg_for_port(const cfg_t *c, int port, cfg_t *out);

/* One dotted IPv4 address, nothing around it, into host byte order.
 * Returns 0, or -1 if v is not one. */
int cfg_parse_ip(const char *v, uint32_t *out);

#endif
