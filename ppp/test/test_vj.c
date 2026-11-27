/* ppp_vj.c (RFC 1144): streams of TCP/IP packets through the compressor
 * and the decompressor, each rebuilt to the octet; what goes as plain IP;
 * more connections than slots; the slot number left out or not; tossing
 * after a lost frame; malformed and random input to the decompressor. */
#include <string.h>
#include <stdio.h>
#include "test.h"
#include "ppp_vj.h"

static uint32_t rnd = 12345;
static uint32_t rnext(void) { rnd = rnd * 1103515245u + 12345u; return rnd >> 8; }

static uint16_t ip_sum(const uint8_t *h, size_t n)
{
    uint32_t s = 0; size_t i;
    for (i = 0; i + 1 < n; i += 2) s += (uint32_t)h[i] << 8 | h[i + 1];
    while (s >> 16) s = (s & 0xffff) + (s >> 16);
    return (uint16_t)~s;
}

typedef struct {
    uint32_t src, dst; uint16_t sport, dport;
    uint32_t seq, ack; uint16_t win, id, urp; uint8_t flags;
    uint8_t ttl, tos; int topt;    /* TCP options length (0 or 12) */
    uint32_t tsval;
} conn_t;

#define F_FIN 0x01
#define F_SYN 0x02
#define F_RST 0x04
#define F_PSH 0x08
#define F_ACK 0x10
#define F_URG 0x20

/* a TCP/IP packet of c with `data` octets of payload */
static size_t mk(uint8_t *p, const conn_t *c, size_t data)
{
    size_t th = 20 + (size_t)c->topt, len = 20 + th + data, i;
    memset(p, 0, 20 + th);
    p[0] = 0x45; p[1] = c->tos; p[2] = (uint8_t)(len >> 8); p[3] = (uint8_t)len;
    p[4] = (uint8_t)(c->id >> 8); p[5] = (uint8_t)c->id; p[6] = 0x40;   /* DF */
    p[8] = c->ttl; p[9] = 6;
    p[12] = (uint8_t)(c->src >> 24); p[13] = (uint8_t)(c->src >> 16); p[14] = (uint8_t)(c->src >> 8); p[15] = (uint8_t)c->src;
    p[16] = (uint8_t)(c->dst >> 24); p[17] = (uint8_t)(c->dst >> 16); p[18] = (uint8_t)(c->dst >> 8); p[19] = (uint8_t)c->dst;
    { uint16_t s = ip_sum(p, 20); p[10] = (uint8_t)(s >> 8); p[11] = (uint8_t)s; }
    uint8_t *t = p + 20;
    t[0] = (uint8_t)(c->sport >> 8); t[1] = (uint8_t)c->sport; t[2] = (uint8_t)(c->dport >> 8); t[3] = (uint8_t)c->dport;
    t[4] = (uint8_t)(c->seq >> 24); t[5] = (uint8_t)(c->seq >> 16); t[6] = (uint8_t)(c->seq >> 8); t[7] = (uint8_t)c->seq;
    t[8] = (uint8_t)(c->ack >> 24); t[9] = (uint8_t)(c->ack >> 16); t[10] = (uint8_t)(c->ack >> 8); t[11] = (uint8_t)c->ack;
    t[12] = (uint8_t)((th / 4) << 4); t[13] = c->flags;
    t[14] = (uint8_t)(c->win >> 8); t[15] = (uint8_t)c->win;
    t[16] = (uint8_t)rnext(); t[17] = (uint8_t)rnext();                 /* a checksum: carried, not checked */
    t[18] = (uint8_t)(c->urp >> 8); t[19] = (uint8_t)c->urp;
    if (c->topt) {                                                      /* NOP NOP timestamps */
        t[20] = 1; t[21] = 1; t[22] = 8; t[23] = 10;
        t[24] = (uint8_t)(c->tsval >> 24); t[25] = (uint8_t)(c->tsval >> 16); t[26] = (uint8_t)(c->tsval >> 8); t[27] = (uint8_t)c->tsval;
    }
    for (i = 0; i < data; i++) p[20 + th + i] = (uint8_t)(rnext() ^ i);
    return len;
}

static ppp_vj_t tx, rx;
static int sent, comp, uncomp, plain, bad;
static size_t octets_in, octets_out;

