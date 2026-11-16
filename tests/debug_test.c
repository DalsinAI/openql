/*
 * The debugger's cases (src/debug.h, src/cpu/dis68k.h): a 68040 running a
 * small program from RAM, stopped at breakpoints, stepped over and into a
 * call, and the disassembler against known instructions (each checked
 * against GNU objdump's m68k disassembler when written).
 */
#include "debug.h"
#include "cpu/dis68k.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint8_t ram[0x10000], rom[0x10000];
static int io_reads, io_writes;
static uint32_t io_read(void *o, uint32_t a, int size) { (void)o; (void)a; (void)size; io_reads++; return 0; }
static void io_write(void *o, uint32_t a, uint32_t v, int size) { (void)o; (void)a; (void)v; (void)size; io_writes++; }

/* as the boards run: slices, ended early when the debugger stops the CPU */
static void run(ac090 *c, uint64_t clocks) {
    uint64_t until = c->clock + clocks;
    while (c->clock < until && !c->halted && !c->dbg_stop) ac090_run(c, until - c->clock);
}

static void words(uint8_t *m, uint32_t a, const uint16_t *w, int n) {
    for (int i = 0; i < n; i++) { m[a + 2 * i] = (uint8_t)(w[i] >> 8); m[a + 2 * i + 1] = (uint8_t)w[i]; }
}

/* ---- the disassembler -------------------------------------------------------- */

static const uint8_t *dis_mem;
static uint16_t dis_rd(void *ctx, uint32_t a) { (void)ctx; return (uint16_t)(dis_mem[a] << 8 | dis_mem[a + 1]); }

static void dis_case(int model, const char *hex, int len, const char *want) {
    uint8_t m[64];
    memset(m, 0, sizeof m);
    for (int i = 0; i < 64 - 4; i += 2) { m[i] = 0x4e; m[i + 1] = 0x71; }
    int n = 0;
    for (const char *p = hex; *p && n < 40;) {
        while (*p == ' ') p++;
        if (!*p) break;
        unsigned v;
        sscanf(p, "%4x", &v);
        m[n++] = (uint8_t)(v >> 8); m[n++] = (uint8_t)v;
        p += 4;
    }
    dis_mem = m;
    char out[160];
    int got = dis68k(0, dis_rd, NULL, model, out, sizeof out);
    char norm[160];   /* the mnemonic's padding as one space */
    size_t k = 0;
    for (const char *p = out; *p && k + 1 < sizeof norm; p++) {
        if (*p == ' ' && (k && norm[k - 1] == ' ')) continue;
        norm[k++] = *p;
    }
    norm[k] = 0;
    CHECK(got == len && !strcmp(norm, want), "%s: %d \"%s\", want %d \"%s\"", hex, got, norm, len, want);
}

