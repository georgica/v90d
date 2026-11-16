/* See proslic.h. */
#include "proslic.h"

#define CTL_WRITE 0x20          /* register access, channel 0 */
#define CTL_READ  0x60
#define READY_POLLS 60          /* reads of RAMSTAT before giving up */

uint8_t ps_rd(const ps_bus *b, uint8_t reg)
{
    b->xfer(b->ctx, CTL_READ);
    b->xfer(b->ctx, reg);
    return b->xfer(b->ctx, 0xFF);
}

void ps_wr(const ps_bus *b, uint8_t reg, uint8_t v)
{
    b->xfer(b->ctx, CTL_WRITE);
    b->xfer(b->ctx, reg);
    b->xfer(b->ctx, v);
}

static int ready(const ps_bus *b)
{
    int i;
    for (i = 0; i < READY_POLLS; i++)
        if (!(ps_rd(b, PS_RAMSTAT) & 1)) return 0;
    return -1;
}

int ps_ram_rd(const ps_bus *b, unsigned addr, uint32_t *v)
{
    uint32_t w;
    if (addr > PS_RAM_MAX || ready(b)) return -1;
    ps_wr(b, PS_RAM_ADDR_HI, (uint8_t)((addr >> 3) & 0xE0));
    ps_wr(b, PS_RAM_ADDR_LO, (uint8_t)addr);
    if (ready(b)) return -1;
    w  = (uint32_t)ps_rd(b, PS_RAM_DATA_B0) >> 3;
    w |= (uint32_t)ps_rd(b, PS_RAM_DATA_B0 + 1) << 5;
    w |= (uint32_t)ps_rd(b, PS_RAM_DATA_B0 + 2) << 13;
    w |= (uint32_t)ps_rd(b, PS_RAM_DATA_B0 + 3) << 21;
    *v = w & PS_RAM_MASK;
    return 0;
}

int ps_ram_wr(const ps_bus *b, unsigned addr, uint32_t v)
{
    if (addr > PS_RAM_MAX || ready(b)) return -1;
    ps_wr(b, PS_RAM_ADDR_HI, (uint8_t)((addr >> 3) & 0xE0));
    ps_wr(b, PS_RAM_DATA_B0,     (uint8_t)((v << 3) & 0xF8));
    ps_wr(b, PS_RAM_DATA_B0 + 1, (uint8_t)(v >> 5));
    ps_wr(b, PS_RAM_DATA_B0 + 2, (uint8_t)(v >> 13));
    ps_wr(b, PS_RAM_DATA_B0 + 3, (uint8_t)(v >> 21));
    ps_wr(b, PS_RAM_ADDR_LO, (uint8_t)addr);
    return ready(b);
}

int ps_user_mode(const ps_bus *b, int on)
{
    if (((ps_rd(b, PS_TEST_CNTL) & 1) != 0) == (on != 0)) return 0;
    ps_wr(b, PS_TEST_CNTL, 0x02);
    ps_wr(b, PS_TEST_CNTL, 0x08);
    ps_wr(b, PS_TEST_CNTL, 0x0E);
    ps_wr(b, PS_TEST_CNTL, 0x00);
    return ((ps_rd(b, PS_TEST_CNTL) & 1) != 0) == (on != 0) ? 0 : -1;
}