/* through both ends; the rebuilt datagram must be the one sent */
static void through(const uint8_t *p, size_t len, int drop)
{
    static uint8_t w[2000], back[2000];
    size_t n = 0;
    uint16_t proto = ppp_vj_compress(&tx, p, len, w, sizeof w, &n);
    int r;
    sent++; octets_in += len; octets_out += n;
    if (proto == PPP_PROTO_VJC) comp++; else if (proto == PPP_PROTO_VJUC) uncomp++; else if (proto == 0x0021) plain++;
    else { bad++; return; }
    if (drop) { ppp_vj_rx_lost(&rx); return; }
    if (proto == 0x0021) {
        if (n != len || memcmp(w, p, len) != 0) bad++;
        return;
    }
    r = ppp_vj_uncompress(&rx, proto, w, n, back, sizeof back);
    if (r != (int)len || memcmp(back, p, len) != 0) {
        if (bad < 5) printf("    packet %d (proto %04x, %u octets in, %u sent) came back as %d octets%s\n",
                            sent, proto, (unsigned)len, (unsigned)n, r, r == (int)len ? ", different" : "");
        bad++;
    }
}

static void reset_counts(void) { sent = comp = uncomp = plain = bad = 0; octets_in = octets_out = 0; }

static void conn_init(conn_t *c, int k)
{
    memset(c, 0, sizeof *c);
    c->src = 0x0a404002u; c->dst = 0x5db8d822u + (uint32_t)k;
    c->sport = (uint16_t)(1025 + k); c->dport = 80;
    c->seq = 0x10000000u * (uint32_t)(k + 1) + 7; c->ack = 0x7fff0000u + (uint32_t)k;
    c->win = 8760; c->id = (uint16_t)(100 * k + 1); c->ttl = 64; c->flags = F_ACK;
}

/* the traffic of a dial-up session: bulk, acks, typing, window changes */
static void test_stream(void)
{
    static uint8_t p[2000];
    conn_t a, b;
    int i;

    ppp_vj_tx_init(&tx, 16, 1);
    ppp_vj_rx_init(&rx, 16);
    reset_counts();
    conn_init(&a, 0); conn_init(&b, 1);
    for (i = 0; i < 3000; i++) {
        conn_t *c = (i % 5 == 0) ? &b : &a;
        size_t data;
        unsigned kind = rnext() % 100;     /* urgent data is rare in real traffic (2%) */
        c->flags = F_ACK;
        if (kind < 40) {                        /* bulk: data, seq moves */
            data = 1 + rnext() % 1400;
            if (rnext() % 3 == 0) c->flags |= F_PSH;
            c->seq += 0; /* seq of this packet as it is */
            c->id++;
            through(p, mk(p, c, data), 0);
            c->seq += (uint32_t)data;
        } else if (kind < 60) {                 /* a pure ack */
            c->ack += 1 + rnext() % 3000;
            c->id++;
            through(p, mk(p, c, 0), 0);
        } else if (kind < 80) {                 /* typing: echo, seq and ack both move by the data */
            data = 1 + rnext() % 3;
            c->id++;
            through(p, mk(p, c, data), 0);
            c->seq += (uint32_t)data; c->ack += (uint32_t)data;
        } else if (kind < 98) {                 /* the window changes, the id jumps */
            c->win = (uint16_t)(c->win + (rnext() % 2000) - 1000);
            c->id = (uint16_t)(c->id + 1 + rnext() % 300);
            data = rnext() % 200;
            through(p, mk(p, c, data), 0);
            c->seq += (uint32_t)data;
        } else {                                /* urgent data now and then */
            c->flags |= F_URG; c->urp = (uint16_t)(1 + rnext() % 50);
            c->id++;
            data = 1 + rnext() % 60;
            through(p, mk(p, c, data), 0);
            c->seq += (uint32_t)data;
            c->urp = 0;
        }
    }
    CHECK_EQ_I(bad, 0);
    CHECK(comp > sent * 8 / 10);                /* most of it compressed */
    CHECK(uncomp >= 2 && uncomp < 120);         /* the first of each, and after an urgent one */
    printf("    stream: %d packets, %d compressed, %d uncompressed, %d as IP; %lu octets -> %lu\n",
           sent, comp, uncomp, plain, (unsigned long)octets_in, (unsigned long)octets_out);

    /* the sequence number wraps; jumps past 0xffff go uncompressed */
    reset_counts();
    a.seq = 0xffffff00u;
    for (i = 0; i < 20; i++) { a.id++; through(p, mk(p, &a, 100), 0); a.seq += 100; }
    a.seq += 70000; a.id++;
    through(p, mk(p, &a, 10), 0);
    CHECK_EQ_I(bad, 0);
    CHECK(uncomp >= 1);

    /* a typed character and its echo: the special cases, a few octets each */
    reset_counts();
    for (i = 0; i < 50; i++) {
        a.flags = F_ACK | F_PSH; a.id++;
        through(p, mk(p, &a, 1), 0);
        a.seq += 1; a.ack += 1;
    }
    CHECK_EQ_I(bad, 0);
    CHECK(octets_out < octets_in / 6);          /* 41 octets each down to about 4-5 */
}