static void disassembler(void) {
    static const struct { int model; const char *hex; int len; const char *want; } t[] = {
        { 2, "4e71", 2, "nop" },
        { 2, "7aff", 2, "moveq #-$1,d5" },
        { 2, "203c 1234 5678", 6, "move.l #$12345678,d0" },
        { 2, "41fa fffe", 4, "lea $00000000(pc),a0" },
        { 2, "4cdf 7fff", 4, "movem.l (a7)+,d0-d7/a0-a6" },
        { 2, "48e7 c0c0", 4, "movem.l d0-d1/a0-a1,-(a7)" },
        { 2, "6100 0010", 4, "bsr.w $00000012" },
        { 2, "66fa", 2, "bne.s $fffffffc" },
        { 2, "51c8 fffc", 4, "dbra d0,$fffffffe" },
        { 2, "e5a9", 2, "lsl.l d2,d1" },
        { 2, "c340", 2, "exg d1,d0" },
        { 2, "4e7a 0801", 4, "movec vbr,d0" },
        { 2, "2071 0130 0000 0010", 8, "movea.l ($10,a1,d0.w),a0" },
        { 2, "2031 2d30 0000 0010", 8, "move.l ($10,a1,d2.l*4),d0" },
        { 2, "e9c1 0805", 4, "bfextu d1{d0:5},d0" },
        { 2, "4c41 2001", 4, "divul.l d1,d1:d2" },
        { 2, "0ad0 0040", 4, "cas.b d0,d1,(a0)" },
        { 2, "f227 6800", 4, "fmove.x fp0,-(a7)" },
        { 2, "f239 4422 0000 1000", 8, "fadd.s $00001000,fp0" },
        { 2, "f200 0420", 4, "fdiv.x fp1,fp0" },
        { 2, "f281 0010", 4, "fbeq.w $00000012" },
        { 2, "f227 e0ff", 4, "fmovem.x fp0-fp7,-(a7)" },
        { 2, "f21f d0f0", 4, "fmovem.x (a7)+,fp0-fp3" },
        { 2, "f200 a800", 4, "fmove.l fpsr,d0" },
        { 2, "f200 5c0f", 4, "fmovecr.x #$f,fp0" },
        { 2, "f4f8", 2, "cpusha bc" },
        { 2, "f518", 2, "pflusha" },
        { 2, "f620 9000", 4, "move16 (a0)+,(a1)+" },
        { 2, "007c 0700", 4, "ori.w #$700,sr" },
        { 2, "f200 5400", 2, "dc.w $f200" },          /* fmove.d from a data register: no such thing */
        { 2, "2030 1808", 4, "move.l $8(a0,d1.l),d0" },
        { 0, "2030 0900", 4, "move.l $0(a0,d0.l),d0" },   /* the 68000 takes any extension word as a brief one */
        { 0, "4e7a 0801", 2, "dc.w $4e7a" },
        { 0, "49c0", 2, "dc.w $49c0" },
        { 0, "4e75", 2, "rts" },
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) dis_case(t[i].model, t[i].hex, t[i].len, t[i].want);
}

