#include <stdio.h>
#include <string.h>
#include "test.h"
#include "../proslic.h"
#include "chipsim.h"

static chipsim C; static ps_bus B;
static void fresh(void) { chipsim_init(&C); chipsim_bus(&C, &B); }

static void test_registers(void)
{
    fresh();
    CHECK_EQ_I(ps_rd(&B, PS_ID), 0xB2); CHECK_EQ_I((int)C.frames, 3);
    ps_wr(&B, PS_PCMTXLO, 0x13); CHECK_EQ_I(C.reg[12], 0x13); CHECK_EQ_I((int)C.frames, 6);
    CHECK_EQ_I(ps_rd(&B, PS_PCMTXLO), 0x13);
    CHECK_EQ_I(C.bad, 0);
}

static void test_ram(void)
{
    uint32_t v = 0; unsigned a;
    fresh(); C.busy_polls = 3;
    /* every address below the user area, 29 bits wide */
    for (a = 0; a < PS_RAM_USER; a += 37) CHECK(ps_ram_wr(&B, a, (0x12345678ul + a * 0x10001ul) & PS_RAM_MASK) == 0);
    for (a = 0; a < PS_RAM_USER; a += 37) {
        CHECK(ps_ram_rd(&B, a, &v) == 0);
        CHECK_EQ_I((long)v, (long)((0x12345678ul + a * 0x10001ul) & PS_RAM_MASK));
        CHECK_EQ_I((long)C.ram[a], (long)v);
    }
    CHECK(ps_ram_wr(&B, 449, 0x1FFFFFFFul) == 0); CHECK(ps_ram_rd(&B, 449, &v) == 0); CHECK_EQ_I((long)v, 0x1FFFFFFFl);
    CHECK(ps_ram_wr(&B, 449, 0xFFFFFFFFul) == 0); CHECK_EQ_I((long)C.ram[449], 0x1FFFFFFFl);   /* 29 bits */
    CHECK_EQ_I(C.bad, 0);                           /* never touched the data while busy */
}

static void test_ram_dead_chip(void)
{
    uint32_t v = 77;
    fresh(); C.dead = 1;
    CHECK(ps_ram_rd(&B, 3, &v) == -1); CHECK_EQ_I((long)v, 77);
    CHECK(ps_ram_wr(&B, 3, 5) == -1); CHECK_EQ_I((long)C.ram[3], 0);
    CHECK(C.frames < 2000);                         /* gives up */
    CHECK(ps_ram_rd(&B, 4000, &v) == -1);           /* no such address */
}

static void test_user_mode(void)
{
    uint32_t v = 0;
    fresh();
    C.ram[1538] = 0x00300000;
    CHECK(ps_ram_rd(&B, 1538, &v) == 0); CHECK_EQ_I((long)v, 0);    /* hidden */
    CHECK(ps_ram_wr(&B, 1538, 0x00700000) == 0); CHECK_EQ_I((long)C.ram[1538], 0x00300000l);
    CHECK(ps_user_mode(&B, 1) == 0); CHECK(C.user);
    CHECK(ps_user_mode(&B, 1) == 0); CHECK(C.user);                 /* already on: stays on */
    CHECK(ps_ram_rd(&B, 1538, &v) == 0); CHECK_EQ_I((long)v, 0x00300000l);
    CHECK(ps_ram_wr(&B, 1538, 0x00700000) == 0); CHECK_EQ_I((long)C.ram[1538], 0x00700000l);
    CHECK(ps_user_mode(&B, 0) == 0); CHECK(!C.user);
    CHECK(ps_user_mode(&B, 0) == 0); CHECK(!C.user);
    CHECK_EQ_I(C.bad, 0);
}

int main(void)
{
    test_registers();
    test_ram();
    test_ram_dead_chip();
    test_user_mode();
    T_REPORT("proslic");
}