/* what is not compressed: other protocols, fragments, SYN/FIN/RST, no ACK */
static void test_plain(void)
{
    static uint8_t p[2000], w[2000];
    conn_t c;
    size_t len, n;

    ppp_vj_tx_init(&tx, 16, 1);
    conn_init(&c, 3);
    len = mk(p, &c, 10);
    p[9] = 17;                                  /* UDP */
    CHECK_EQ_I(ppp_vj_compress(&tx, p, len, w, sizeof w, &n), 0x0021);
    CHECK(n == len && memcmp(w, p, len) == 0);
    len = mk(p, &c, 10);
    p[6] = 0x20;                                /* more fragments */
    CHECK_EQ_I(ppp_vj_compress(&tx, p, len, w, sizeof w, &n), 0x0021);
    len = mk(p, &c, 10);
    p[7] = 0x08; p[6] = 0;                      /* a later fragment */
    CHECK_EQ_I(ppp_vj_compress(&tx, p, len, w, sizeof w, &n), 0x0021);
    c.flags = F_SYN | F_ACK;
    CHECK_EQ_I(ppp_vj_compress(&tx, p, mk(p, &c, 0), w, sizeof w, &n), 0x0021);
    c.flags = F_FIN | F_ACK;
    CHECK_EQ_I(ppp_vj_compress(&tx, p, mk(p, &c, 0), w, sizeof w, &n), 0x0021);
    c.flags = F_RST | F_ACK;
    CHECK_EQ_I(ppp_vj_compress(&tx, p, mk(p, &c, 0), w, sizeof w, &n), 0x0021);
    c.flags = F_PSH;                            /* no ACK */
    CHECK_EQ_I(ppp_vj_compress(&tx, p, mk(p, &c, 5), w, sizeof w, &n), 0x0021);
    len = mk(p, &c, 0);
    CHECK_EQ_I(ppp_vj_compress(&tx, p, 30, w, sizeof w, &n), 0x0021);   /* shorter than its headers */
    /* off: everything as it is */
    ppp_vj_tx_init(&tx, 0, 1);
    c.flags = F_ACK;
    len = mk(p, &c, 10);
    CHECK_EQ_I(ppp_vj_compress(&tx, p, len, w, sizeof w, &n), 0x0021);
    CHECK_EQ_I(ppp_vj_compress(&tx, p, len, w, sizeof w, &n), 0x0021);
    /* the first packet of a connection goes uncompressed, the next compressed */
    ppp_vj_tx_init(&tx, 16, 1);
    CHECK_EQ_I(ppp_vj_compress(&tx, p, len, w, sizeof w, &n), PPP_PROTO_VJUC);
    CHECK(n == len && w[9] < 16);               /* the slot where the protocol was */
    c.seq += 10; c.id++;
    len = mk(p, &c, 10);
    CHECK_EQ_I(ppp_vj_compress(&tx, p, len, w, sizeof w, &n), PPP_PROTO_VJC);
    CHECK(n < len - 30);
    /* a TCP option that changes (timestamps) re-states the header */
    c.topt = 12; c.tsval = 1;
    c.id++;
    CHECK_EQ_I(ppp_vj_compress(&tx, p, mk(p, &c, 10), w, sizeof w, &n), PPP_PROTO_VJUC);
    c.tsval = 2; c.id++;
    CHECK_EQ_I(ppp_vj_compress(&tx, p, mk(p, &c, 10), w, sizeof w, &n), PPP_PROTO_VJUC);
    /* the same options again: compressed */
    c.id++; c.seq += 10;
    CHECK_EQ_I(ppp_vj_compress(&tx, p, mk(p, &c, 10), w, sizeof w, &n), PPP_PROTO_VJC);
    /* too small an output buffer */
    c.id++;
    CHECK_EQ_I(ppp_vj_compress(&tx, p, mk(p, &c, 10), w, 4, &n), 0);
}

