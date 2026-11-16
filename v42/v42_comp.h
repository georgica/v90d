/* ITU-T V.42 bis (01/90) data compression: encoder and decoder.
 *
 * Written from the Recommendation text (refs/itu/T-REC-V.42bis-199001-I).
 * One encoder and one decoder per direction, each with its own dictionary
 * held in fixed arrays sized by V42COMP_MAX_CODEWORDS. No allocation, no
 * floating point, no OS calls. Both calls work with bounded output buffers
 * so the caller can cut the encoder output into I frames of at most N401
 * octets and can apply flow control to the decoder output. */
#ifndef V90D_V42_COMP_H
#define V90D_V42_COMP_H
#include <stddef.h>
#include <stdint.h>

/* Compile-time ceiling for N2, the total number of codewords (P1). */
#ifndef V42COMP_MAX_CODEWORDS
#define V42COMP_MAX_CODEWORDS 2048
#endif
#define V42COMP_MIN_CODEWORDS 512     /* minimum and default P1 (clause 5.1) */
#define V42COMP_MIN_STRING    6       /* minimum and default P2 */
#define V42COMP_MAX_STRING    250     /* maximum P2 */

/* clause 10 constants */
#define V42COMP_N3 8                  /* character size, bits */
#define V42COMP_N4 256                /* characters in the alphabet */
#define V42COMP_N6 3                  /* control codewords */
#define V42COMP_N5 (V42COMP_N4 + V42COMP_N6)   /* first string entry */

/* Dictionary: a set of trees, one root per character (clause 6.1). Entry i
 * is the node with codeword i. Entries 0..2 are the control codewords and
 * are never used as nodes. */
typedef struct {
    uint16_t parent[V42COMP_MAX_CODEWORDS];   /* 0 = empty entry (or root) */
    uint16_t child[V42COMP_MAX_CODEWORDS];    /* first dependant, 0 = leaf */
    uint16_t sibling[V42COMP_MAX_CODEWORDS];  /* next dependant of parent */
    uint8_t  ch[V42COMP_MAX_CODEWORDS];       /* last character of string */
    uint16_t n2;             /* total number of codewords in use */
    uint16_t n7;             /* maximum string length */
    uint16_t c1;             /* next empty entry (6.5) */
    uint16_t str;            /* current string, 0 = none */
    uint16_t str_len;
    uint16_t last_new;       /* entry made by the last string match (6.3) */
    int      exc;            /* string already transferred: the next
                                character only updates the dictionary and
                                restarts matching (6.3 last paragraph) */
    unsigned long added;     /* entries created */
    unsigned long recovered; /* leaf entries detached for re-use (6.5) */
} v42comp_dict_t;

/* ------------------------------------------------------------- encoder */
enum {
    V42COMP_AUTO = 0,        /* compressibility test picks the mode (7.8) */
    V42COMP_ALWAYS,          /* enter compressed mode and stay there */
    V42COMP_NEVER            /* stay in transparent mode */
};

#define V42COMP_OVF 64

typedef struct {
    v42comp_dict_t d;
    int      compressed;     /* 0 = transparent mode, 1 = compressed mode */
    int      n1;             /* maximum codeword size, bits */
    int      c2;             /* current codeword size, bits */
    uint32_t c3;             /* threshold for codeword size change */
    uint8_t  esc;            /* escape character (9.2) */
    int      policy;
    uint32_t bitbuf;         /* codeword bits not yet making a full octet */
    int      nbits;
    uint8_t  ovf[V42COMP_OVF];   /* output that did not fit the caller's buffer */
    int      ovf_len, ovf_pos;
    uint8_t *out;            /* caller's buffer during a call */
    size_t   out_max, out_len;
    uint32_t win_chars, win_bits;    /* compressibility test window */
    unsigned long in_chars, out_octets, mode_switches;
} v42comp_enc_t;

/* C-INIT. Returns 0, or -1 if n2 or n7 is outside what is supported
 * (512..V42COMP_MAX_CODEWORDS and 6..250). */
int    v42comp_enc_init(v42comp_enc_t *e, int n2, int n7);
void   v42comp_enc_policy(v42comp_enc_t *e, int policy);
/* C-DATA request. Consumes up to inlen characters, appending encoder output
 * to out[*outlen..outmax). Returns the number of characters consumed; it
 * stops early when out is full. */
size_t v42comp_encode(v42comp_enc_t *e, const uint8_t *in, size_t inlen,
                      uint8_t *out, size_t outmax, size_t *outlen);
/* C-FLUSH request (7.9): push out everything given so far, octet aligned.
 * Returns 1 when all of it is in out, 0 if out filled up first (call again
 * with more room). */
int    v42comp_flush(v42comp_enc_t *e, uint8_t *out, size_t outmax, size_t *outlen);
/* Octets produced but not yet handed to the caller. */
size_t v42comp_enc_pending(const v42comp_enc_t *e);

/* ------------------------------------------------------------- decoder */
/* C-ERROR indications (clause 5.8) */
#define V42COMP_ERR_STEPUP   (-1)   /* STEPUP would take C2 past N1 */
#define V42COMP_ERR_C1       (-2)   /* codeword equal to C1 */
#define V42COMP_ERR_EMPTY    (-3)   /* codeword of an empty entry */
#define V42COMP_ERR_COMMAND  (-4)   /* reserved command code */
#define V42COMP_ERR_RANGE    (-5)   /* codeword beyond N2, or broken tree */

typedef struct {
    v42comp_dict_t d;
    int      compressed;
    int      n1;
    int      c2;
    uint8_t  esc;
    int      escaped;        /* transparent mode: last octet was the escape */
    uint32_t bitbuf;
    int      nbits;
    int      error;          /* sticky C-ERROR */
    unsigned long in_octets, out_chars, resets;
} v42comp_dec_t;

int  v42comp_dec_init(v42comp_dec_t *x, int n2, int n7);
/* C-TRANSFER request. Decodes octets from in, appending characters to
 * out[*outlen..outmax). *consumed is set to the octets taken. Stops early
 * when the next item does not fit; offer at least N7 octets of room to
 * guarantee progress. Returns 0, or a V42COMP_ERR_* code (sticky). */
int  v42comp_decode(v42comp_dec_t *x, const uint8_t *in, size_t inlen, size_t *consumed,
                    uint8_t *out, size_t outmax, size_t *outlen);
/* True when a complete codeword is buffered waiting for output room, so
 * v42comp_decode() should be called again even with no new input. */
int  v42comp_dec_pending(const v42comp_dec_t *x);

#endif
