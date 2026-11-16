/* ITU-T V.44 (11/2000) data compression, stream method: encoder and decoder.
 *
 * Written from the Recommendation text (refs/itu/T-REC-V.44-200011-I).
 * One encoder and one decoder per direction. The encoder dictionary (root
 * array, node-tree, history; 6.2.1) and the decoder dictionary (string
 * collection, history; 6.2.2) live in fixed arrays sized by the compile-time
 * maxima below. No allocation, no floating point, no OS calls. Both calls
 * work with bounded output buffers, like v42_comp, so the caller can cut the
 * encoder output into I frames and apply flow control to the decoder output.
 *
 * With the default maxima (2048 codewords, history 4096) an encoder is
 * about 19 kB and a decoder about 10 kB; see v42/README.md.
 *
 * Not implemented: the packet and multi-packet methods of Annex B and the
 * parameter mode of 7.4.2 (an EPM command is a C-ERROR). The readings taken
 * where the text leaves a choice are listed in v42/README.md. */
#ifndef V90D_V44_COMP_H
#define V90D_V44_COMP_H
#include <stddef.h>
#include <stdint.h>

/* Compile-time ceilings for N2 (codewords, P1), and N8 (history, P3). */
#ifndef V44COMP_MAX_CODEWORDS
#define V44COMP_MAX_CODEWORDS 2048
#endif
#ifndef V44COMP_MAX_HISTORY
#define V44COMP_MAX_HISTORY   4096
#endif
/* ranges of Table 10 */
#define V44COMP_MIN_CODEWORDS 256
#define V44COMP_MIN_STRING    32
#define V44COMP_MAX_STRING    255
#define V44COMP_MIN_HISTORY   512
/* defaults of Table 10 when a proposal leaves a parameter out */
#define V44COMP_DEF_CODEWORDS 1024
#define V44COMP_DEF_STRING    255

/* Table 11 */
#define V44COMP_N3 8                  /* character size, bits */
#define V44COMP_N4 256                /* characters in the alphabet */
#define V44COMP_N5 4                  /* control codes; first codeword */

/* Table 8: control codes (compressed mode); Table 9: commands (transparent) */
enum { V44COMP_ETM = 0, V44COMP_FLUSH = 1, V44COMP_STEPUP = 2, V44COMP_REINIT = 3 };
enum { V44COMP_ECM = 0, V44COMP_EID = 1, V44COMP_EPM = 2 };

/* ------------------------------------------------------------- encoder */
enum {
    V44COMP_AUTO = 0,        /* compressibility test picks the mode (7.11.5) */
    V44COMP_ALWAYS,          /* stay in compressed mode */
    V44COMP_NEVER            /* go to transparent mode and stay there */
};

#define V44COMP_OVF 40

typedef struct {
    /* dictionary (6.2.1). A node is indexed by its codeword; 0 = no node. */
    uint16_t root[V44COMP_N4];                /* down-index of each root */
    uint16_t pos[V44COMP_MAX_CODEWORDS];      /* history position of the first
                                                 character of the string-segment */
    uint16_t down[V44COMP_MAX_CODEWORDS];     /* a segment that follows this one */
    uint16_t side[V44COMP_MAX_CODEWORDS];     /* next segment at the same level */
    uint8_t  len[V44COMP_MAX_CODEWORDS];      /* characters in the segment */
    uint8_t  hist[V44COMP_MAX_HISTORY];
    uint16_t n2, n7, n8;     /* negotiated N2T, N7T, N8T */
    int      n1;             /* maximum codeword size, bits (Table 11) */
    uint16_t c1;             /* next codeword (Table 12) */
    int      c2;             /* current codeword size, bits */
    uint32_t c3;             /* threshold for changing the codeword size */
    uint16_t c4;             /* current history position: characters held */
    int      c5;             /* current ordinal size, bits */

    /* string matching and string extension in progress (6.3) */
    uint8_t  st;             /* nothing held / first character / in the tree */
    uint8_t  rootch;         /* first character of the string */
    uint16_t cur;            /* last completely matched string-segment */
    uint16_t slen;           /* length of the string matched down to cur */
    uint16_t j;              /* characters matched after cur */
    uint16_t lim;            /* longest segment below cur that may still match;
                                0 = string extension */
    uint16_t best, bestlen;  /* longest segment below cur matched completely */
    uint16_t mpos;           /* history position of the character after cur */
    uint16_t epos;           /* history position after cur's own segment */
    uint16_t scan;           /* next history position to look at */
    uint8_t  pend;           /* after a flush: the next character is appended
                                to this root or segment (6.3.2, 7.13) */
    uint8_t  pend_ch;
    uint16_t pend_node, pend_slen;
    uint8_t  term;           /* matching is being brought to an end: why */
    uint8_t  dirty;          /* characters taken since the last flush */

    int      compressed;     /* 1 = compressed mode, 0 = transparent mode */
    int      after_cw;       /* the last code transferred was a codeword (6.6.3) */
    uint8_t  esc;            /* ESCAPE (7.14) */
    int      policy;
    uint32_t bitbuf;         /* code bits not yet making a full octet */
    int      nbits;
    uint8_t  ovf[V44COMP_OVF];   /* output that did not fit the caller's buffer */
    int      ovf_len, ovf_pos;
    uint8_t *out;            /* caller's buffer during a call */
    size_t   out_max, out_len;
    uint32_t win_chars, win_bits;    /* compressibility test window (7.11.5) */
    unsigned long in_chars, out_octets, mode_switches;
    unsigned long reinits;   /* dictionary reinitializations (7.12) */
    unsigned long nodes;     /* string-segments created */
} v44comp_enc_t;