/* more connections than slots, slot counts from 1 to 16, slot number always sent */
static void test_slots(void)
{
    static uint8_t p[2000];
    conn_t c[24];
    unsigned slots;
    int i, k;

    for (slots = 1; slots <= 16; slots += 5) {
        for (k = 0; k < 2; k++) {
            ppp_vj_tx_init(&tx, slots, k);
            ppp_vj_rx_init(&rx, slots);
            reset_counts();
            for (i = 0; i < 24; i++) conn_init(&c[i], i);
            for (i = 0; i < 2000; i++) {
                conn_t *x = &c[(i * 7 + (int)(rnext() % 3)) % 24];
                size_t d = rnext() % 300;
                x->id++;
                through(p, mk(p, x, d), 0);
                x->seq += (uint32_t)d;
                if (rnext() % 4 == 0) x->ack += 100;
            }
            CHECK_EQ_I(bad, 0);
        }
    }
    /* without Comp-Slot-Id every compressed packet names its slot */
    {
        static uint8_t w[2000];
        conn_t a; size_t n;
        ppp_vj_tx_init(&tx, 16, 0);
        conn_init(&a, 0);
        (void)ppp_vj_compress(&tx, p, mk(p, &a, 10), w, sizeof w, &n);
        for (i = 0; i < 5; i++) {
            a.seq += 10; a.id++;
            CHECK_EQ_I(ppp_vj_compress(&tx, p, mk(p, &a, 10), w, sizeof w, &n), PPP_PROTO_VJC);
            CHECK(w[0] & 0x40);
        }
        /* with it, the same slot twice running leaves the number out */
        ppp_vj_tx_init(&tx, 16, 1);
        (void)ppp_vj_compress(&tx, p, mk(p, &a, 10), w, sizeof w, &n);
        a.seq += 10; a.id++;
        (void)ppp_vj_compress(&tx, p, mk(p, &a, 10), w, sizeof w, &n);
        a.seq += 10; a.id++;
        CHECK_EQ_I(ppp_vj_compress(&tx, p, mk(p, &a, 10), w, sizeof w, &n), PPP_PROTO_VJC);
        CHECK(!(w[0] & 0x40));
    }
}

/* RFC 1144 4.2: after a lost frame the receiver tosses compressed packets
 * that do not name their slot. The sender's TCP then resends what was lost:
 * its sequence number goes back, which the compressor sends uncompressed,
 * and that puts the slot right again; from there on all is rebuilt exactly */
static void test_toss(void)
{
    static uint8_t p[2000], w[2000], back[2000];
    conn_t a;
    size_t n;
    uint32_t lost_seq;
    int i, r, tossed = 0;
    uint16_t proto;

    ppp_vj_tx_init(&tx, 16, 1);
    ppp_vj_rx_init(&rx, 16);
    reset_counts();
    conn_init(&a, 0);
    for (i = 0; i < 6; i++) { a.id++; through(p, mk(p, &a, 50), 0); a.seq += 50; }
    CHECK_EQ_I(bad, 0);
    lost_seq = a.seq;
    a.id++; through(p, mk(p, &a, 50), 1); a.seq += 50;      /* lost on the line */
    for (i = 0; i < 5; i++) {
        a.id++;
        proto = ppp_vj_compress(&tx, p, mk(p, &a, 50), w, sizeof w, &n);
        a.seq += 50;
        CHECK_EQ_I(proto, PPP_PROTO_VJC);
        r = ppp_vj_uncompress(&rx, proto, w, n, back, sizeof back);
        if (r < 0) tossed++;
    }
    CHECK_EQ_I(tossed, 5);
    /* the resend: back to the lost sequence number, uncompressed */
    a.seq = lost_seq; a.id++;
    proto = ppp_vj_compress(&tx, p, mk(p, &a, 50), w, sizeof w, &n);
    CHECK_EQ_I(proto, PPP_PROTO_VJUC);
    r = ppp_vj_uncompress(&rx, proto, w, n, back, sizeof back);
    CHECK(r == (int)(40 + 50) && memcmp(back, p, (size_t)r) == 0);
    a.seq += 50;
    reset_counts();
    for (i = 0; i < 20; i++) { a.id++; through(p, mk(p, &a, 40), 0); a.seq += 40; }
    CHECK_EQ_I(bad, 0);
    CHECK_EQ_I(comp, 20);
    /* a compressed packet that names its slot also ends the tossing */
    ppp_vj_tx_init(&tx, 16, 0);                 /* slot number always sent */
    ppp_vj_rx_init(&rx, 16);
    reset_counts();
    for (i = 0; i < 3; i++) { a.id++; through(p, mk(p, &a, 40), 0); a.seq += 40; }
    ppp_vj_rx_lost(&rx);                        /* lost, but nothing of a's was */
    for (i = 0; i < 3; i++) { a.id++; through(p, mk(p, &a, 40), 0); a.seq += 40; }
    CHECK_EQ_I(bad, 0);
}