int main(void) {
    disassembler();

    static ac090 cpu;
    ac090 *c = &cpu;
    ac090_init(c, AC090_68040);
    c->addr_mask = 0xffffffffu;
    c->bus.read = io_read;
    c->bus.write = io_write;
    c->bus.rpage[0] = c->bus.wpage[0] = ram;       /* RAM at 0 */
    c->bus.rpage[1] = rom;                          /* ROM at $10000 */
    /* $20000 and up: hardware (the bus's functions) */
    static const uint16_t vec[] = { 0x0000, 0x8000, 0x0000, 0x1000 };
    words(ram, 0, vec, 4);
    static const uint16_t prog[] = {
        0x7000,          /* $1000 moveq #0,d0 */
        0x6106,          /* $1002 bsr.s $100a */
        0x5280,          /* $1004 addq.l #1,d0 */
        0x60fa,          /* $1006 bra.s $1002 */
        0x4e71,          /* $1008 nop */
        0x5481,          /* $100a addq.l #2,d1 */
        0x4e75,          /* $100c rts */
    };
    words(ram, 0x1000, prog, 7);
    ac090_reset(c);
    CHECK(c->pc == 0x1000, "reset to $1000 (%08x)", c->pc);

    /* a breakpoint stops before its instruction */
    CHECK(dbg_bp_toggle(c, 0x1004) == 1, "a breakpoint set");
    run(c, 100000);
    CHECK(dbg_stopped(c) && c->pc == 0x1004 && c->dbg_why == AC090_DBG_BP, "stopped at $1004 (pc %08x, why %d)", c->pc, c->dbg_why);
    CHECK(c->d[1] == 2 && c->d[0] == 0, "the call ran once, the addq not yet (d0 %u d1 %u)", c->d[0], c->d[1]);
    uint64_t clk = c->clock;
    run(c, 100000);
    CHECK(c->clock == clk && c->pc == 0x1004, "stopped: nothing runs");

    /* on from it, round the loop to it again */
    dbg_continue(c);
    run(c, 100000);
    CHECK(dbg_stopped(c) && c->pc == 0x1004 && c->d[0] == 1 && c->d[1] == 4, "round once more (pc %08x d0 %u d1 %u)", c->pc, c->d[0], c->d[1]);

    /* steps: one instruction each, the breakpoint at the start passed */
    dbg_step(c); run(c, 100000);
    CHECK(c->pc == 0x1006 && c->d[0] == 2 && c->dbg_why == AC090_DBG_STEP, "a step: the addq (pc %08x d0 %u)", c->pc, c->d[0]);
    dbg_step(c); run(c, 100000);
    CHECK(c->pc == 0x1002, "a step: the bra (pc %08x)", c->pc);
    dbg_step(c); run(c, 100000);
    CHECK(c->pc == 0x100a, "a step into the bsr (pc %08x)", c->pc);
    dbg_step(c); run(c, 100000);
    dbg_step(c); run(c, 100000);
    CHECK(c->pc == 0x1004 && c->d[1] == 6, "out of it again (pc %08x d1 %u)", c->pc, c->d[1]);
    dbg_step(c); run(c, 100000);
    dbg_step(c); run(c, 100000);
    CHECK(c->pc == 0x1002, "back at the bsr (pc %08x)", c->pc);

    /* step over the call: it runs whole, and the stop is after it */
    dbg_step_over(c); run(c, 100000);
    CHECK(c->pc == 0x1004 && c->d[1] == 8 && dbg_stopped(c), "stepped over the bsr (pc %08x d1 %u why %d)", c->pc, c->d[1], c->dbg_why);
    dbg_step_over(c); run(c, 100000);
    CHECK(c->pc == 0x1006, "step over a plain instruction is a step (pc %08x)", c->pc);

    /* run to an address */
    dbg_run_to(c, 0x100c); run(c, 100000);
    CHECK(c->pc == 0x100c && c->dbg_why == AC090_DBG_TEMP, "ran to $100c (pc %08x why %d)", c->pc, c->dbg_why);

    /* released, it runs freely; the breakpoint still stops it */
    CHECK(dbg_bp_toggle(c, 0x1004) == 0, "the breakpoint cleared");
    dbg_release(c);
    CHECK(!c->dbg_on, "no breakpoints: the fast way again");
    uint64_t ins = c->instructions;
    run(c, 100000);
    CHECK(!dbg_stopped(c) && c->instructions > ins + 1000, "runs freely (%llu instructions)", (unsigned long long)(c->instructions - ins));
    dbg_break(c);
    ins = c->instructions;
    run(c, 100000);
    CHECK(dbg_stopped(c) && c->instructions == ins, "a break holds it");
    dbg_bp_toggle(c, 0x100a);
    dbg_release(c);
    run(c, 100000);
    CHECK(dbg_stopped(c) && c->pc == 0x100a, "a breakpoint kept after the debugger closed (pc %08x)", c->pc);
    dbg_bp_clear(c);
    dbg_release(c);

    /* memory: RAM and ROM, never the hardware */
    uint8_t v = 0;
    CHECK(dbg_peek(c, 0x1001, &v) && v == 0x00, "peek RAM");
    rom[0x10] = 0x5a;
    CHECK(dbg_peek(c, 0x10010, &v) && v == 0x5a, "peek ROM");
    CHECK(!dbg_poke(c, 0x10010, 1) && rom[0x10] == 0x5a, "ROM cannot be poked");
    CHECK(dbg_poke(c, 0x2000, 0x42) && ram[0x2000] == 0x42, "poke RAM");
    char t[160];
    for (uint32_t a = 0x20000; a < 0x20010; a += 2) { dbg_peek(c, a, &v); dbg_disasm(c, a, t, sizeof t); }
    CHECK(io_reads == 0 && io_writes == 0, "the hardware was not touched (%d reads, %d writes)", io_reads, io_writes);
    CHECK(dbg_disasm(c, 0x1002, t, sizeof t) == 2 && strstr(t, "bsr.s") && strstr(t, "$0000100a"), "dbg_disasm: %s", t);
    CHECK(dbg_back(c, 0x1006, 2) == 0x1002, "two instructions back from $1006: $%08x", dbg_back(c, 0x1006, 2));

    /* through the 68040's MMU: logical $3000 is physical $5000 (4K pages), $4000 nothing */
    {
        static const uint16_t tables[] = { 0x0000, 0x6202 };     /* $6000: the root's first entry, a pointer table at $6200 */
        words(ram, 0x6000, tables, 2);
        static const uint16_t ptr[] = { 0x0000, 0x6402 };        /* $6200: a page table at $6400 */
        words(ram, 0x6200, ptr, 2);
        static const uint16_t pages[] = { 0x0000, 0x5001 };      /* $640C: page 3 at $5000, resident */
        words(ram, 0x640c, pages, 2);
        ram[0x5010] = 0x77; ram[0x3010] = 0x11;
        c->srp = c->urp = 0x6000; c->tc = 0x8000; c->mmu_on = 1;
        uint32_t pa;
        CHECK(ac090_mmu_peek(c, 0x3010, 0, &pa) == 0 && pa == 0x5010, "$3010 is at $%08x", pa);
        CHECK(dbg_peek(c, 0x3010, &v) && v == 0x77, "peek through the MMU (%02x)", v);
        CHECK(!dbg_peek(c, 0x4000, &v), "nothing mapped at $4000");
        CHECK(dbg_poke(c, 0x3011, 0x99) && ram[0x5011] == 0x99, "poke through it");
        c->dtt0 = 0x0000c000;                                    /* a transparent window over $00xxxxxx */
        CHECK(dbg_peek(c, 0x3010, &v) && v == 0x11, "a transparent window: as it is (%02x)", v);
        c->dtt0 = 0; c->tc = 0; c->mmu_on = 0;
    }

    /* registers and addresses by name */
    uint32_t r;
    dbg_set_reg(c, "d3", 0x1234);
    CHECK(dbg_get_reg(c, "D3", &r) == 0 && r == 0x1234, "d3");
    CHECK(dbg_parse(c, "$1000", &r) == 0 && r == 0x1000, "$1000");
    CHECK(dbg_parse(c, "fe000000", &r) == 0 && r == 0xfe000000u, "fe000000");
    CHECK(dbg_parse(c, "0x20", &r) == 0 && r == 0x20, "0x20");
    CHECK(dbg_parse(c, "d3+$10", &r) == 0 && r == 0x1244, "d3+$10 (%x)", r);
    CHECK(dbg_parse(c, " pc - 4 ", &r) == 0 && r == c->pc - 4, "pc - 4");
    CHECK(dbg_parse(c, "a0", &r) == 0 && r == c->a[0], "a0 is the register");
    CHECK(dbg_parse(c, "$a0", &r) == 0 && r == 0xa0, "$a0 is the number");
    CHECK(dbg_parse(c, "fred", &r) == -1 && dbg_parse(c, "", &r) == -1 && dbg_parse(c, "12 34", &r) == -1, "not addresses");
    uint32_t ssp = c->a[7];
    CHECK(dbg_get_reg(c, "ssp", &r) == 0 && r == ssp && c->s, "supervisor: a7 is the SSP");
    dbg_set_reg(c, "usp", 0x4000);
    dbg_set_reg(c, "sr", 0x0000);
    CHECK(!c->s && c->a[7] == 0x4000 && dbg_get_reg(c, "ssp", &r) == 0 && r == ssp, "to user mode: a7 the USP, the SSP kept (a7 %08x)", c->a[7]);
    dbg_set_reg(c, "sr", 0x2700);
    CHECK(c->s && c->a[7] == ssp, "back to supervisor");
    dbg_sr_text(0x2714, t, sizeof t);
    CHECK(!strcmp(t, "-- S - I7 X-Z--"), "SR as text: %s", t);

    printf(fails ? "debug_test: %d failed\n" : "debug_test: all passed\n", fails);
    return fails != 0;
}
