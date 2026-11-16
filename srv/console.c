/* Packets of a game console: see console.h. */
#include <string.h>
#include "console.h"

void console_init(console_t *k) { memset(k, 0, sizeof *k); }

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

    /* TCP, first fragment, SYN without ACK */
    if (pkt[9] != 6 || ((pkt[6] & 0x1f) | pkt[7]) != 0 || len < hl + 14) return 1;
    if ((pkt[hl + 13] & 0x12) != 0x02) return 1;
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