/* the decompressor on what it should not get */
static void test_malformed(void)
{
    static uint8_t p[2000], w[2000], back[2000];
    conn_t a;
    size_t n, len;
    int i;

    ppp_vj_rx_init(&rx, 16);
    /* compressed, naming a slot that holds nothing */
    w[0] = 0x40 | 0x08; w[1] = 3; w[2] = 0; w[3] = 0; w[4] = 5;
    CHECK_EQ_I(ppp_vj_uncompress(&rx, PPP_PROTO_VJC, w, 5, back, sizeof back), -1);
    /* compressed with no slot named and none before */
    ppp_vj_rx_init(&rx, 16);
    w[0] = 0x08; w[1] = 0; w[2] = 0; w[3] = 5;
    CHECK_EQ_I(ppp_vj_uncompress(&rx, PPP_PROTO_VJC, w, 4, back, sizeof back), -1);
    /* uncompressed with a slot past ours */
    ppp_vj_tx_init(&tx, 16, 1);
    ppp_vj_rx_init(&rx, 4);
    conn_init(&a, 0);
    len = mk(p, &a, 10);
    memcpy(w, p, len); w[9] = 7;
    CHECK_EQ_I(ppp_vj_uncompress(&rx, PPP_PROTO_VJUC, w, len, back, sizeof back), -1);
    /* uncompressed that is not an IPv4 TCP header */
    memcpy(w, p, len); w[9] = 0; w[0] = 0x65;
    CHECK_EQ_I(ppp_vj_uncompress(&rx, PPP_PROTO_VJUC, w, len, back, sizeof back), -1);
    memcpy(w, p, len); w[9] = 0;
    CHECK_EQ_I(ppp_vj_uncompress(&rx, PPP_PROTO_VJUC, w, 30, back, sizeof back), -1);    /* cut short */
    CHECK_EQ_I(ppp_vj_uncompress(&rx, PPP_PROTO_VJUC, w, len, back, 20), -1);           /* no room */
    CHECK_EQ_I(ppp_vj_uncompress(&rx, PPP_PROTO_VJUC, w, len, back, sizeof back), (int)len);
    /* a compressed packet cut short in its deltas */
    ppp_vj_rx_init(&rx, 16);
    ppp_vj_tx_init(&tx, 16, 1);
    (void)ppp_vj_compress(&tx, p, len, w, sizeof w, &n);
    CHECK_EQ_I(ppp_vj_uncompress(&rx, PPP_PROTO_VJUC, w, n, back, sizeof back), (int)len);
    a.seq += 10; a.ack += 70000 - 69000; a.win = 100; a.id += 5;
    (void)ppp_vj_compress(&tx, p, mk(p, &a, 0), w, sizeof w, &n);
    for (i = 0; i < (int)n - 0; i++) {
        int r = ppp_vj_uncompress(&rx, PPP_PROTO_VJC, w, (size_t)i, back, sizeof back);
        CHECK(r == -1 || r >= 40);
    }
    /* other protocol numbers */
    CHECK_EQ_I(ppp_vj_uncompress(&rx, 0x0021, w, n, back, sizeof back), -1);
    /* off */
    ppp_vj_rx_init(&rx, 0);
    CHECK_EQ_I(ppp_vj_uncompress(&rx, PPP_PROTO_VJUC, p, len, back, sizeof back), -1);
}

/* random input: never past the buffers (run it under -fsanitize=address) */
static void test_fuzz(void)
{
    static uint8_t w[600], back[700];
    int i, k;
    ppp_vj_rx_init(&rx, 16);
    for (i = 0; i < 200000; i++) {
        size_t n = rnext() % sizeof w;
        uint16_t proto = (rnext() & 1) ? PPP_PROTO_VJC : PPP_PROTO_VJUC;
        int r;
        for (k = 0; k < (int)n; k++) w[k] = (uint8_t)rnext();
        if (proto == PPP_PROTO_VJUC && n > 20) { w[0] = 0x45; w[9] = (uint8_t)(rnext() % 18); }
        r = ppp_vj_uncompress(&rx, proto, w, n, back, 100 + rnext() % 600);
        if (r > 700) { CHECK(r <= 700); break; }
        if (i % 1000 == 0) ppp_vj_rx_lost(&rx);
    }
}

int main(void)
{
    test_stream();
    test_plain();
    test_slots();
    test_toss();
    test_malformed();
    test_fuzz();
    T_REPORT("vj");
}