/* C-INIT (7.5). n2 = number of codewords, n7 = maximum string length, n8 =
 * history length. Returns 0, or -1 if a value is outside what is supported
 * (256..V44COMP_MAX_CODEWORDS, 32..255, 512..V44COMP_MAX_HISTORY). The
 * encoder starts in compressed mode. */
int    v44comp_enc_init(v44comp_enc_t *e, int n2, int n7, int n8);
void   v44comp_enc_policy(v44comp_enc_t *e, int policy);
/* C-DATA request. Consumes up to inlen characters, appending encoder output
 * to out[*outlen..outmax). Returns the number of characters consumed; it
 * stops early when out is full. */
size_t v44comp_encode(v44comp_enc_t *e, const uint8_t *in, size_t inlen,
                      uint8_t *out, size_t outmax, size_t *outlen);
/* C-FLUSH request (7.13): push out everything given so far, octet aligned.
 * Returns 1 when all of it is in out, 0 if out filled up first (call again
 * with more room). */
int    v44comp_flush(v44comp_enc_t *e, uint8_t *out, size_t outmax, size_t *outlen);
/* Octets produced but not yet handed to the caller. */
size_t v44comp_enc_pending(const v44comp_enc_t *e);

/* Transfer of one code as 6.6 lays it down, with the code prefix of 6.6.3,
 * STEPUP as 7.11.1 and 7.11.2 require, and for ETM and FLUSH the 0 bits up
 * to the octet boundary. It is what the encoder uses; it is public so that
 * the packing can be checked against Table 6 on its own. */
enum { V44COMP_CODE_CONTROL = 0, V44COMP_CODE_ORDINAL, V44COMP_CODE_CODEWORD, V44COMP_CODE_EXTLEN };
void   v44comp_enc_put(v44comp_enc_t *e, int type, unsigned value,
                       uint8_t *out, size_t outmax, size_t *outlen);

/* ------------------------------------------------------------- decoder */
/* C-ERROR indications (7.15). The first three are the conditions the
 * Recommendation lists; the others are losses of synchronization that only
 * a broken peer or a corrupt stream can produce. */
#define V44COMP_ERR_STEPUP   (-1)   /* STEPUP would take C2 past N1 */
#define V44COMP_ERR_ORDINAL  (-2)   /* STEPUP would take C5 past 8 */
#define V44COMP_ERR_CODEWORD (-3)   /* codeword greater than C1, or equal to
                                       C1 with nothing to build it from */
#define V44COMP_ERR_COMMAND  (-4)   /* EPM or a reserved command code */
#define V44COMP_ERR_EXTEND   (-5)   /* string-extension length past N7 */
#define V44COMP_ERR_HISTORY  (-6)   /* more characters than the history holds */

typedef struct {
    /* dictionary (6.2.2); a string is indexed by its codeword */
    uint16_t last[V44COMP_MAX_CODEWORDS];     /* history position of the last
                                                 character of the string */
    uint8_t  slen[V44COMP_MAX_CODEWORDS];     /* length of the string */
    uint8_t  hist[V44COMP_MAX_HISTORY];
    uint16_t n2, n7, n8;     /* negotiated N2R, N7R, N8R */
    int      n1;
    uint16_t c1;
    int      c2;
    uint16_t c4;
    int      c5;
    uint16_t done;           /* history position delivered to the caller */
    int      prev;           /* previous code, for Table 2 */
    uint16_t prev_len;       /* length of the string it stood for */
    uint16_t prev_cw;        /* the codeword, for a string-extension length */
    int      after_cw;       /* the last code received was a codeword (6.6.3) */
    int      compressed;
    uint8_t  esc;
    int      escaped;        /* transparent mode: last octet was the ESCAPE */
    uint32_t bitbuf;
    int      nbits;
    int      error;          /* sticky C-ERROR */
    unsigned long in_octets, out_chars, reinits;
} v44comp_dec_t;

int  v44comp_dec_init(v44comp_dec_t *x, int n2, int n7, int n8);
/* C-TRANSFER request. Decodes octets from in, appending characters to
 * out[*outlen..outmax). *consumed is set to the octets taken. It stops when
 * out is full; any room at all guarantees progress. Returns 0, or a
 * V44COMP_ERR_* code (sticky). */
int  v44comp_decode(v44comp_dec_t *x, const uint8_t *in, size_t inlen, size_t *consumed,
                    uint8_t *out, size_t outmax, size_t *outlen);
/* True when decoded characters are waiting for output room, so
 * v44comp_decode() should be called again even with no new input. */
int  v44comp_dec_pending(const v44comp_dec_t *x);

#endif
