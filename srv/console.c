/* Packets of a game console: see console.h. */
#include <string.h>
#include "console.h"

void console_init(console_t *k) { memset(k, 0, sizeof *k); }

/* A UDP datagram (header and all): if it is one question for an address
 * (type A, class IN) to port 53, with the name written out in full, keep
 * the name. */
static void name_asked(console_t *k, const uint8_t *u, size_t len)
{
    char name[CONSOLE_NAME_MAX + 1]; size_t at = 20, n = 0; unsigned lab;

    if (len < 8 + 12 + 5 || u[2] != 0 || u[3] != 53) return;
    if ((u[10] & 0xf8) != 0 || u[12] != 0 || u[13] != 1) return;    /* a query, standard, one question */
    for (;;) {
        if (at >= len) return;
        lab = u[at++];
        if (lab == 0) break;
        if (lab > 63 || at + lab > len) return;                     /* a pointer, or cut off */
        if (n + lab + (n ? 1 : 0) > CONSOLE_NAME_MAX) return;
        if (n) name[n++] = '.';
        memcpy(name + n, u + at, lab); n += lab; at += lab;
    }
    if (n == 0 || at + 4 > len) return;
    if (u[at] != 0 || u[at + 1] != 1 || u[at + 2] != 0 || u[at + 3] != 1) return;
    { size_t i; for (i = 0; i < n; i++) if ((unsigned char)name[i] < 0x21 || (unsigned char)name[i] > 0x7e) return; }
    name[n] = 0;
    memcpy(k->name, name, n + 1); k->name_seq++;
}

int console_from_caller(console_t *k, uint8_t *pkt, size_t len, uint32_t now_ms)
{
    size_t hl;
    unsigned sport, dport;

    if (len < 20 || (pkt[0] >> 4) != 4) return 1;
    hl = (size_t)(pkt[0] & 15) * 4;
    if (hl < 20 || hl > len) return 1;

    if (pkt[8] != CONSOLE_TTL) {
        /* RFC 1624: HC' = ~(~HC + ~m + m'), m the 16-bit word that holds
         * the TTL in its upper octet */
        uint32_t sum = (uint32_t)(~(((unsigned)pkt[10] << 8) | pkt[11]) & 0xffff);
        sum += (uint32_t)(~(((unsigned)pkt[8] << 8) | pkt[9]) & 0xffff);
        sum += ((uint32_t)CONSOLE_TTL << 8) | pkt[9];
        sum = (sum & 0xffff) + (sum >> 16);
        sum = (sum & 0xffff) + (sum >> 16);
        sum = ~sum & 0xffff;
        pkt[8] = CONSOLE_TTL;
        pkt[10] = (uint8_t)(sum >> 8); pkt[11] = (uint8_t)sum;
    }

    if (((pkt[6] & 0x1f) | pkt[7]) != 0) return 1;          /* first fragments only */
    if (pkt[9] == 17) { name_asked(k, pkt + hl, len - hl); return 1; }

    /* TCP, SYN without ACK */
    if (pkt[9] != 6 || len < hl + 14) return 1;
    if ((pkt[hl + 13] & 0x12) != 0x02) return 1;
    if ((pkt[hl + 13] & 0x3f) == 0x0a && len >= hl + 18) {
        /* SYN with PSH and nothing else: PSH goes. The checksum covers the
         * word of data offset and flags; same rule as for the TTL. */
        uint32_t sum = (uint32_t)(~(((unsigned)pkt[hl + 16] << 8) | pkt[hl + 17]) & 0xffff);
        sum += (uint32_t)(~(((unsigned)pkt[hl + 12] << 8) | pkt[hl + 13]) & 0xffff);
        pkt[hl + 13] = (uint8_t)(pkt[hl + 13] & ~0x08);
        sum += ((uint32_t)pkt[hl + 12] << 8) | pkt[hl + 13];
        sum = (sum & 0xffff) + (sum >> 16);
        sum = (sum & 0xffff) + (sum >> 16);
        sum = ~sum & 0xffff;
        pkt[hl + 16] = (uint8_t)(sum >> 8); pkt[hl + 17] = (uint8_t)sum;
    }
    sport = ((unsigned)pkt[hl] << 8) | pkt[hl + 1];
    dport = ((unsigned)pkt[hl + 2] << 8) | pkt[hl + 3];
    if (sport < CONSOLE_SYN_LO || sport > CONSOLE_SYN_HI) return 1;
    sport -= CONSOLE_SYN_LO;
    if (k->syn[sport].seen && k->syn[sport].dport == dport &&
        (uint32_t)(now_ms - k->syn[sport].ms) < CONSOLE_SYN_MS)
        return 0;
    k->syn[sport].seen = 1; k->syn[sport].dport = (uint16_t)dport; k->syn[sport].ms = now_ms;
    return 1;
}

const char *console_name(const console_t *k, uint32_t *seq) { if (seq) *seq = k->name_seq; return k->name; }
