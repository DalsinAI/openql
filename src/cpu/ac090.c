/*
 * AC090 integer core in C. See ac090.h.
 *
 * Rules this file keeps, from the 29 September review:
 * - one core: there is no second fast path with its own idea of the flags;
 * - every exception is taken, with the frame the model uses;
 * - the core never reads operating-system memory to decide what to do;
 * - no allocation and no closure per instruction.
 */
#include "ac090.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

typedef void (*handler)(ac090 *c);
static handler table[65536];
static int table_ready;

/* ---- clocks ------------------------------------------------------------
 * 68020 timing, cache case: each opcode costs its Motorola cache-case clocks
 * (operation plus effective address, from clk020 below), assuming operands
 * on a zero-wait 32-bit bus. On top of that:
 *  - every access to a direct page adds the board's wait for that page
 *    (bus.wait), so chip RAM and ROM cost what the A1200 makes them cost;
 *  - an instruction fetch that misses the instruction cache adds one bus
 *    cycle (3 clocks) plus that wait; a hit adds nothing;
 *  - taken branches, DBcc expiry, MOVEM per register, exceptions and
 *    full-format extension words add in the handlers.
 * The figures come from the MC68020 User's Manual, cross-checked against
 * WinUAE's 68020/030 table (table68k, gencpu.cpp); no WinUAE code is used. */
static uint8_t clk020[65536];
/* Opcodes after which the interpreter's run of plain instructions
 * (run_loop) checks the CPU's state again: those that may change SR's
 * system byte, the MMU, the caches or STOP the CPU, and line A/F (the
 * FPU, MMU and cache instructions, and the emulator's own traps).
 * Exceptions, device accesses and interrupts set c->recheck instead. */
static uint8_t op_recheck[65536];
static int op_recheck_wants(uint32_t op) {
    if (op >= 0xa000 && op < 0xb000) return 1;                  /* line A */
    if (op >= 0xf000) return 1;                                  /* line F: FPU, MMU, CINV/CPUSH, MOVE16 */
    if ((op & 0xffc0) == 0x46c0) return 1;                       /* MOVE to SR */
    if (op == 0x007c || op == 0x027c || op == 0x0a7c) return 1;  /* ORI, ANDI, EORI to SR */
    if ((op & 0xfff0) == 0x4e40) return 1;                       /* TRAP */
    if (op >= 0x4e70 && op <= 0x4e77 && op != 0x4e71 && op != 0x4e75) return 1;   /* RESET, STOP, RTE, RTD, TRAPV, RTR */
    if (op == 0x4e7a || op == 0x4e7b) return 1;                  /* MOVEC */
    if ((op & 0xff00) == 0x0e00) return 1;                       /* MOVES */
    if ((op & 0xffc0) == 0x06c0) return 1;                       /* CALLM, RTM */
    if (op == 0x4afc || (op & 0xfff8) == 0x4848) return 1;       /* ILLEGAL, BKPT */
    return 0;
}

/* ---- memory ------------------------------------------------------------ */

static inline uint32_t mask(ac090 *c, uint32_t a) { return a & c->addr_mask; }
/* The board's wait for the page at a, into the clock and the waits so far:
 * nothing to add for a page without one (fast RAM), the common case. */
/* Something that may change what the interpreter's run checks (run_loop):
 * the run ends after this instruction, and its fetches go the general way
 * from now on (the page table or the waits may have changed). */
#define RECHECK(c) do { (c)->recheck = 1; (c)->run_page = NULL; } while (0)
#define BUS_WAIT(c, a) do { uint32_t w_ = (c)->bus.wait[(a) >> 16]; if (w_) { (c)->clock += w_; (c)->bus_waited += w_; } } while (0)

static void address_error(ac090 *c) {
    c->fault = 1;
    longjmp(c->abort, 1);
}

static void wlog_bytes(ac090 *c, uint32_t a, const uint8_t *p, int n) {
    for (int i = 0; i < n; i++) {
        if (c->wlog_n == c->wlog_cap) {
            c->wlog_cap = c->wlog_cap ? c->wlog_cap * 2 : 4096;
            c->wlog_addr = realloc(c->wlog_addr, c->wlog_cap * sizeof *c->wlog_addr);
            c->wlog_old = realloc(c->wlog_old, c->wlog_cap);
        }
        c->wlog_addr[c->wlog_n] = a + (uint32_t)i; c->wlog_old[c->wlog_n] = p[i]; c->wlog_n++;
    }
}
/* A hardware access while probing: stop before it has any effect. */
static void probe_abort(ac090 *c) { c->probe_hit = 1; longjmp(c->abort, 4); }
/* Probing: may this hardware access go ahead (no lasting side effect)? */
static int probe_io_ok(ac090 *c, uint32_t a, int write) {
    if (c->probe_io) return c->probe_io(c->bus.opaque, a, 0, write);
    if (a < c->probe_io_lo || a >= c->probe_io_hi) return 0;
    if (write) for (int i = 0; i < 4; i++) if (c->probe_io_deny[i] && (a & ~3u) == c->probe_io_deny[i]) return 0;
    return 1;
}

static inline uint32_t prd8(ac090 *c, uint32_t addr) {
    uint32_t a = mask(c, addr);
    uint8_t *p = c->bus.rpage[a >> 16];
    BUS_WAIT(c, a);
    if (p) return p[a & 0xffff];
    if (c->probe && !probe_io_ok(c, a, 0)) probe_abort(c);
    RECHECK(c); return c->bus.read(c->bus.opaque, a, 1) & 0xff;
}

static inline uint32_t prd16(ac090 *c, uint32_t addr) {
    uint32_t a = mask(c, addr);
    if ((a & 1) && c->model == AC090_68000) address_error(c);
    uint8_t *p = c->bus.rpage[a >> 16];
    BUS_WAIT(c, a);
    if (p && (a & 0xffff) != 0xffff) {
        p += a & 0xffff;
        return (uint32_t)(p[0] << 8 | p[1]);
    }
    if (!p) { if (c->probe && !probe_io_ok(c, a, 0)) probe_abort(c); RECHECK(c); return c->bus.read(c->bus.opaque, a, 2) & 0xffff; }
    return (prd8(c, a) << 8) | prd8(c, a + 1);
}

static inline uint32_t prd32(ac090 *c, uint32_t addr) {
    uint32_t a = mask(c, addr);
    if ((a & 1) && c->model == AC090_68000) address_error(c);
    uint8_t *p = c->bus.rpage[a >> 16];
    if (p && (a & 0xffff) <= 0xfffc) {
        BUS_WAIT(c, a);
        p += a & 0xffff;
        return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
    }
    if (!p && (a & 0xffff) <= 0xfffc) { if (c->probe && !probe_io_ok(c, a, 0)) probe_abort(c); BUS_WAIT(c, a); RECHECK(c); return c->bus.read(c->bus.opaque, a, 4); }   /* I/O: the board sees the long */
    return (prd16(c, a) << 16) | prd16(c, a + 2);
}

static inline void pwr8(ac090 *c, uint32_t addr, uint32_t v) {
    uint32_t a = mask(c, addr);
    uint8_t *p = c->bus.wpage[a >> 16];
    BUS_WAIT(c, a);
    if (p) { if (c->wlog_on) wlog_bytes(c, a, p + (a & 0xffff), 1); p[a & 0xffff] = (uint8_t)v; return; }
    if (c->bus.rpage[a >> 16] && !c->bus.rom_writes) return; /* ROM */
    if (c->probe && !probe_io_ok(c, a, 1)) probe_abort(c);
    RECHECK(c); c->bus.write(c->bus.opaque, a, v & 0xff, 1);
}

static inline void pwr16(ac090 *c, uint32_t addr, uint32_t v) {
    uint32_t a = mask(c, addr);
    if ((a & 1) && c->model == AC090_68000) address_error(c);
    uint8_t *p = c->bus.wpage[a >> 16];
    BUS_WAIT(c, a);
    if (p && (a & 0xffff) != 0xffff) {
        p += a & 0xffff;
        if (c->wlog_on) wlog_bytes(c, a, p, 2);
        p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
        return;
    }
    if (!p && !c->bus.rpage[a >> 16]) { if (c->probe && !probe_io_ok(c, a, 1)) probe_abort(c); RECHECK(c); c->bus.write(c->bus.opaque, a, v & 0xffff, 2); return; }
    pwr8(c, a, v >> 8); pwr8(c, a + 1, v);
}

static inline void pwr32(ac090 *c, uint32_t addr, uint32_t v) {
    uint32_t a = mask(c, addr);
    if ((a & 1) && c->model == AC090_68000) address_error(c);
    uint8_t *p = c->bus.wpage[a >> 16];
    if (p && (a & 0xffff) <= 0xfffc) {
        BUS_WAIT(c, a);
        p += a & 0xffff;
        if (c->wlog_on) wlog_bytes(c, a, p, 4);
        p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
        return;
    }
    if (!p && !c->bus.rpage[a >> 16] && (a & 0xffff) <= 0xfffc) { if (c->probe && !probe_io_ok(c, a, 1)) probe_abort(c); }
    if (!p && !c->bus.rpage[a >> 16] && (a & 0xffff) <= 0xfffc) { BUS_WAIT(c, a); RECHECK(c); c->bus.write(c->bus.opaque, a, v, 4); return; }   /* I/O: the board sees the long */
    pwr16(c, a, v >> 16); pwr16(c, a + 2, v);
}

/* ---- the 68040 MMU in front of memory -----------------------------------
 * With translation on (TC E), every access the core makes goes through the
 * MMU (ac090_mmu.c): data with the current mode's function code, instruction
 * fetches as program space. The accessors above are physical. An access that
 * crosses a page is split, so each part is translated. */
static uint32_t mmu_xlate(ac090 *c, uint32_t a, int write, int fc, int size);
static inline int fc_data(const ac090 *c) { return c->mmu_fc ? c->mmu_fc : c->s ? 5 : 1; }
static inline uint32_t rd8(ac090 *c, uint32_t a) {
    if (c->mmu_on) a = mmu_xlate(c, a, 0, fc_data(c), 1);
    return prd8(c, a);
}
static inline uint32_t rd16(ac090 *c, uint32_t a) {
    if (c->mmu_on) {
        if ((a & 0xfff) == 0xfff) return rd8(c, a) << 8 | rd8(c, a + 1);
        a = mmu_xlate(c, a, 0, fc_data(c), 2);
    }
    return prd16(c, a);
}
static inline uint32_t rd32(ac090 *c, uint32_t a) {
    if (c->mmu_on) {
        if ((a & 0xfff) > 0xffc) return rd16(c, a) << 16 | rd16(c, a + 2);
        a = mmu_xlate(c, a, 0, fc_data(c), 4);
    }
    return prd32(c, a);
}
static inline void wr8(ac090 *c, uint32_t a, uint32_t v) {
    if (c->mmu_on) a = mmu_xlate(c, a, 1, fc_data(c), 1);
    pwr8(c, a, v);
}
static inline void wr16(ac090 *c, uint32_t a, uint32_t v) {
    if (c->mmu_on) {
        if ((a & 0xfff) == 0xfff) { wr8(c, a, v >> 8); wr8(c, a + 1, v); return; }
        a = mmu_xlate(c, a, 1, fc_data(c), 2);
    }
    pwr16(c, a, v);
}
static inline void wr32(ac090 *c, uint32_t a, uint32_t v) {
    if (c->mmu_on) {
        if ((a & 0xfff) > 0xffc) { wr16(c, a, v >> 16); wr16(c, a + 2, v); return; }
        a = mmu_xlate(c, a, 1, fc_data(c), 4);
    }
    pwr32(c, a, v);
}
/* An instruction word: program space. */
static inline uint32_t rd16_prog(ac090 *c, uint32_t a) {
    if (c->mmu_on) a = mmu_xlate(c, a, 0, c->s ? 6 : 2, 2);
    return prd16(c, a);
}

/* Set the PC for a jump, branch or return. A 68000 faults on an odd PC
 * here, before the next fetch; the 68020 has no address error. */
static void address_error_020(ac090 *c, uint32_t pc);
static inline void jump(ac090 *c, uint32_t pc) {
    if (pc & 1) {
        if (c->model == AC090_68000) address_error(c);
        address_error_020(c, pc);
        return;
    }
    c->pc = pc;
    c->flow = 1;
}

/* Instruction fetch. With the 68020 cache enabled (CACR E), a fetch is
 * served from the cache when the entry is valid and its tag matches; a miss
 * reads the whole longword and fills the entry unless the cache is frozen
 * (CACR F). A hit costs no bus cycle. */
static uint32_t fetch_cached(ac090 *c, uint32_t pc) {
    uint32_t a = mask(c, pc);
    unsigned idx = a >> 2 & 63;
    uint32_t tag = (a >> 8) | (c->s << 24);
    if (c->icache_valid[idx] && c->icache_tag[idx] == tag) {
        c->icache_hits++;
        return (a & 2) ? (c->icache_data[idx] & 0xffff) : (c->icache_data[idx] >> 16);
    }
    c->icache_misses++;
    c->clock += 3;
    uint32_t l = rd32(c, a & ~3u);
    if (!(c->cacr & 2)) { c->icache_valid[idx] = 1; c->icache_tag[idx] = tag; c->icache_data[idx] = l; }
    return (a & 2) ? (l & 0xffff) : (l >> 16);
}

static inline uint32_t fetch16(ac090 *c) {
    uint32_t v;
    if (c->run_page) {                                    /* in a plain run (run_loop): no prefetch, cache model or MMU; its code page */
        uint32_t off = (c->pc & c->addr_mask) - c->run_base;
        if (__builtin_expect(off <= 0xfffe && !(off & 1), 1)) {
            const uint8_t *p = c->run_page + off;
            v = (uint32_t)p[0] << 8 | p[1];
            c->clock += 3u + c->run_w;                    /* as below: the wait (prd16), then the bus cycle */
            if (c->run_w) c->bus_waited += c->run_w;
            c->pc += 2;
            return v;
        }
    }
    if (__builtin_expect(c->pf_len != 0, 0)) {           /* after RESET: what the pipeline held */
        if (c->pc - c->pf_base < (uint32_t)c->pf_len * 2) { v = c->pf_word[(c->pc - c->pf_base) >> 1]; c->pc += 2; return v; }
        c->pf_len = 0;
    }
    if ((c->cacr & 1) && !c->jit_on) v = fetch_cached(c, c->pc);   /* under the JIT the translations are the cache */
    else { v = rd16_prog(c, c->pc); c->clock += 3; }
    c->pc += 2;
    return v;
}
static inline uint32_t fetch32(ac090 *c) {
    uint32_t hi = fetch16(c);
    return hi << 16 | fetch16(c);
}

/* ---- status register ---------------------------------------------------- */

uint16_t ac090_get_sr(const ac090 *c) {
    return (uint16_t)(c->t1 << 15 | c->t0 << 14 | c->s << 13 | c->m << 12 | c->ipl << 8 |
                      c->x << 4 | c->n << 3 | c->z << 2 | c->v << 1 | c->c);
}

/* Keep A7 in step with S and M: the inactive stack pointers live in
 * usp/isp/msp, the active one in a[7]. */
static void save_sp(ac090 *c) {
    if (!c->s) c->usp = c->a[7];
    else if (c->m && c->model >= AC090_68020) c->msp = c->a[7];
    else c->isp = c->a[7];
}
static void load_sp(ac090 *c) {
    if (!c->s) c->a[7] = c->usp;
    else if (c->m && c->model >= AC090_68020) c->a[7] = c->msp;
    else c->a[7] = c->isp;
    /* Adaptive stacks: the supervisor stack is never checked; back in user
     * mode (another task, perhaps) the first block that pushes asks again. */
    if (c->stack_hook) { if (c->s) { c->stk_lo = 0; c->stk_span = 0xffffffffu; } else c->stk_span = 0; }
}

void ac090_set_sr(ac090 *c, uint16_t sr) {
    c->flow = 1;
    save_sp(c);
    c->t1 = sr >> 15 & 1;
    c->t0 = c->model >= AC090_68020 ? sr >> 14 & 1 : 0;
    c->s = sr >> 13 & 1;
    c->m = c->model >= AC090_68020 ? sr >> 12 & 1 : 0;
    c->ipl = sr >> 8 & 7;
    c->x = sr >> 4 & 1; c->n = sr >> 3 & 1; c->z = sr >> 2 & 1; c->v = sr >> 1 & 1; c->c = sr & 1;
    load_sp(c);
}

static inline uint32_t ccr(ac090 *c) { return c->x << 4 | c->n << 3 | c->z << 2 | c->v << 1 | c->c; }
static inline void set_ccr(ac090 *c, uint32_t v) {
    c->x = v >> 4 & 1; c->n = v >> 3 & 1; c->z = v >> 2 & 1; c->v = v >> 1 & 1; c->c = v & 1;
}

/* ---- exceptions ---------------------------------------------------------- */

enum {
    V_BUS = 2, V_ADDRESS = 3, V_ILLEGAL = 4, V_ZERO = 5, V_CHK = 6, V_TRAPV = 7,
    V_PRIV = 8, V_TRACE = 9, V_LINEA = 10, V_LINEF = 11, V_FORMAT = 14,
    V_SPURIOUS = 24, V_AUTO = 24, V_TRAP = 32,
};

/* Take an exception. pc is the return address the frame holds. For the
 * 68020 frames of vectors 5, 6, 7 and 9 also hold the instruction address. */
#ifdef AC090_TRACE
#include <stdio.h>
static void trace_dump(ac090 *c, int vector) {
    fprintf(stderr, "vector %d at %08x (op %04x), a7 %08x sr %04x; the last 80 instructions:\n", vector, c->op_pc, c->op, c->a[7], ac090_get_sr(c));
    for (unsigned n = 24; n >= 1; n--) {
        unsigned k = (c->trace_pos - n) & 8191;
        fprintf(stderr, " %08x:%04x a7=%08x\n", c->trace_pc[k], c->trace_op[k], c->trace_a7[k]);
    }
    for (unsigned n = 4; n >= 2; n--) {
        uint32_t a = c->trace_pc[(c->trace_pos - n) & 8191];
        fprintf(stderr, " words at %08x:", a);
        for (int w = 0; w < 4; w++) fprintf(stderr, " %04x", rd16(c, a + 2u * (uint32_t)w));
        fprintf(stderr, "\n");
    }
    fprintf(stderr, " isp %08x msp %08x usp %08x m %u vbr %08x cacr %08x\n", c->isp, c->msp, c->usp, c->m, c->vbr, c->cacr);
    fprintf(stderr, "the last 40 control transfers (from -> to, op):\n");
    for (unsigned n = 40; n >= 1; n--) {
        unsigned k = (c->jump_pos - n) & 1023;
        fprintf(stderr, "  %08x -> %08x  %04x\n", c->jump_from[k], c->jump_to[k], c->jump_op[k]);
    }
    fprintf(stderr, "instructions before the run of zero words:\n");
    unsigned end = c->trace_pos, i = end - 1;
    while (i > end - 8192 && i > 0 && c->trace_op[(i - 1) & 8191] == 0x0000) i--;   /* skip the slide */
    unsigned from = i > 40 ? i - 40 : 0;
    for (unsigned k = from; k < i + 2 && k < end; k++) {
        fprintf(stderr, " %06x:%04x", c->trace_pc[k & 8191], c->trace_op[k & 8191]);
        if (((k - from) & 7) == 7) fprintf(stderr, "\n");
    }
    fprintf(stderr, "\n slide of %u zero words\n", end - i);
    fprintf(stderr, "last control transfers from ROM or into chip RAM below 0x30000 (from -> to, op):\n");
    int shown = 0;
    for (unsigned n = 1; n <= 1024 && shown < 12; n++) {
        unsigned k = (c->jump_pos - n) & 1023;
        uint32_t f = c->jump_from[k], to = c->jump_to[k];
        if (f >= 0xe00000 && to < 0x30000) {
            fprintf(stderr, "  %06x -> %06x  op %04x  (%u transfers back)\n", f, to, c->jump_op[k], n);
            shown++;
        }
    }
}
#endif

static void exception(ac090 *c, int vector, uint32_t pc) {
    RECHECK(c);                          /* the interpreter's loop: SR and the hooks' doings */
    if (c->exc_hook && c->exc_hook(c, vector, pc)) return;   /* a task core: the instruction runs again on CPU0 */
    c->excepted = 1;
    if (c->fault_hook && ((vector >= 2 && vector <= 4) || vector == 10 || vector == 11)) c->fault_hook(c, vector, pc);
    if (c->irq_hook && vector >= 24 && vector < 32) c->irq_hook(c, vector - 24, pc);
#ifdef AC090_TRACE
    if (vector == c->trace_vector) trace_dump(c, vector);
#endif
    /* The task guard after the hooks that only look, so they still see a crash it holds. */
    if (c->guard_hook && (vector == V_ILLEGAL || vector == V_LINEA) && c->guard_hook(c, vector, pc)) return;
    uint16_t old = ac090_get_sr(c);
    save_sp(c);
    c->s = 1; c->t1 = 0; c->t0 = 0;
    load_sp(c);
    if (c->model >= AC090_68020) {
        if (vector == V_ZERO || vector == V_CHK || vector == V_TRAPV || vector == V_TRACE) {
            c->a[7] -= 4; wr32(c, c->a[7], c->op_pc);
            c->a[7] -= 2; wr16(c, c->a[7], 0x2000 | (vector * 4));
        } else {
            c->a[7] -= 2; wr16(c, c->a[7], vector * 4);
        }
    }
    c->a[7] -= 4; wr32(c, c->a[7], pc);
    c->a[7] -= 2; wr16(c, c->a[7], old);
    c->pc = rd32(c, c->vbr + vector * 4);
    c->stopped = 0;
    c->clock += 20;
}

/* 68020 address error: an instruction fetch from an odd address. The frame
 * is the long bus cycle fault frame (format B, 92 bytes). The documented
 * fields are filled: SR, PC, format/vector, SSW (read, word, program space
 * FC 6 or 2) and the fault address; the internal words are zero. */
static void address_error_020(ac090 *c, uint32_t pc) {
    RECHECK(c);
    if (c->guard_hook && c->guard_hook(c, V_ADDRESS, pc)) { c->excepted = 1; return; }   /* pc: the odd address */
    uint16_t old = ac090_get_sr(c);
    c->excepted = 1;
    save_sp(c);
    c->s = 1; c->t1 = 0; c->t0 = 0;
    load_sp(c);
    if (c->model == AC090_68040) {               /* 68040: format 2, the instruction's PC and the odd address */
        c->a[7] -= 12;
        wr16(c, c->a[7], old);
        wr32(c, c->a[7] + 2, c->op_pc);
        wr16(c, c->a[7] + 6, 0x2000 | V_ADDRESS * 4);
        wr32(c, c->a[7] + 8, pc);
        c->pc = rd32(c, c->vbr + V_ADDRESS * 4);
        c->stopped = 0;
        c->clock += 40;
        return;
    }
    c->a[7] -= 92;
    uint32_t f = c->a[7];
    for (int i = 0; i < 92; i += 4) wr32(c, f + i, 0);
    wr16(c, f + 0, old);
    wr32(c, f + 2, pc);
    wr16(c, f + 6, 0xb000 | V_ADDRESS * 4);
    wr16(c, f + 10, 0x0040 | 0x0020 | (old & 0x2000 ? 6 : 2)); /* RW read, SIZE word, FC */
    wr32(c, f + 16, pc);
    c->pc = rd32(c, c->vbr + V_ADDRESS * 4);
    c->stopped = 0;
    c->clock += 40;
}

#include "ac090_mmu.c"

#define TRAP_AT_INSTRUCTION(c, vec) do { exception((c), (vec), (c)->op_pc); return; } while (0)

static void privilege(ac090 *c) { exception(c, V_PRIV, c->op_pc); }
static void illegal(ac090 *c) { exception(c, V_ILLEGAL, c->op_pc); }

/* ---- effective addresses -------------------------------------------------- */

enum { EA_D, EA_A, EA_MEM, EA_IMM };
typedef struct { int kind; uint32_t addr; } ea_t;

static inline uint32_t size_mask(int sz) { return sz == 1 ? 0xff : sz == 2 ? 0xffff : 0xffffffffu; }
static inline uint32_t sign_bit(int sz) { return sz == 1 ? 0x80 : sz == 2 ? 0x8000 : 0x80000000u; }
static inline int32_t sext(uint32_t v, int sz) {
    return sz == 1 ? (int8_t)v : sz == 2 ? (int16_t)v : (int32_t)v;
}

/* Brief and (68020) full extension words. base is the value of An or of
 * the PC at the extension word. */
static uint32_t indexed(ac090 *c, uint32_t base) {
    uint32_t ext = fetch16(c);
    uint32_t xn = ext & 0x8000 ? c->a[ext >> 12 & 7] : c->d[ext >> 12 & 7];
    if (!(ext & 0x800)) xn = (uint32_t)(int16_t)xn;
    if (c->model >= AC090_68020) xn <<= ext >> 9 & 3;
    if (c->model < AC090_68020 || !(ext & 0x100)) return base + (uint32_t)(int8_t)ext + xn;
    /* full format */
    c->clock += 4;
    int bs = ext >> 7 & 1, is = ext >> 6 & 1, bd_size = ext >> 4 & 3, iis = ext & 7;
    uint32_t bd = 0, od = 0;
    if (bd_size == 2) bd = (uint32_t)(int16_t)fetch16(c);
    else if (bd_size == 3) bd = fetch32(c);
    if (bs) base = 0;
    if (is) xn = 0;
    if (iis == 0) return base + bd + xn;
    int od_size = iis & 3;
    uint32_t addr;
    if (iis & 4) addr = rd32(c, base + bd) + xn;     /* post-indexed */
    else addr = rd32(c, base + bd + xn);              /* pre-indexed */
    if (od_size == 2) od = (uint32_t)(int16_t)fetch16(c);
    else if (od_size == 3) od = fetch32(c);
    return addr + od;
}

/* Decode an effective address, applying (An)+ and -(An). sz is 1, 2 or 4. */
static ea_t ea(ac090 *c, int mode, int reg, int sz) {
    ea_t e = { EA_MEM, 0 };
    switch (mode) {
    case 0: e.kind = EA_D; e.addr = reg; return e;
    case 1: e.kind = EA_A; e.addr = reg; return e;
    case 2: e.addr = c->a[reg]; return e;
    case 3: e.addr = c->a[reg]; c->a[reg] += (sz == 1 && reg == 7) ? 2 : sz; return e;
    case 4: c->a[reg] -= (sz == 1 && reg == 7) ? 2 : sz; e.addr = c->a[reg]; return e;
    case 5: e.addr = c->a[reg] + (uint32_t)(int16_t)fetch16(c); return e;
    case 6: e.addr = indexed(c, c->a[reg]); return e;
    }
    switch (reg) {
    case 0: e.addr = (uint32_t)(int16_t)fetch16(c); return e;
    case 1: e.addr = fetch32(c); return e;
    case 2: { uint32_t b = c->pc; e.addr = b + (uint32_t)(int16_t)fetch16(c); return e; }
    case 3: { uint32_t b = c->pc; e.addr = indexed(c, b); return e; }
    case 4:
        e.kind = EA_IMM;
        if (sz == 4) e.addr = fetch32(c);
        else e.addr = fetch16(c) & size_mask(sz);
        return e;
    }
    illegal(c);
    longjmp(c->abort, 2);
}

static inline uint32_t ea_read(ac090 *c, ea_t *e, int sz) {
    switch (e->kind) {
    case EA_D: return c->d[e->addr] & size_mask(sz);
    case EA_A: return c->a[e->addr] & size_mask(sz);
    case EA_IMM: return e->addr;
    }
    return sz == 1 ? rd8(c, e->addr) : sz == 2 ? rd16(c, e->addr) : rd32(c, e->addr);
}

static inline void ea_write(ac090 *c, ea_t *e, int sz, uint32_t v) {
    switch (e->kind) {
    case EA_D: {
        uint32_t m = size_mask(sz);
        c->d[e->addr] = (c->d[e->addr] & ~m) | (v & m);
        return;
    }
    case EA_A: c->a[e->addr] = v; return;
    case EA_IMM: return;
    }
    if (sz == 1) wr8(c, e->addr, v); else if (sz == 2) wr16(c, e->addr, v); else wr32(c, e->addr, v);
}

/* ---- flag helpers -------------------------------------------------------- */

static inline void nz(ac090 *c, uint32_t r, int sz) {
    r &= size_mask(sz);
    c->n = (r & sign_bit(sz)) != 0;
    c->z = r == 0;
}
static inline void logic_flags(ac090 *c, uint32_t r, int sz) { nz(c, r, sz); c->v = 0; c->c = 0; }

static inline uint32_t do_add(ac090 *c, uint32_t s, uint32_t d, uint32_t x, int sz, int sticky) {
    uint32_t m = size_mask(sz), sb = sign_bit(sz);
    s &= m; d &= m;
    uint64_t r64 = (uint64_t)s + d + x;
    uint32_t r = (uint32_t)r64 & m;
    c->c = c->x = (r64 >> (sz * 8)) & 1;
    c->v = ((~(s ^ d) & (s ^ r)) & sb) != 0;
    c->n = (r & sb) != 0;
    if (sticky) { if (r) c->z = 0; } else c->z = r == 0;
    return r;
}

static inline uint32_t do_sub(ac090 *c, uint32_t s, uint32_t d, uint32_t x, int sz, int sticky, int setx) {
    uint32_t m = size_mask(sz), sb = sign_bit(sz);
    s &= m; d &= m;
    uint64_t r64 = (uint64_t)d - s - x;
    uint32_t r = (uint32_t)r64 & m;
    c->c = (r64 >> (sz * 8)) & 1;
    if (setx) c->x = c->c;
    c->v = (((s ^ d) & (r ^ d)) & sb) != 0;
    c->n = (r & sb) != 0;
    if (sticky) { if (r) c->z = 0; } else c->z = r == 0;
    return r;
}

static inline int cond(ac090 *c, int cc) {
    switch (cc) {
    case 0: return 1;
    case 1: return 0;
    case 2: return !c->c && !c->z;
    case 3: return c->c || c->z;
    case 4: return !c->c;
    case 5: return c->c;
    case 6: return !c->z;
    case 7: return c->z;
    case 8: return !c->v;
    case 9: return c->v;
    case 10: return !c->n;
    case 11: return c->n;
    case 12: return c->n == c->v;
    case 13: return c->n != c->v;
    case 14: return !c->z && c->n == c->v;
    default: return c->z || c->n != c->v;
    }
}

/* ---- opcode fields --------------------------------------------------------- */
#define OP (c->op)
#define MODE (OP >> 3 & 7)
#define REG (OP & 7)
#define RX (OP >> 9 & 7)
static const int size_of[4] = { 1, 2, 4, 0 };
#define SZ size_of[OP >> 6 & 3]

/* ---- group 0: immediates, bit ops, MOVEP ---------------------------------- */

static void op_imm(ac090 *c) { /* ORI ANDI SUBI ADDI EORI CMPI */
    int kind = OP >> 9 & 7, sz = SZ;
    uint32_t imm = sz == 4 ? fetch32(c) : fetch16(c) & size_mask(sz);
    if (MODE == 7 && REG == 4) { /* to CCR / SR */
        uint32_t v;
        if (sz == 1) {
            v = ccr(c);
            if (kind == 0) v |= imm; else if (kind == 1) v &= imm; else v ^= imm;
            set_ccr(c, v);
        } else {
            if (!c->s) { privilege(c); return; }
            v = ac090_get_sr(c);
            if (kind == 0) v |= imm; else if (kind == 1) v &= imm; else v ^= imm;
            ac090_set_sr(c, (uint16_t)v);
        }
        return;
    }
    ea_t e = ea(c, MODE, REG, sz);
    uint32_t d = ea_read(c, &e, sz), r;
    switch (kind) {
    case 0: r = d | imm; logic_flags(c, r, sz); ea_write(c, &e, sz, r); break;
    case 1: r = d & imm; logic_flags(c, r, sz); ea_write(c, &e, sz, r); break;
    case 2: r = do_sub(c, imm, d, 0, sz, 0, 1); ea_write(c, &e, sz, r); break;
    case 3: r = do_add(c, imm, d, 0, sz, 0); ea_write(c, &e, sz, r); break;
    case 5: r = d ^ imm; logic_flags(c, r, sz); ea_write(c, &e, sz, r); break;
    case 6: { uint32_t x = c->x; do_sub(c, imm, d, 0, sz, 0, 0); c->x = x; break; }
    default: illegal(c);
    }
}

static void op_bit(ac090 *c) { /* BTST BCHG BCLR BSET, dynamic (bit 8) or static */
    int kind = OP >> 6 & 3;
    uint32_t bit = (OP & 0x100) ? c->d[RX] : fetch16(c);
    if (MODE == 0) {
        bit &= 31;
        uint32_t d = c->d[REG];
        c->z = !(d >> bit & 1);
        if (kind == 1) d ^= 1u << bit; else if (kind == 2) d &= ~(1u << bit); else if (kind == 3) d |= 1u << bit;
        c->d[REG] = d;
        return;
    }
    bit &= 7;
    ea_t e = ea(c, MODE, REG, 1);
    uint32_t d = ea_read(c, &e, 1);
    c->z = !(d >> bit & 1);
    if (kind == 0) return;
    if (kind == 1) d ^= 1u << bit; else if (kind == 2) d &= ~(1u << bit); else d |= 1u << bit;
    ea_write(c, &e, 1, d);
}

static void op_movep(ac090 *c) {
    uint32_t addr = c->a[REG] + (uint32_t)(int16_t)fetch16(c);
    int dir = OP >> 7 & 1, lng = OP >> 6 & 1;
    int n = lng ? 4 : 2;
    if (dir) {
        uint32_t v = c->d[RX];
        for (int i = n - 1; i >= 0; i--) { wr8(c, addr, v >> (i * 8)); addr += 2; }
    } else {
        uint32_t v = 0;
        for (int i = 0; i < n; i++) { v = v << 8 | rd8(c, addr); addr += 2; }
        if (lng) c->d[RX] = v; else c->d[RX] = (c->d[RX] & 0xffff0000u) | v;
    }
}

/* ---- MOVE, MOVEA -------------------------------------------------------- */

static void op_move(ac090 *c) {
    static const int msz[4] = { 0, 1, 4, 2 };
    int sz = msz[OP >> 12 & 3];
    ea_t s = ea(c, MODE, REG, sz);
    uint32_t v = ea_read(c, &s, sz);
    int dmode = OP >> 6 & 7, dreg = RX;
    if (dmode == 1) { c->a[dreg] = sz == 2 ? (uint32_t)(int16_t)v : v; return; }
    logic_flags(c, v, sz);
    ea_t d = ea(c, dmode, dreg, sz);
    ea_write(c, &d, sz, v);
}

/* ---- group 4: miscellaneous ----------------------------------------------- */

static void op_negx_clr_neg_not(ac090 *c) {
    int kind = OP >> 9 & 3, sz = SZ;
    ea_t e = ea(c, MODE, REG, sz);
    /* CLR on the 68000 reads before it writes; the 68020 does not. */
    uint32_t d = (kind == 1 && c->model >= AC090_68020) ? 0 : ea_read(c, &e, sz), r;
    switch (kind) {
    case 0: r = do_sub(c, d, 0, c->x, sz, 1, 1); break;
    case 1: r = 0; c->n = 0; c->z = 1; c->v = 0; c->c = 0; break;
    case 2: r = do_sub(c, d, 0, 0, sz, 0, 1); break;
    default: r = ~d; logic_flags(c, r, sz); break;
    }
    ea_write(c, &e, sz, r);
}

static void op_move_from_sr(ac090 *c) {
    if (c->model >= AC090_68020 && !c->s) { privilege(c); return; }
    ea_t e = ea(c, MODE, REG, 2);
    if (c->model == AC090_68000 && e.kind == EA_MEM) ea_read(c, &e, 2);
    ea_write(c, &e, 2, ac090_get_sr(c));
}
static void op_move_from_ccr(ac090 *c) { ea_t e = ea(c, MODE, REG, 2); ea_write(c, &e, 2, ccr(c)); }
static void op_move_to_ccr(ac090 *c) { ea_t e = ea(c, MODE, REG, 2); set_ccr(c, ea_read(c, &e, 2)); }
static void op_move_to_sr(ac090 *c) {
    if (!c->s) { privilege(c); return; }
    ea_t e = ea(c, MODE, REG, 2);
    ac090_set_sr(c, (uint16_t)ea_read(c, &e, 2));
}

static void op_nbcd(ac090 *c);
static void op_swap(ac090 *c) {
    uint32_t v = c->d[REG];
    v = v >> 16 | v << 16;
    c->d[REG] = v;
    logic_flags(c, v, 4);
}

static void op_pea(ac090 *c) {
    ea_t e = ea(c, MODE, REG, 4);
    c->a[7] -= 4; wr32(c, c->a[7], e.addr);
}

static void op_ext(ac090 *c) {
    int kind = OP >> 6 & 7;
    uint32_t v = c->d[REG];
    if (kind == 2) { v = (v & 0xffff0000u) | ((uint32_t)(int8_t)v & 0xffff); c->d[REG] = v; logic_flags(c, v, 2); }
    else if (kind == 3) { v = (uint32_t)(int16_t)v; c->d[REG] = v; logic_flags(c, v, 4); }
    else { v = (uint32_t)(int8_t)v; c->d[REG] = v; logic_flags(c, v, 4); } /* EXTB.L, 68020 */
}

static void op_movem(ac090 *c) {
    uint32_t list = fetch16(c);
    c->clock += 4u * (uint32_t)__builtin_popcount(list);
    int sz = OP & 0x40 ? 4 : 2, to_regs = OP >> 10 & 1, mode = MODE, reg = REG;
    if (mode == 4 && !to_regs) {
        uint32_t addr = c->a[reg];
        for (int i = 15; i >= 0; i--) {
            if (!(list & (1u << (15 - i)))) continue;
            uint32_t v = i < 8 ? c->d[i] : c->a[i - 8];
            addr -= sz;
            /* An in its own list: the 68000 stores the initial value, the 68020
             * the initial value less one operand. */
            if (i - 8 == reg && c->model >= AC090_68020) v = c->a[reg] - sz;
            if (sz == 4) wr32(c, addr, v); else wr16(c, addr, v);
        }
        c->a[reg] = addr;
        return;
    }
    uint32_t addr;
    if (mode == 3) addr = c->a[reg];
    else { ea_t e = ea(c, mode, reg, sz); addr = e.addr; }
    for (int i = 0; i < 16; i++) {
        if (!(list & (1u << i))) continue;
        if (to_regs) {
            uint32_t v = sz == 4 ? rd32(c, addr) : (uint32_t)(int16_t)rd16(c, addr);
            if (i < 8) c->d[i] = v; else c->a[i - 8] = v;
        } else {
            uint32_t v = i < 8 ? c->d[i] : c->a[i - 8];
            if (sz == 4) wr32(c, addr, v); else wr16(c, addr, v);
        }
        addr += sz;
    }
    if (to_regs && c->model == AC090_68000) rd16(c, addr); /* the 68000 reads one word past the list */
    if (mode == 3) c->a[reg] = addr;
}

static void op_tst(ac090 *c) { int sz = SZ; ea_t e = ea(c, MODE, REG, sz); logic_flags(c, ea_read(c, &e, sz), sz); }

/* ---- task cores: atomic TAS, CAS and CAS2 (ac090.h, ac090_atomic) ---------- */

int ac090_atomic;
static uint64_t atomic_count;
uint64_t ac090_atomic_ops(void) { return __atomic_load_n(&atomic_count, __ATOMIC_RELAXED); }
#if defined(__x86_64__) || defined(__i386__)
#define CPU_RELAX() __builtin_ia32_pause()
#elif defined(__aarch64__)
#define CPU_RELAX() __asm__ volatile("yield")
#else
#define CPU_RELAX() ((void)0)
#endif
#define HOSTLOCKS 1024
static volatile int hostlocks[HOSTLOCKS];
static unsigned hostlock_of(uint32_t addr) { uint32_t l = addr >> 4; return (l ^ (l >> 10) ^ (l >> 20)) & (HOSTLOCKS - 1); }
void ac090_hostlock(uint32_t addr) {
    volatile int *l = &hostlocks[hostlock_of(addr)];
    while (__atomic_exchange_n(l, 1, __ATOMIC_ACQUIRE)) while (__atomic_load_n(l, __ATOMIC_RELAXED)) CPU_RELAX();
}
void ac090_hostunlock(uint32_t addr) { __atomic_store_n(&hostlocks[hostlock_of(addr)], 0, __ATOMIC_RELEASE); }

/* The host byte behind guest operand a (sz bytes, one 64 KB bus page, the
 * MMU's translation taken as a write), or NULL: not RAM the host reaches
 * directly, a write log running, or atomics off. */
static uint8_t *atomic_host(ac090 *c, uint32_t a, int sz) {
    if (!ac090_atomic || c->wlog_on || c->probe) return NULL;
    if (c->mmu_on) {
        if ((a & 0xfff) + (uint32_t)sz > 0x1000) return NULL;
        a = mmu_xlate(c, a, 1, fc_data(c), sz);
    }
    a = mask(c, a);
    if ((a & 0xffff) + (uint32_t)sz > 0x10000) return NULL;
    uint8_t *p = c->bus.wpage[a >> 16];
    if (!p) return NULL;
    BUS_WAIT(c, a);
    return p + (a & 0xffff);
}
static uint32_t be_get(const uint8_t *p, int sz) { return sz == 1 ? p[0] : sz == 2 ? (uint32_t)(p[0] << 8 | p[1]) : (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
/* Compare-and-exchange of sz bytes at p (big-endian guest values): the old
 * value, which equals want when the exchange was made. */
static uint32_t atomic_cas(uint8_t *p, int sz, uint32_t want, uint32_t put, uint32_t guest) {
    __atomic_add_fetch(&atomic_count, 1, __ATOMIC_RELAXED);
    if (sz == 1) { uint8_t e = (uint8_t)want; __atomic_compare_exchange_n(p, &e, (uint8_t)put, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); return e; }
    if (sz == 2 && !((uintptr_t)p & 1)) {
        uint16_t e = __builtin_bswap16((uint16_t)want);
        __atomic_compare_exchange_n((uint16_t *)(void *)p, &e, __builtin_bswap16((uint16_t)put), 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
        return __builtin_bswap16(e);
    }
    if (sz == 4 && !((uintptr_t)p & 3)) {
        uint32_t e = __builtin_bswap32(want);
        __atomic_compare_exchange_n((uint32_t *)(void *)p, &e, __builtin_bswap32(put), 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
        return __builtin_bswap32(e);
    }
    ac090_hostlock(guest);                                /* not aligned: the line's host lock */
    uint32_t old = be_get(p, sz);
    if (old == want) for (int i = 0; i < sz; i++) p[i] = (uint8_t)(put >> (8 * (sz - 1 - i)));
    ac090_hostunlock(guest);
    return old;
}

static void op_tas(ac090 *c) {
    ea_t e = ea(c, MODE, REG, 1);
    uint8_t *p = e.kind == EA_MEM ? atomic_host(c, e.addr, 1) : NULL;
    if (p) {
        __atomic_add_fetch(&atomic_count, 1, __ATOMIC_RELAXED);
        logic_flags(c, __atomic_fetch_or(p, 0x80, __ATOMIC_SEQ_CST), 1);
        return;
    }
    uint32_t v = ea_read(c, &e, 1);
    logic_flags(c, v, 1);
    ea_write(c, &e, 1, v | 0x80);
}

static void op_trap(ac090 *c) { exception(c, V_TRAP + (OP & 15), c->pc); }

static void op_link(ac090 *c) {
    uint32_t disp = (OP & 0xfff8) == 0x4808 ? fetch32(c) : (uint32_t)(int16_t)fetch16(c); /* 0x4e50 word, 0x4808 long */
    c->a[7] -= 4; wr32(c, c->a[7], c->a[REG]);
    c->a[REG] = c->a[7];
    c->a[7] += disp;
}
static void op_unlk(ac090 *c) {
    uint32_t sp = c->a[REG];
    uint32_t v = rd32(c, sp);
    c->a[7] = sp + 4;
    c->a[REG] = v;
}
static void op_move_usp(ac090 *c) {
    if (!c->s) { privilege(c); return; }
    if (OP & 8) c->a[REG] = c->usp; else c->usp = c->a[REG];
}

static void op_misc4e7(ac090 *c) {
    switch (OP & 7) {
    case 0: /* RESET: everything else on the board resets; the CPU goes on */
        if (!c->s) { privilege(c); return; }
        c->clock += 518 - 255;   /* the table holds 255 of RESET's 518 */
        if (c->reset_hook) {
            int n = (int)((0x1000 - (c->pc & 0xfff)) >> 1);      /* the pipeline: up to 8 words, within the page */
            if (n > 8) n = 8;
            for (int i = 0; i < n; i++) c->pf_word[i] = (uint16_t)rd16_prog(c, c->pc + 2u * (uint32_t)i);
            c->pf_base = c->pc; c->pf_len = n;
            c->reset_hook(c->bus.opaque);
        }
        return;
    case 1: return; /* NOP */
    case 2: /* STOP */
        if (!c->s) { privilege(c); return; }
        ac090_set_sr(c, (uint16_t)fetch16(c));
        c->stopped = 1;
        return;
    case 3: { /* RTE */
        if (!c->s) { privilege(c); return; }
        uint32_t sp = c->a[7];
        uint16_t sr = (uint16_t)rd16(c, sp);
        uint32_t pc = rd32(c, sp + 2);
        uint32_t size = 6;
        if (c->model >= AC090_68020) {
            uint32_t fmt = rd16(c, sp + 6) >> 12;
            if (fmt == 0 || fmt == 1) size = 8;
            else if (fmt == 2) size = 12;
            else if (fmt == 9) size = 20;
            else if (fmt == 0xa) size = 32;
            else if (fmt == 0xb) size = 92;
            else if (fmt == 7 && c->model == AC090_68040) size = 60;   /* access error: the instruction restarts */
            else { exception(c, V_FORMAT, c->op_pc); return; }
            c->a[7] = sp + size;
            ac090_set_sr(c, sr);
            if (fmt == 1) { c->op_pc = pc; op_misc4e7(c); return; } /* throwaway: return through the next frame */
            jump(c, pc);
            if (0) { /* throwaway: continue with the next frame */ c->op_pc = pc; op_misc4e7(c); }
            return;
        }
        if ((pc & 1) && c->model == AC090_68000) address_error(c);
        c->a[7] = sp + size;
        ac090_set_sr(c, sr);
        c->pc = pc;
        return;
    }
    case 4: { /* RTD, 68020 */
        if (c->model < AC090_68020) { illegal(c); return; }
        uint32_t disp = (uint32_t)(int16_t)fetch16(c);
        uint32_t to = rd32(c, c->a[7]);
        c->a[7] += 4 + disp;
        jump(c, to);
        return;
    }
    case 5: /* RTS */
        jump(c, rd32(c, c->a[7]));
        c->a[7] += 4;
        return;
    case 6: /* TRAPV */
        if (c->v) exception(c, V_TRAPV, c->pc);
        return;
    default: { /* RTR */
        uint32_t v = rd16(c, c->a[7]);
        jump(c, rd32(c, c->a[7] + 2));
        c->a[7] += 6;
        set_ccr(c, v);
        return;
    }
    }
}

static void op_movec(ac090 *c) {
    if (!c->s) { privilege(c); return; }
    uint32_t ext = fetch16(c);
    uint32_t *r = ext & 0x8000 ? &c->a[ext >> 12 & 7] : &c->d[ext >> 12 & 7];
    uint32_t *cr;
    int m40 = c->model == AC090_68040;
    switch (ext & 0xfff) {
    case 0x000: cr = &c->sfc; break;
    case 0x001: cr = &c->dfc; break;
    case 0x002: cr = &c->cacr; break;
    case 0x003: if (!m40) { illegal(c); return; } cr = &c->tc; break;
    case 0x004: if (!m40) { illegal(c); return; } cr = &c->itt0; break;
    case 0x005: if (!m40) { illegal(c); return; } cr = &c->itt1; break;
    case 0x006: if (!m40) { illegal(c); return; } cr = &c->dtt0; break;
    case 0x007: if (!m40) { illegal(c); return; } cr = &c->dtt1; break;
    case 0x800: cr = &c->usp; break;
    case 0x801: cr = &c->vbr; break;
    case 0x802: if (m40) { illegal(c); return; } cr = &c->caar; break;   /* no CAAR on the 68040 */
    case 0x803: save_sp(c); cr = &c->msp; break;
    case 0x804: save_sp(c); cr = &c->isp; break;
    case 0x805: if (!m40) { illegal(c); return; } cr = &c->mmusr; break;
    case 0x806: if (!m40) { illegal(c); return; } cr = &c->urp; break;
    case 0x807: if (!m40) { illegal(c); return; } cr = &c->srp; break;
    default: illegal(c); return;                 /* 0x808 PCR: 68060 only */
    }
    if (OP & 1) {
        save_sp(c);                  /* A7 into ISP/MSP first: load_sp below reads them back */
        uint32_t cacr_was = c->cacr;
        *cr = *r;
        if (cr == &c->cacr && m40) {   /* 68040: DE and IE; clearing is CINV/CPUSH */
            c->cacr &= 0x80008000u;
            /* IE on or off changes how the JIT watches code (flush_only):
             * translations are checked again. A write that leaves IE as it
             * was (CacheControl for the data cache, or the same bits again)
             * clears nothing on a 68040, and checks nothing again here */
            if ((c->cacr ^ cacr_was) & 0x8000) { c->cache_epoch++; c->clr_n[AC090_CLR_CACR]++; }
            else c->clr_n[AC090_CLR_CACR_SAME]++;
        }
        else if (cr == &c->tc || cr == &c->urp || cr == &c->srp) {   /* the MMU: TC E and P; root tables 512-byte aligned */
            c->tc &= 0xc000; c->urp &= ~0x1ffu; c->srp &= ~0x1ffu;
            mmu_changed(c);
        }
        else if (cr == &c->itt0 || cr == &c->itt1 || cr == &c->dtt0 || cr == &c->dtt1) { *cr &= 0xffffe364u; mmu_io_combine(c); }   /* device pages the DTTs pass */
        else if (cr == &c->cacr) {
            if (c->cacr & 8) memset(c->icache_valid, 0, sizeof c->icache_valid);
            if (c->cacr & 4) c->icache_valid[c->caar >> 2 & 63] = 0;
            if (c->cacr & 0xc) c->cache_epoch++;   /* translated code is rechecked */
            c->cacr &= 3;
        }
        load_sp(c);
    }
    else { *r = *cr; }
}

static void op_jsr_jmp(ac090 *c) {
    ea_t e = ea(c, MODE, REG, 4);
    if ((e.addr & 1) && c->model == AC090_68000) address_error(c);
    if (!(OP & 0x40)) { c->a[7] -= 4; wr32(c, c->a[7], c->pc); }
    if (e.addr & 1) { address_error_020(c, e.addr); return; }
    c->pc = e.addr;
    c->flow = 1;
}

static void op_lea(ac090 *c) { ea_t e = ea(c, MODE, REG, 4); c->a[RX] = e.addr; }

static void op_chk(ac090 *c) {
    int sz = OP & 0x80 ? 2 : 4;
    ea_t e = ea(c, MODE, REG, sz);
    int32_t bound = sext(ea_read(c, &e, sz), sz);
    int32_t v = sext(c->d[RX], sz);
    c->z = v == 0; c->v = 0; c->c = 0;
    if (v < 0) { c->n = 1; exception(c, V_CHK, c->pc); }
    else if (v > bound) { c->n = 0; exception(c, V_CHK, c->pc); }
}

static void zero_divide(ac090 *c);

static void op_mull(ac090 *c) { /* MULU.L / MULS.L, 68020 */
    uint32_t ext = fetch16(c);
    ea_t e = ea(c, MODE, REG, 4);
    uint32_t s = ea_read(c, &e, 4);
    int dl = ext >> 12 & 7, dh = ext & 7, sgn = ext >> 11 & 1, wide = ext >> 10 & 1;
    uint64_t r;
    if (sgn) r = (uint64_t)((int64_t)(int32_t)s * (int32_t)c->d[dl]);
    else r = (uint64_t)s * c->d[dl];
    c->c = 0;
    if (wide) {
        c->d[dh] = (uint32_t)(r >> 32); c->d[dl] = (uint32_t)r;
        c->n = (r >> 63) & 1; c->z = r == 0; c->v = 0;
    } else {
        uint32_t lo = (uint32_t)r;
        c->d[dl] = lo;
        c->n = lo >> 31; c->z = lo == 0;
        c->v = sgn ? (int64_t)r != (int64_t)(int32_t)lo : (r >> 32) != 0;
    }
}

static void op_divl(ac090 *c) { /* DIVU.L / DIVS.L, 68020 */
    uint32_t ext = fetch16(c);
    ea_t e = ea(c, MODE, REG, 4);
    uint32_t s = ea_read(c, &e, 4);
    int dq = ext >> 12 & 7, dr = ext & 7, sgn = ext >> 11 & 1, wide = ext >> 10 & 1;
    if (s == 0) { zero_divide(c); return; }
    c->c = 0;
    if (sgn) {
        int64_t n = wide ? (int64_t)((uint64_t)c->d[dr] << 32 | c->d[dq]) : (int64_t)(int32_t)c->d[dq];
        int64_t q, r;
        if (n == INT64_MIN && (int32_t)s == -1) { c->v = 1; return; }
        q = n / (int32_t)s; r = n % (int32_t)s;
        if (q != (int32_t)q) { c->v = 1; return; }
        if (dr != dq) c->d[dr] = (uint32_t)r;
        c->d[dq] = (uint32_t)q;
        c->n = ((uint32_t)q) >> 31; c->z = q == 0; c->v = 0;
    } else {
        uint64_t n = wide ? ((uint64_t)c->d[dr] << 32 | c->d[dq]) : c->d[dq];
        uint64_t q = n / s, r = n % s;
        if (q > 0xffffffffu) { c->v = 1; return; }
        if (dr != dq) c->d[dr] = (uint32_t)r;
        c->d[dq] = (uint32_t)q;
        c->n = ((uint32_t)q) >> 31; c->z = q == 0; c->v = 0;
    }
}

/* ---- group 5: ADDQ SUBQ Scc DBcc TRAPcc ------------------------------------ */

static void op_addq_subq(ac090 *c) {
    uint32_t q = RX ? RX : 8;
    int sz = SZ;
    if (MODE == 1) { /* address register: whole register, no flags */
        if (OP & 0x100) c->a[REG] -= q; else c->a[REG] += q;
        return;
    }
    ea_t e = ea(c, MODE, REG, sz);
    uint32_t d = ea_read(c, &e, sz);
    uint32_t r = OP & 0x100 ? do_sub(c, q, d, 0, sz, 0, 1) : do_add(c, q, d, 0, sz, 0);
    ea_write(c, &e, sz, r);
}

static void op_scc(ac090 *c) {
    ea_t e = ea(c, MODE, REG, 1);
    if (c->model == AC090_68000 && e.kind == EA_MEM) ea_read(c, &e, 1);
    ea_write(c, &e, 1, cond(c, OP >> 8 & 15) ? 0xff : 0);
}

static void op_dbcc(ac090 *c) {
    uint32_t base = c->pc;
    uint32_t disp = (uint32_t)(int16_t)fetch16(c);
    if (cond(c, OP >> 8 & 15)) return;
    uint32_t v = (c->d[REG] - 1) & 0xffff;
    c->d[REG] = (c->d[REG] & 0xffff0000u) | v;
    if (v != 0xffff) jump(c, base + disp);
    else c->clock += 4;                                               /* count expired: 10 */
}

static void op_trapcc(ac090 *c) {
    int m = REG;
    if (m == 2) fetch16(c); else if (m == 3) fetch32(c);
    if (cond(c, OP >> 8 & 15)) exception(c, V_TRAPV, c->pc);
}

/* ---- group 6: branches ------------------------------------------------------ */

static void op_bcc(ac090 *c) {
    uint32_t base = c->pc;
    uint32_t disp = (uint32_t)(int8_t)OP;
    if ((OP & 0xff) == 0) disp = (uint32_t)(int16_t)fetch16(c);
    else if ((OP & 0xff) == 0xff && c->model >= AC090_68020) disp = fetch32(c);
    int cc = OP >> 8 & 15;
    if (cc == 1) { if (((base + disp) & 1) && c->model == AC090_68000) address_error(c); c->a[7] -= 4; wr32(c, c->a[7], c->pc); jump(c, base + disp); return; } /* BSR */
    if (cond(c, cc)) { c->clock += 2; jump(c, base + disp); }          /* taken: 6 */
    else if ((OP & 0xff) == 0 || (OP & 0xff) == 0xff) c->clock += 2;  /* .W/.L not taken: 6 */
}

/* ---- MOVEQ ------------------------------------------------------------------- */

static void op_moveq(ac090 *c) {
    uint32_t v = (uint32_t)(int8_t)OP;
    c->d[RX] = v;
    logic_flags(c, v, 4);
}

/* ---- groups 8 and C: OR AND DIVU DIVS MULU MULS SBCD ABCD EXG ---------------- */

static void op_or_and(ac090 *c) {
    int sz = SZ, and = (OP >> 12) == 0xc;
    ea_t e = ea(c, MODE, REG, sz);
    uint32_t s = ea_read(c, &e, sz), r;
    if (OP & 0x100) { /* Dn op <ea> -> <ea> */
        r = and ? s & c->d[RX] : s | c->d[RX];
        logic_flags(c, r, sz);
        ea_write(c, &e, sz, r);
    } else {
        r = and ? s & c->d[RX] : s | c->d[RX];
        logic_flags(c, r, sz);
        ea_t d = { EA_D, (uint32_t)RX };
        ea_write(c, &d, sz, r);
    }
}

/* Divide by zero. Motorola leaves N, Z and V undefined and clears C. The
 * one corpus case (DIVU 5745) clears all four, and so does this. */
static void zero_divide(ac090 *c) {
    c->n = c->z = c->v = c->c = 0;
    exception(c, V_ZERO, c->pc);
}

static void op_divu(ac090 *c) {
    ea_t e = ea(c, MODE, REG, 2);
    uint32_t s = ea_read(c, &e, 2);
    if (!s) { zero_divide(c); return; }
    uint32_t d = c->d[RX];
    uint32_t q = d / s, r = d % s;
    c->c = 0;
    if (q > 0xffff) { c->v = 1; c->n = 1; c->z = 0; return; }
    c->d[RX] = r << 16 | q;
    c->n = (q >> 15) & 1; c->z = q == 0; c->v = 0;
}

static void op_divs(ac090 *c) {
    ea_t e = ea(c, MODE, REG, 2);
    int32_t s = (int16_t)ea_read(c, &e, 2);
    if (!s) { zero_divide(c); return; }
    int32_t d = (int32_t)c->d[RX];
    c->c = 0;
    if (d == INT32_MIN && s == -1) { c->v = 1; c->n = 0; c->z = 1; return; }
    int32_t q = d / s, r = d % s;
    if (q != (int16_t)q) { c->v = 1; c->n = 1; c->z = 0; return; }
    c->d[RX] = ((uint32_t)r & 0xffff) << 16 | ((uint32_t)q & 0xffff);
    c->n = (q >> 15) & 1; c->z = (q & 0xffff) == 0; c->v = 0;
}

static void op_mulu(ac090 *c) {
    ea_t e = ea(c, MODE, REG, 2);
    uint32_t r = ea_read(c, &e, 2) * (c->d[RX] & 0xffff);
    c->d[RX] = r;
    logic_flags(c, r, 4);
}
static void op_muls(ac090 *c) {
    ea_t e = ea(c, MODE, REG, 2);
    uint32_t r = (uint32_t)((int16_t)ea_read(c, &e, 2) * (int16_t)c->d[RX]);
    c->d[RX] = r;
    logic_flags(c, r, 4);
}

/* BCD by carry chains, invalid digits included. Flamewing's analysis of
 * the 68000; matches both the Tom Harte and the MAME corpora exactly. */
static uint32_t bcd_add(ac090 *c, uint32_t src, uint32_t dst) {
    uint32_t ss = src + dst + c->x;
    uint32_t bc = ((src & dst) | (~ss & (src | dst))) & 0x88;
    uint32_t dc = (((ss + 0x66) ^ ss) & 0x110) >> 1;
    uint32_t corf = (bc | dc) - ((bc | dc) >> 2);
    uint32_t rr = ss + corf;
    c->x = c->c = ((bc | (ss & ~rr)) >> 7) & 1;
    c->v = ((~ss & rr) >> 7) & 1;
    c->n = (rr >> 7) & 1;
    if (rr & 0xff) c->z = 0;
    return rr & 0xff;
}
static uint32_t bcd_sub(ac090 *c, uint32_t src, uint32_t dst) {
    uint32_t dd = dst - src - c->x;
    uint32_t bc = ((~dst & src) | (dd & ~(dst ^ src))) & 0x88;
    uint32_t corf = bc - (bc >> 2);
    uint32_t rr = dd - corf;
    c->x = c->c = ((bc | (~dd & rr)) >> 7) & 1;
    c->v = ((dd & ~rr) >> 7) & 1;
    c->n = (rr >> 7) & 1;
    if (rr & 0xff) c->z = 0;
    return rr & 0xff;
}

static void op_nbcd(ac090 *c) {
    ea_t e = ea(c, MODE, REG, 1);
    uint32_t d = ea_read(c, &e, 1);
    ea_write(c, &e, 1, bcd_sub(c, d, 0));
}

static void op_abcd_sbcd(ac090 *c) {
    int add = (OP >> 12) == 0xc;
    if (OP & 8) { /* -(Ay),-(Ax) */
        c->a[REG] -= REG == 7 ? 2 : 1;
        uint32_t s = rd8(c, c->a[REG]);
        c->a[RX] -= RX == 7 ? 2 : 1;
        uint32_t d = rd8(c, c->a[RX]);
        wr8(c, c->a[RX], add ? bcd_add(c, s, d) : bcd_sub(c, s, d));
    } else {
        uint32_t s = c->d[REG] & 0xff, d = c->d[RX] & 0xff;
        uint32_t r = add ? bcd_add(c, s, d) : bcd_sub(c, s, d);
        c->d[RX] = (c->d[RX] & ~0xffu) | r;
    }
}

static void op_exg(ac090 *c) {
    uint32_t t;
    switch (OP >> 3 & 0x1f) {
    case 0x08: t = c->d[RX]; c->d[RX] = c->d[REG]; c->d[REG] = t; break;
    case 0x09: t = c->a[RX]; c->a[RX] = c->a[REG]; c->a[REG] = t; break;
    default: t = c->d[RX]; c->d[RX] = c->a[REG]; c->a[REG] = t; break;
    }
}

/* ---- groups 9 and D: SUB ADD SUBA ADDA SUBX ADDX ------------------------------ */

static void op_add_sub(ac090 *c) {
    int sz = SZ, add = (OP >> 12) == 0xd;
    ea_t e = ea(c, MODE, REG, sz);
    uint32_t s = ea_read(c, &e, sz);
    if (OP & 0x100) { /* Dn op <ea> -> <ea> */
        uint32_t r = add ? do_add(c, c->d[RX], s, 0, sz, 0) : do_sub(c, c->d[RX], s, 0, sz, 0, 1);
        ea_write(c, &e, sz, r);
    } else {
        uint32_t r = add ? do_add(c, s, c->d[RX], 0, sz, 0) : do_sub(c, s, c->d[RX], 0, sz, 0, 1);
        ea_t d = { EA_D, (uint32_t)RX };
        ea_write(c, &d, sz, r);
    }
}

static void op_adda_suba(ac090 *c) {
    int sz = OP & 0x100 ? 4 : 2, add = (OP >> 12) == 0xd;
    ea_t e = ea(c, MODE, REG, sz);
    uint32_t s = ea_read(c, &e, sz);
    if (sz == 2) s = (uint32_t)(int16_t)s;
    c->a[RX] = add ? c->a[RX] + s : c->a[RX] - s;
}

static void op_addx_subx(ac090 *c) {
    int sz = SZ, add = (OP >> 12) == 0xd;
    if (OP & 8) {
        uint32_t step = sz == 1 && REG == 7 ? 2 : sz;
        c->a[REG] -= step;
        uint32_t s = sz == 1 ? rd8(c, c->a[REG]) : sz == 2 ? rd16(c, c->a[REG]) : rd32(c, c->a[REG]);
        step = sz == 1 && RX == 7 ? 2 : sz;
        c->a[RX] -= step;
        uint32_t d = sz == 1 ? rd8(c, c->a[RX]) : sz == 2 ? rd16(c, c->a[RX]) : rd32(c, c->a[RX]);
        uint32_t r = add ? do_add(c, s, d, c->x, sz, 1) : do_sub(c, s, d, c->x, sz, 1, 1);
        if (sz == 1) wr8(c, c->a[RX], r); else if (sz == 2) wr16(c, c->a[RX], r); else wr32(c, c->a[RX], r);
    } else {
        uint32_t r = add ? do_add(c, c->d[REG], c->d[RX], c->x, sz, 1) : do_sub(c, c->d[REG], c->d[RX], c->x, sz, 1, 1);
        uint32_t m = size_mask(sz);
        c->d[RX] = (c->d[RX] & ~m) | r;
    }
}

/* ---- group B: CMP CMPA CMPM EOR ---------------------------------------------- */

static void op_cmp(ac090 *c) {
    int sz = SZ;
    ea_t e = ea(c, MODE, REG, sz);
    uint32_t s = ea_read(c, &e, sz);
    do_sub(c, s, c->d[RX], 0, sz, 0, 0);
}
static void op_cmpa(ac090 *c) {
    int sz = OP & 0x100 ? 4 : 2;
    ea_t e = ea(c, MODE, REG, sz);
    uint32_t s = ea_read(c, &e, sz);
    if (sz == 2) s = (uint32_t)(int16_t)s;
    do_sub(c, s, c->a[RX], 0, 4, 0, 0);
}
static void op_cmpm(ac090 *c) {
    int sz = SZ;
    uint32_t step = sz == 1 && REG == 7 ? 2 : sz;
    uint32_t sa = c->a[REG]; c->a[REG] += step;
    uint32_t s = sz == 1 ? rd8(c, sa) : sz == 2 ? rd16(c, sa) : rd32(c, sa);
    step = sz == 1 && RX == 7 ? 2 : sz;
    uint32_t da = c->a[RX]; c->a[RX] += step;
    uint32_t d = sz == 1 ? rd8(c, da) : sz == 2 ? rd16(c, da) : rd32(c, da);
    do_sub(c, s, d, 0, sz, 0, 0);
}
static void op_eor(ac090 *c) {
    int sz = SZ;
    ea_t e = ea(c, MODE, REG, sz);
    uint32_t r = ea_read(c, &e, sz) ^ c->d[RX];
    logic_flags(c, r, sz);
    ea_write(c, &e, sz, r);
}

/* ---- group E: shifts and rotates ---------------------------------------------- */

static uint32_t shift(ac090 *c, int kind, int left, uint32_t v, uint32_t count, int sz) {
    uint32_t m = size_mask(sz), sb = sign_bit(sz);
    int bits = sz * 8;
    v &= m;
    c->v = 0;
    if (count == 0) {
        c->c = kind == 2 ? c->x : 0;
        nz(c, v, sz);
        return v;
    }
    switch (kind) {
    case 0: /* AS */
        if (left) {
            uint32_t r = v;
            int overflow = 0;
            for (uint32_t i = 0; i < count; i++) {
                uint32_t out = (r & sb) != 0;
                r = (r << 1) & m;
                if (((r & sb) != 0) != out) overflow = 1;
                c->c = c->x = out;
            }
            c->v = overflow;
            nz(c, r, sz);
            return r;
        } else {
            int32_t sv = sext(v, sz);
            uint32_t r;
            if (count >= (uint32_t)bits) { c->c = c->x = sv < 0; r = sv < 0 ? m : 0; }
            else { c->c = c->x = (sv >> (count - 1)) & 1; r = (uint32_t)(sv >> count) & m; }
            nz(c, r, sz);
            return r;
        }
    case 1: /* LS */
        if (left) {
            uint32_t r;
            if (count > (uint32_t)bits) { c->c = c->x = 0; r = 0; }
            else { c->c = c->x = (uint32_t)(((uint64_t)v << count) >> bits) & 1; r = (uint32_t)((uint64_t)v << count) & m; }
            nz(c, r, sz);
            return r;
        } else {
            uint32_t r;
            if (count > (uint32_t)bits) { c->c = c->x = 0; r = 0; }
            else { c->c = c->x = (uint32_t)(v >> (count - 1)) & 1; r = (uint32_t)((uint64_t)v >> count); }
            nz(c, r, sz);
            return r;
        }
    case 2: { /* ROX */
        uint32_t r = v, x = c->x;
        count %= (uint32_t)(bits + 1);
        for (uint32_t i = 0; i < count; i++) {
            if (left) { uint32_t out = (r & sb) != 0; r = ((r << 1) | x) & m; x = out; }
            else { uint32_t out = r & 1; r = (r >> 1) | (x ? sb : 0); x = out; }
        }
        c->x = c->c = x;
        nz(c, r, sz);
        return r;
    }
    default: { /* RO */
        uint32_t k = count & (uint32_t)(bits - 1), r;
        if (left) r = k ? ((v << k) | (v >> (bits - k))) & m : v;
        else r = k ? ((v >> k) | (v << (bits - k))) & m : v;
        c->c = left ? r & 1 : (r & sb) != 0;
        nz(c, r, sz);
        return r;
    }
    }
}

static void op_shift_reg(ac090 *c) {
    int sz = SZ, kind = OP >> 3 & 3, left = OP >> 8 & 1;
    uint32_t count = OP & 0x20 ? c->d[RX] & 63 : (RX ? RX : 8);
    uint32_t r = shift(c, kind, left, c->d[REG], count, sz);
    uint32_t m = size_mask(sz);
    c->d[REG] = (c->d[REG] & ~m) | r;
    c->clock += count;
}

static void op_shift_mem(ac090 *c) {
    int kind = OP >> 9 & 3, left = OP >> 8 & 1;
    ea_t e = ea(c, MODE, REG, 2);
    uint32_t v = ea_read(c, &e, 2);
    ea_write(c, &e, 2, shift(c, kind, left, v, 1, 2));
}

/* ---- 68020 bit fields ------------------------------------------------------- */

static void op_bitfield(ac090 *c) {
    int kind = OP >> 8 & 7; /* 0 TST 1 EXTU 2 CHG 3 EXTS 4 CLR 5 FFO 6 SET 7 INS */
    uint32_t ext = fetch16(c);
    int32_t offset = ext & 0x800 ? (int32_t)c->d[ext >> 6 & 7] : (int32_t)(ext >> 6 & 31);
    uint32_t width = ext & 0x20 ? c->d[ext & 7] & 31 : ext & 31;
    if (width == 0) width = 32;
    uint32_t field, mask32;
    if (MODE == 0) {
        uint32_t d = c->d[REG];
        uint32_t off = (uint32_t)offset & 31;
        uint32_t rot = (d << off) | (off ? d >> (32 - off) : 0);
        field = rot >> (32 - width);
        mask32 = width == 32 ? 0xffffffffu : (0xffffffffu << (32 - width));
        c->n = field >> (width - 1) & 1; c->z = field == 0; c->v = 0; c->c = 0;
        uint32_t newrot = rot;
        switch (kind) {
        case 0: return;
        case 1: c->d[ext >> 12 & 7] = field; return;
        case 3: c->d[ext >> 12 & 7] = width == 32 ? field : (uint32_t)((int32_t)(field << (32 - width)) >> (32 - width)); return;
        case 5: { uint32_t i = 0; while (i < width && !(field >> (width - 1 - i) & 1)) i++; c->d[ext >> 12 & 7] = (uint32_t)offset + i; return; }
        case 2: newrot = rot ^ mask32; break;
        case 4: newrot = rot & ~mask32; break;
        case 6: newrot = rot | mask32; break;
        case 7: {
            uint32_t ins = c->d[ext >> 12 & 7] & (width == 32 ? 0xffffffffu : ((1u << width) - 1));
            c->n = ins >> (width - 1) & 1; c->z = ins == 0;
            newrot = (rot & ~mask32) | (ins << (32 - width));
            break;
        }
        }
        c->d[REG] = off ? (newrot >> off) | (newrot << (32 - off)) : newrot;
        return;
    }
    ea_t e = ea(c, MODE, REG, 1);
    uint32_t base = e.addr + (uint32_t)(offset >> 3);
    int bitoff = offset & 7;
    uint64_t data = 0;
    int bytes = (bitoff + (int)width + 7) / 8;
    for (int i = 0; i < bytes; i++) data = data << 8 | rd8(c, base + i);
    int shiftr = bytes * 8 - bitoff - (int)width;
    uint64_t fmask = ((width == 32 ? 0xffffffffull : ((1ull << width) - 1))) << shiftr;
    field = (uint32_t)((data & fmask) >> shiftr);
    c->n = field >> (width - 1) & 1; c->z = field == 0; c->v = 0; c->c = 0;
    uint64_t nd = data;
    switch (kind) {
    case 0: return;
    case 1: c->d[ext >> 12 & 7] = field; return;
    case 3: c->d[ext >> 12 & 7] = width == 32 ? field : (uint32_t)((int32_t)(field << (32 - width)) >> (32 - width)); return;
    case 5: { uint32_t i = 0; while (i < width && !(field >> (width - 1 - i) & 1)) i++; c->d[ext >> 12 & 7] = (uint32_t)offset + i; return; }
    case 2: nd = data ^ fmask; break;
    case 4: nd = data & ~fmask; break;
    case 6: nd = data | fmask; break;
    case 7: {
        uint32_t ins = c->d[ext >> 12 & 7] & (width == 32 ? 0xffffffffu : ((1u << width) - 1));
        c->n = ins >> (width - 1) & 1; c->z = ins == 0;
        nd = (data & ~fmask) | ((uint64_t)ins << shiftr);
        break;
    }
    }
    for (int i = bytes - 1; i >= 0; i--) { wr8(c, base + i, (uint32_t)nd); nd >>= 8; }
}

/* ---- 68020: CAS CAS2 CHK2/CMP2 PACK UNPK MOVES ------------------------------ */

static void op_cas(ac090 *c) {
    static const int csz[4] = { 0, 1, 2, 4 };
    int sz = csz[OP >> 9 & 3];
    uint32_t ext = fetch16(c);
    ea_t e = ea(c, MODE, REG, sz);
    int du = ext >> 6 & 7, dc = ext & 7;
    uint8_t *p = e.kind == EA_MEM ? atomic_host(c, e.addr, sz) : NULL;
    if (p) {                                              /* atomic: written only if equal, in one host operation */
        uint32_t m = size_mask(sz), d = atomic_cas(p, sz, c->d[dc] & m, c->d[du] & m, e.addr);
        uint32_t x = c->x;
        do_sub(c, c->d[dc], d, 0, sz, 0, 0);
        c->x = x;
        if (!c->z) c->d[dc] = (c->d[dc] & ~m) | (d & m);
        return;
    }
    uint32_t d = ea_read(c, &e, sz);
    uint32_t x = c->x;
    do_sub(c, c->d[dc], d, 0, sz, 0, 0);
    c->x = x;
    if (c->z) ea_write(c, &e, sz, c->d[du]);
    else { uint32_t m = size_mask(sz); c->d[dc] = (c->d[dc] & ~m) | (d & m); }
}

static void op_cas2(ac090 *c) {
    int sz = OP & 0x200 ? 4 : 2;
    uint32_t e1 = fetch16(c), e2 = fetch16(c);
    uint32_t a1 = e1 & 0x8000 ? c->a[e1 >> 12 & 7] : c->d[e1 >> 12 & 7];
    uint32_t a2 = e2 & 0x8000 ? c->a[e2 >> 12 & 7] : c->d[e2 >> 12 & 7];
    uint8_t *p1 = ac090_atomic ? atomic_host(c, a1, sz) : NULL, *p2 = p1 ? atomic_host(c, a2, sz) : NULL;
    if (p1 && p2) {                                       /* both operands under their lines' host locks, in address order */
        uint32_t lo = a1 >> 4 < a2 >> 4 ? a1 : a2, hi = lo == a1 ? a2 : a1;
        int same = hostlock_of(lo) == hostlock_of(hi);
        ac090_hostlock(lo); if (!same) ac090_hostlock(hi);
        __atomic_add_fetch(&atomic_count, 1, __ATOMIC_RELAXED);
        uint32_t v1 = be_get(p1, sz), v2 = be_get(p2, sz);
        int dc1 = e1 & 7, dc2 = e2 & 7, du1 = e1 >> 6 & 7, du2 = e2 >> 6 & 7;
        uint32_t x = c->x;
        do_sub(c, c->d[dc1], v1, 0, sz, 0, 0);
        if (c->z) do_sub(c, c->d[dc2], v2, 0, sz, 0, 0);
        c->x = x;
        if (c->z) {
            for (int i = 0; i < sz; i++) p1[i] = (uint8_t)(c->d[du1] >> (8 * (sz - 1 - i)));
            for (int i = 0; i < sz; i++) p2[i] = (uint8_t)(c->d[du2] >> (8 * (sz - 1 - i)));
        } else {
            uint32_t m = size_mask(sz);
            c->d[dc1] = (c->d[dc1] & ~m) | (v1 & m);
            c->d[dc2] = (c->d[dc2] & ~m) | (v2 & m);
        }
        if (!same) ac090_hostunlock(hi);
        ac090_hostunlock(lo);
        return;
    }
    uint32_t v1 = sz == 4 ? rd32(c, a1) : rd16(c, a1);
    uint32_t v2 = sz == 4 ? rd32(c, a2) : rd16(c, a2);
    int dc1 = e1 & 7, dc2 = e2 & 7, du1 = e1 >> 6 & 7, du2 = e2 >> 6 & 7;
    uint32_t x = c->x;
    do_sub(c, c->d[dc1], v1, 0, sz, 0, 0);
    if (c->z) do_sub(c, c->d[dc2], v2, 0, sz, 0, 0);
    c->x = x;
    if (c->z) {
        if (sz == 4) { wr32(c, a1, c->d[du1]); wr32(c, a2, c->d[du2]); }
        else { wr16(c, a1, c->d[du1]); wr16(c, a2, c->d[du2]); }
    } else {
        uint32_t m = size_mask(sz);
        c->d[dc1] = (c->d[dc1] & ~m) | (v1 & m);
        c->d[dc2] = (c->d[dc2] & ~m) | (v2 & m);
    }
}

/* CHK2/CMP2. A data register is compared at the operand size, unsigned;
 * an address register against bounds sign-extended to 32 bits. A lower
 * bound above the upper bound is a range that wraps, which is how signed
 * bounds read when compared unsigned. */
static void op_chk2_cmp2(ac090 *c) {
    static const int csz[4] = { 1, 2, 4, 0 };
    int sz = csz[OP >> 9 & 3];
    uint32_t ext = fetch16(c);
    ea_t e = ea(c, MODE, REG, sz);
    uint32_t lo, hi;
    if (sz == 1) { lo = rd8(c, e.addr); hi = rd8(c, e.addr + 1); }
    else if (sz == 2) { lo = rd16(c, e.addr); hi = rd16(c, e.addr + 2); }
    else { lo = rd32(c, e.addr); hi = rd32(c, e.addr + 4); }
    uint32_t v;
    if (ext & 0x8000) {
        v = c->a[ext >> 12 & 7];
        lo = (uint32_t)sext(lo, sz); hi = (uint32_t)sext(hi, sz);
    } else {
        v = c->d[ext >> 12 & 7] & size_mask(sz);
    }
    c->z = v == lo || v == hi;
    c->c = lo <= hi ? (v < lo || v > hi) : (v > hi && v < lo);
    if (c->c && (ext & 0x800)) exception(c, V_CHK, c->pc);
}

static void op_pack_unpk(ac090 *c) {
    uint32_t adj = fetch16(c);
    int unpk = (OP & 0x1f0) == 0x180;
    if (!(OP & 8)) {
        if (!unpk) {
            uint32_t v = (c->d[REG] + adj) & 0xffff;
            c->d[RX] = (c->d[RX] & ~0xffu) | ((v >> 4 & 0xf0) | (v & 0x0f));
        } else {
            uint32_t v = c->d[REG] & 0xff;
            v = (((v << 4) & 0x0f00) | (v & 0x0f)) + adj;
            c->d[RX] = (c->d[RX] & 0xffff0000u) | (v & 0xffff);
        }
        return;
    }
    if (!unpk) {
        c->a[REG] -= REG == 7 ? 2 : 1; uint32_t v = rd8(c, c->a[REG]);
        c->a[REG] -= REG == 7 ? 2 : 1; v |= rd8(c, c->a[REG]) << 8;
        v = (v + adj) & 0xffff;
        c->a[RX] -= RX == 7 ? 2 : 1; wr8(c, c->a[RX], (v >> 4 & 0xf0) | (v & 0x0f));
    } else {
        c->a[REG] -= REG == 7 ? 2 : 1; uint32_t v = rd8(c, c->a[REG]);
        v = ((((v << 4) & 0x0f00) | (v & 0x0f)) + adj) & 0xffff;
        c->a[RX] -= RX == 7 ? 2 : 1; wr8(c, c->a[RX], v);
        c->a[RX] -= RX == 7 ? 2 : 1; wr8(c, c->a[RX], v >> 8);
    }
}

/* MOVES: one address space on the A1200; with the 68040 MMU on, the access
 * is translated for SFC's or DFC's space. Privileged. */
static void op_moves(ac090 *c) {
    if (!c->s) { privilege(c); return; }
    int sz = SZ;
    uint32_t ext = fetch16(c);
    ea_t e = ea(c, MODE, REG, sz);
    int an = ext >> 15 & 1, r = ext >> 12 & 7;
    if (ext & 0x800) {
        uint32_t v = an ? c->a[r] : c->d[r];
        c->mmu_fc = (int)(c->dfc & 7);       /* translated in DFC's space (the one address space otherwise) */
        ea_write(c, &e, sz, v);
        c->mmu_fc = 0;
    } else {
        c->mmu_fc = (int)(c->sfc & 7);
        uint32_t v = ea_read(c, &e, sz);
        c->mmu_fc = 0;
        if (an) c->a[r] = (uint32_t)sext(v, sz);
        else { uint32_t m = size_mask(sz); c->d[r] = (c->d[r] & ~m) | (v & m); }
    }
}

/* ---- line A and line F ------------------------------------------------------ */

static void op_linea(ac090 *c) { exception(c, V_LINEA, c->op_pc); }
static void op_linef(ac090 *c) { exception(c, V_LINEF, c->op_pc); }

/* ---- 68040 ---- */

/* CINV/CPUSH: invalidate or push caches. There is no data cache model, so
 * the data cache alone (CachePreDMA, CachePostDMA, CacheClearE with
 * CACRF_ClearD) changes nothing: the JIT keeps translations of written code
 * right itself (write protection, dirty pages), not through the data cache.
 * The instruction cache: all of it (CINVA, CPUSHA) has every translation
 * checked again; a line or a page (CINVL, CPUSHL, CINVP, CPUSHP: CacheClearE
 * over a range, one instruction a line) only the translations that meet it,
 * gathered into clr_lo-clr_hi. Until 20 Oct 2026 each line form was a whole
 * clear, and a CacheClearE over a buffer one for each 16 bytes of it. */
static void op_cinv_cpush(ac090 *c) {
    if (!c->s) { privilege(c); return; }
    int caches = OP >> 6 & 3, scope = OP >> 3 & 3;
    if (scope == 0) { illegal(c); return; }
    if (!(caches & 2)) c->clr_n[AC090_CLR_DC]++;
    else if (scope == 3) { c->cache_epoch++; c->clr_n[AC090_CLR_IC_ALL]++; }
    else {
        uint32_t size = scope == 1 ? 16 : (c->tc & 0x4000) ? 8192 : 4096, lo = c->a[REG] & ~(size - 1), hi = lo + (size - 1);
        if (!c->clr_pending) { c->clr_lo = lo; c->clr_hi = hi; c->clr_pending = 1; }
        else { if (lo < c->clr_lo) c->clr_lo = lo; if (hi > c->clr_hi) c->clr_hi = hi; }
        c->clr_n[scope == 1 ? AC090_CLR_IC_LINE : AC090_CLR_IC_PAGE]++;
    }
    c->clock += 8;
}
/* PFLUSH and PTEST (ac090_mmu.c). */
static void op_pflush_ptest(ac090 *c) {
    if (!c->s) { privilege(c); return; }
    if ((OP & 0xffd8) == 0xf548) mmu_ptest(c, !(OP & 0x20), c->a[REG]);   /* PTESTW f548, PTESTR f568 */
    else mmu_pflush(c, OP >> 3 & 3, c->a[REG]);
}
/* MOVE16: one 16-byte line, both addresses aligned down. */
static void move16_line(ac090 *c, uint32_t from, uint32_t to) {
    from &= ~15u; to &= ~15u;
    uint32_t v[4];
    for (int i = 0; i < 4; i++) v[i] = rd32(c, from + 4u * (uint32_t)i);
    for (int i = 0; i < 4; i++) wr32(c, to + 4u * (uint32_t)i, v[i]);
}
static void op_move16(ac090 *c) {
    int ry = OP & 7;
    if ((OP & 0xfff8) == 0xf620) {               /* (Ax)+,(Ay)+ */
        uint16_t ext = (uint16_t)fetch16(c);
        if ((ext & 0x8fff) != 0x8000) { illegal(c); return; }
        int rx = ext >> 12 & 7;
        uint32_t ax = c->a[ry], ay = c->a[rx];
        move16_line(c, ax, ay);
        c->a[ry] += 16;
        if (rx != ry) c->a[rx] += 16;
        return;
    }
    uint32_t abs = fetch32(c);
    switch (OP >> 3 & 3) {
    case 0: move16_line(c, c->a[ry], abs); c->a[ry] += 16; break;   /* (Ay)+,(xxx).L */
    case 1: move16_line(c, abs, c->a[ry]); c->a[ry] += 16; break;   /* (xxx).L,(Ay)+ */
    case 2: move16_line(c, c->a[ry], abs); break;                   /* (Ay),(xxx).L */
    case 3: move16_line(c, abs, c->a[ry]); break;                   /* (xxx).L,(Ay) */
    }
}

/* ---- table ---------------------------------------------------------------------- */

static int ea_ok(int mode, int reg, const char *allowed) {
    /* allowed: string of mode letters: d a m + - o x w l p i (m = (An), o = d16(An),
     * x = d8(An,Xn), w = abs.W, l = abs.L, p = PC modes, i = immediate) */
    char k;
    switch (mode) {
    case 0: k = 'd'; break; case 1: k = 'a'; break; case 2: k = 'm'; break; case 3: k = '+'; break;
    case 4: k = '-'; break; case 5: k = 'o'; break; case 6: k = 'x'; break;
    default:
        if (reg == 0) k = 'w'; else if (reg == 1) k = 'l'; else if (reg == 2 || reg == 3) k = 'p';
        else if (reg == 4) k = 'i'; else return 0;
    }
    return strchr(allowed, k) != NULL;
}

#define ALL "dam+-oxwlpi"
#define DATA "dm+-oxwlpi"
#define ALTER "dam+-oxwl"
#define DALT "dm+-oxwl"
#define MALT "m+-oxwl"
#define CTRL "moxwlp"
#define CALT "moxwl"

static int fpu_present = 1;          /* AC090_NOFPU=1: a 68LC040 */
#include "ac090_fpu.c"

static handler decode(uint16_t op, int model) {
    int mode = op >> 3 & 7, reg = op & 7, szf = op >> 6 & 3;
    int m20 = model >= AC090_68020;
    switch (op >> 12) {
    case 0x0:
        if ((op & 0xf138) == 0x0108) return op_movep;
        if (op & 0x100) {
            int kind = op >> 6 & 3;
            if (kind == 0 ? ea_ok(mode, reg, DATA) : ea_ok(mode, reg, DALT)) return op_bit;
            return 0;
        }
        if ((op & 0xff00) == 0x0800) {
            int kind = op >> 6 & 3;
            if (kind == 0 ? ea_ok(mode, reg, "dm+-oxwlp") : ea_ok(mode, reg, DALT)) return op_bit;
            return 0;
        }
        if (m20 && (op & 0xff00) == 0x0e00 && szf != 3) return ea_ok(mode, reg, MALT) ? op_moves : 0;
        if (m20 && (op & 0xfdff) == 0x0cfc) return op_cas2;
        if (m20 && (op & 0xf9c0) == 0x08c0 && (op & 0x0600)) return ea_ok(mode, reg, MALT) ? op_cas : 0;
        if (m20 && (op & 0xf9c0) == 0x00c0 && (op & 0x0600) != 0x0600) return ea_ok(mode, reg, CTRL) ? op_chk2_cmp2 : 0;
        if ((op & 0x0e00) == 0x0e00) return 0;
        if (szf == 3) return 0;
        {
            int kind = op >> 9 & 7;
            if (kind == 4 || kind == 7) return 0;
            if ((kind == 0 || kind == 1 || kind == 5) && mode == 7 && reg == 4 && szf != 2) return op_imm;
            if (kind == 6) return ea_ok(mode, reg, m20 ? "dm+-oxwlp" : DALT) ? op_imm : 0;
            return ea_ok(mode, reg, DALT) ? op_imm : 0;
        }
    case 0x1: case 0x2: case 0x3: {
        int dmode = op >> 6 & 7, dreg = op >> 9 & 7;
        if ((op >> 12) == 1 && mode == 1) return 0;
        if (!ea_ok(mode, reg, ALL)) return 0;
        if (dmode == 1) return (op >> 12) == 1 ? 0 : op_move;
        if (!ea_ok(dmode, dreg, DALT)) return 0;
        return op_move;
    }
    case 0x4:
        if ((op & 0xfff8) == 0x4e70 && (m20 || (op & 7) != 4)) return op_misc4e7;
        if ((op & 0xfff0) == 0x4e40) return op_trap;
        if ((op & 0xfff8) == 0x4e50) return op_link;
        if (m20 && (op & 0xfff8) == 0x4808) return op_link;
        if ((op & 0xfff8) == 0x4e58) return op_unlk;
        if ((op & 0xfff0) == 0x4e60) return op_move_usp;
        if (m20 && (op & 0xfffe) == 0x4e7a) return op_movec;
        if ((op & 0xff80) == 0x4e80) return ea_ok(mode, reg, CTRL) ? op_jsr_jmp : 0;
        if (m20 && (op & 0xfff8) == 0x49c0) return op_ext; /* EXTB.L, before LEA: 0x49c0 is also LEA's pattern */
        if ((op & 0xf1c0) == 0x41c0) return ea_ok(mode, reg, CTRL) ? op_lea : 0;
        if ((op & 0xf1c0) == 0x4180) return ea_ok(mode, reg, DATA) ? op_chk : 0;
        if (m20 && (op & 0xf1c0) == 0x4100) return ea_ok(mode, reg, DATA) ? op_chk : 0;
        if ((op & 0xffc0) == 0x40c0) return ea_ok(mode, reg, DALT) ? op_move_from_sr : 0;
        if (m20 && (op & 0xffc0) == 0x42c0) return ea_ok(mode, reg, DALT) ? op_move_from_ccr : 0;
        if ((op & 0xffc0) == 0x44c0) return ea_ok(mode, reg, DATA) ? op_move_to_ccr : 0;
        if ((op & 0xffc0) == 0x46c0) return ea_ok(mode, reg, DATA) ? op_move_to_sr : 0;
        if ((op & 0xf900) == 0x4000 && szf != 3) return ea_ok(mode, reg, DALT) ? op_negx_clr_neg_not : 0;
        if ((op & 0xffc0) == 0x4800) return ea_ok(mode, reg, DALT) ? op_nbcd : 0;
        if ((op & 0xfff8) == 0x4840) return op_swap;
        if ((op & 0xffc0) == 0x4840) return ea_ok(mode, reg, CTRL) ? op_pea : 0;
        if ((op & 0xfff8) == 0x4880 || (op & 0xfff8) == 0x48c0) return op_ext;
        if ((op & 0xfb80) == 0x4880) {
            if (op & 0x400) return ea_ok(mode, reg, "m+oxwlp") ? op_movem : 0;
            return ea_ok(mode, reg, "m-oxwl") ? op_movem : 0;
        }
        if (op == 0x4afc) return 0; /* ILLEGAL */
        if ((op & 0xffc0) == 0x4ac0) return ea_ok(mode, reg, DALT) ? op_tas : 0;
        if ((op & 0xff00) == 0x4a00 && szf != 3) return ea_ok(mode, reg, m20 ? ALL : DALT) && !(mode == 1 && szf == 0) ? op_tst : 0;
        if (m20 && (op & 0xffc0) == 0x4c00) return ea_ok(mode, reg, DATA) ? op_mull : 0;
        if (m20 && (op & 0xffc0) == 0x4c40) return ea_ok(mode, reg, DATA) ? op_divl : 0;
        return 0;
    case 0x5:
        if (szf == 3) {
            if (mode == 1) return op_dbcc;
            if (m20 && mode == 7 && reg >= 2 && reg <= 4) return op_trapcc;
            return ea_ok(mode, reg, DALT) ? op_scc : 0;
        }
        if (mode == 1 && szf == 0) return 0;
        return ea_ok(mode, reg, ALTER) ? op_addq_subq : 0;
    case 0x6:
        return op_bcc;
    case 0x7:
        return op & 0x100 ? 0 : op_moveq;
    case 0x8: case 0xc:
        if ((op & 0xf1f0) == 0x8100 || (op & 0xf1f0) == 0xc100) return op_abcd_sbcd;
        if (m20 && ((op & 0xf1f0) == 0x8140 || (op & 0xf1f0) == 0x8180)) return op_pack_unpk;
        if ((op >> 12) == 0xc && ((op & 0xf1f8) == 0xc140 || (op & 0xf1f8) == 0xc148 || (op & 0xf1f8) == 0xc188)) return op_exg;
        if (szf == 3) {
            if (!ea_ok(mode, reg, DATA)) return 0;
            if ((op >> 12) == 0x8) return op & 0x100 ? op_divs : op_divu;
            return op & 0x100 ? op_muls : op_mulu;
        }
        if (op & 0x100) return ea_ok(mode, reg, MALT) ? op_or_and : 0;
        return ea_ok(mode, reg, DATA) ? op_or_and : 0;
    case 0x9: case 0xd:
        if (szf == 3) return ea_ok(mode, reg, ALL) ? op_adda_suba : 0;
        if ((op & 0x130) == 0x100) return op_addx_subx;
        if (op & 0x100) return ea_ok(mode, reg, MALT) ? op_add_sub : 0;
        if (mode == 1 && szf == 0) return 0;
        return ea_ok(mode, reg, ALL) ? op_add_sub : 0;
    case 0xa:
        return op_linea;
    case 0xb:
        if (szf == 3) return ea_ok(mode, reg, ALL) ? op_cmpa : 0;
        if (op & 0x100) {
            if (mode == 1) return op_cmpm;
            return ea_ok(mode, reg, DALT) ? op_eor : 0;
        }
        if (mode == 1 && szf == 0) return 0;
        return ea_ok(mode, reg, ALL) ? op_cmp : 0;
    case 0xe:
        if (szf == 3) {
            if (op & 0x800) {
                if (!m20) return 0;
                int kind = op >> 8 & 7;
                const char *ok = (kind == 0 || kind == 1 || kind == 3 || kind == 5) ? "dmoxwlp" : "dmoxwl";
                return ea_ok(mode, reg, ok) ? op_bitfield : 0;
            }
            return ea_ok(mode, reg, MALT) ? op_shift_mem : 0;
        }
        return op_shift_reg;
    case 0xf:
        if (model == AC090_68040) {
            if ((op & 0xff00) == 0xf400 && (op & 0x18)) return op_cinv_cpush;
            if ((op & 0xffe0) == 0xf500 || (op & 0xffd8) == 0xf548) return op_pflush_ptest;
            if ((op & 0xffe0) == 0xf600 || (op & 0xfff8) == 0xf620) return op_move16;
            if ((op & 0x0e00) == 0x0200 && fpu_present) { handler h = fpu_decode(op); return h ? h : op_linef; }
        }
        return op_linef;
    }
    return 0;
}


/* ---- 68020 cache-case clocks per opcode --------------------------------------- */

/* Fetch effective address (operand read), cache case. */
static int fea(int mode, int reg, int sz) {
    switch (mode) {
    case 0: case 1: return 0;
    case 2: case 3: return 3;
    case 4: return 4;
    case 5: return 4;
    case 6: return 6;
    }
    switch (reg) {
    case 0: return 4;               /* abs.W */
    case 1: return 4;               /* abs.L */
    case 2: return 4;               /* (d16,PC) */
    case 3: return 6;               /* (d8,PC,Xn) */
    case 4: return sz == 4 ? 4 : 2; /* #imm */
    }
    return 0;
}
/* Calculate effective address (LEA, PEA, JMP, JSR, Scc, CLR, MOVE to memory). */
static int cea(int mode, int reg) {
    switch (mode) {
    case 0: case 1: return 0;
    case 2: case 3: case 4: case 5: return 2;
    case 6: return 4;
    }
    if (reg == 1) return 4;         /* abs.L */
    if (reg == 3) return 4;         /* (d8,PC,Xn) */
    return 2;
}
/* Immediate source to a memory destination (ORI..CMPI, bit ops #n). */
static int fiea(int mode, int reg, int sz) {
    int l = sz == 4;
    switch (mode) {
    case 0: case 1: return 0;
    case 2: return l ? 4 : 3;
    case 3: return l ? 7 : 5;
    case 4: return 4;
    case 5: return l ? 6 : 4;
    case 6: return l ? 10 : 8;
    }
    if (reg == 0) return l ? 8 : 6;
    if (reg == 1) return l ? 8 : 6;
    if (reg == 2) return l ? 6 : 4;
    if (reg == 3) return l ? 10 : 8;
    return 0;
}

static int cost020(uint16_t op, handler h) {
    int mode = op >> 3 & 7, reg = op & 7, sz = size_of[op >> 6 & 3];
    int dn = mode == 0;
    if (h == op_moveq) return 2;
    if (h == op_move) {
        static const int msz[4] = { 0, 1, 4, 2 };
        int s2 = msz[op >> 12 & 3], dm = op >> 6 & 7, dr = op >> 9 & 7;
        static const int dst[8] = { 2, 2, 3, 3, 4, 4, 6, 0 };
        int d = dm < 7 ? dst[dm] : (dr == 1 ? 5 : 4);
        return fea(mode, reg, s2) + d;
    }
    if (h == op_imm) {
        int kind = op >> 9 & 7;
        if (mode == 7 && reg == 4) return sz == 1 ? 4 : 8;   /* to CCR / SR */
        if (dn) return sz == 4 ? 4 : 2;
        return (kind == 6 ? 2 : 3) + fiea(mode, reg, sz);
    }
    if (h == op_bit) {
        int kind = op >> 6 & 3, stat = !(op & 0x100);
        int base = kind == 0 ? 4 : 6;
        if (dn) return base;
        return base + (stat ? fiea(mode, reg, 1) : fea(mode, reg, 1));
    }
    if (h == op_add_sub || h == op_or_and) return (op & 0x100) ? 3 + fea(mode, reg, sz) : 2 + fea(mode, reg, sz);
    if (h == op_adda_suba) return ((op & 0x100) ? 2 : 4) + fea(mode, reg, (op & 0x100) ? 4 : 2);
    if (h == op_cmp) return 2 + fea(mode, reg, sz);
    if (h == op_cmpa) return 4 + fea(mode, reg, (op & 0x100) ? 4 : 2);
    if (h == op_cmpm) return 8;
    if (h == op_eor) return dn ? 2 : 3 + fea(mode, reg, sz);
    if (h == op_addx_subx) return (op & 8) ? 9 : 2;
    if (h == op_abcd_sbcd) return (op & 8) ? 13 : 4;
    if (h == op_addq_subq) return (dn || mode == 1) ? 2 : 3 + fea(mode, reg, sz);
    if (h == op_negx_clr_neg_not) {
        int kind = op >> 9 & 3;
        if (dn) return 2;
        return 3 + (kind == 1 ? cea(mode, reg) : fea(mode, reg, sz));
    }
    if (h == op_tst) return 2 + fea(mode, reg, sz);
    if (h == op_tas) return dn ? 4 : 12 + fea(mode, reg, 1);  /* read-modify-write cycle */
    if (h == op_nbcd) return dn ? 6 : 6 + fea(mode, reg, 1);
    if (h == op_swap || h == op_ext) return 4;
    if (h == op_exg) return 4;
    if (h == op_lea) return 2 + cea(mode, reg);
    if (h == op_pea) return 4 + cea(mode, reg);
    if (h == op_jsr_jmp) return 4 + cea(mode, reg);
    if (h == op_scc) return 2 + (dn ? 0 : cea(mode, reg));
    if (h == op_dbcc) return 6;
    if (h == op_bcc) return ((op >> 8 & 15) == 1) ? 6 : 4;
    if (h == op_link) return (op & 0xfff8) == 0x4808 ? 6 : 4;
    if (h == op_unlk) return 5;
    if (h == op_move_usp) return 4;
    if (h == op_movec) return 6;
    if (h == op_move_to_sr) return 8 + fea(mode, reg, 2);
    if (h == op_move_to_ccr) return 4 + fea(mode, reg, 2);
    if (h == op_move_from_sr || h == op_move_from_ccr) return 4 + (dn ? 0 : cea(mode, reg));
    if (h == op_misc4e7) {
        switch (op & 7) {
        case 0: return 518;  /* RESET */
        case 1: return 2;    /* NOP */
        case 2: return 8;    /* STOP */
        case 3: return 18;   /* RTE */
        case 4: return 10;   /* RTD */
        case 5: return 9;    /* RTS */
        case 6: return 4;    /* TRAPV */
        default: return 12;  /* RTR */
        }
    }
    if (h == op_trap) return 4;
    if (h == op_trapcc) return 4;
    if (h == op_chk) return 8 + fea(mode, reg, (op & 0x80) ? 2 : 4);
    if (h == op_chk2_cmp2) return 18 + fea(mode, reg, 4);
    if (h == op_cas) return 12 + fea(mode, reg, 4);
    if (h == op_cas2) return 24;
    if (h == op_moves) return 6 + fea(mode, reg, sz);
    if (h == op_movem) return 8 + cea(mode, reg);
    if (h == op_movep) return (op & 0x40) ? 22 : 16;
    /* Motorola 68020 cache case: MULU.W 25, MULS.W 26, DIVU.W 42, DIVS.W 54,
     * MULx.L 43, DIVU.L 78, DIVS.L 90. */
    if (h == op_mulu) return 25 + fea(mode, reg, 2);
    if (h == op_muls) return 26 + fea(mode, reg, 2);
    if (h == op_divu) return 42 + fea(mode, reg, 2);
    if (h == op_divs) return 54 + fea(mode, reg, 2);
    if (h == op_mull) return 43 + fea(mode, reg, 4);
    if (h == op_divl) return 84 + fea(mode, reg, 4);
    if (h == op_shift_reg) {
        int kind = op >> 3 & 3, byreg = op & 0x20;
        static const int imm[4] = { 6, 4, 12, 6 }, rg[4] = { 6, 6, 12, 8 };
        return byreg ? rg[kind] : imm[kind];
    }
    if (h == op_shift_mem) return ((op >> 9 & 3) == 3 ? 6 : 4) + fea(mode, reg, 2);
    if (h == op_bitfield) {
        int kind = op >> 8 & 7, rmw = kind == 2 || kind == 4 || kind == 6 || kind == 7;
        return dn ? (rmw ? 12 : 8) : (rmw ? 16 : 12) + cea(mode, reg);
    }
    if (h == op_pack_unpk) return (op & 8) ? 11 : ((op & 0x1f0) == 0x180 ? 8 : 6);
    return 4;  /* exceptions (line A/F, illegal) cost in exception() */
}

static handler table20[65536], table40[65536];
static int table20_ready, table40_ready;
#define TABLE(c) ((c)->model == AC090_68040 ? table40 : (c)->model >= AC090_68020 ? table20 : table)

static void build(handler *t, int model) {
    for (uint32_t op = 0; op < 65536; op++) {
        handler h = decode((uint16_t)op, model);
        t[op] = h ? h : illegal;
        if (model >= AC090_68020) { int k = cost020((uint16_t)op, t[op]); clk020[op] = (uint8_t)(k > 255 ? 255 : k); }
        op_recheck[op] = (uint8_t)op_recheck_wants(op);
    }
    if (model == AC090_68000 && !clk020[0x4e71]) for (uint32_t op = 0; op < 65536; op++) clk020[op] = 4;
}

/* ---- public ------------------------------------------------------------------ */

static uint32_t no_read(void *o, uint32_t a, int s) { (void)o; (void)a; (void)s; return 0; }
static void no_write(void *o, uint32_t a, uint32_t v, int s) { (void)o; (void)a; (void)v; (void)s; }

void ac090_init(ac090 *c, int model) {
    memset(c, 0, sizeof *c);
    c->model = model;
    c->stk_span = 0xffffffffu;                  /* adaptive stacks: no window until a board watches */
    c->addr_mask = 0x00ffffff;
    c->dbg_temp = c->dbg_skip = 1;              /* the debugger: no temporary breakpoint, nothing to skip */
    c->bus.read = no_read;
    c->bus.write = no_write;
    if (model == AC090_68000 && !table_ready) { build(table, model); table_ready = 1; }
    if (model == AC090_68020 && !table20_ready) { build(table20, model); table20_ready = 1; }
    if (model == AC090_68040 && !table40_ready) {
        const char *nf = getenv("AC090_NOFPU"); fpu_present = !(nf && *nf == '1'); build(table40, model); table40_ready = 1; }
}

void ac090_reset(ac090 *c) {
    c->s = 1; c->m = 0; c->t1 = c->t0 = 0; c->ipl = 7;
    c->stk_lo = 0; c->stk_span = 0xffffffffu;   /* supervisor: the stack check's window is everything */
    c->vbr = 0; c->cacr = 0;
    c->tc = 0; c->itt0 = c->itt1 = c->dtt0 = c->dtt1 = 0;   /* 68040: translation off after reset */
    c->mmu_on = 0; c->mmu_identity = 1; c->mmu_fc = 0; c->mmu_in_fault = 0;
    mmu_atc_flush(c);
    fpu_reset(c);
    c->stopped = 0; c->halted = 0; c->pf_len = 0;
    c->isp = rd32(c, 0);
    c->a[7] = c->isp;
    c->pc = rd32(c, 4);
}

void ac090_set_irq(ac090 *c, int level) {
    c->irq = level & 7;
    RECHECK(c);
    if (c->irq && ((uint32_t)c->irq > c->ipl || c->irq == 7)) ac090_end_slice(c);   /* translated code leaves at the next block */
}

static inline int take_irq(ac090 *c) {
    if (c->irq && (c->irq > (int)c->ipl || c->irq == 7)) {
        int level = c->irq;
        c->irq_taken[level & 7]++;
        c->clock += 6;
        int was_m = c->m && c->model >= AC090_68020;
        uint32_t pc = c->pc;
        int vec = V_AUTO + level;
        if (c->iack_hook) {                  /* the board answers the acknowledge */
            c->irq = 0;
            int v = c->iack_hook(c, level);
            if (v >= 0) vec = v;
        }
        exception(c, vec, pc);
        c->ipl = (uint32_t)level;
        if (was_m) {
            uint16_t sr = ac090_get_sr(c);   /* S=1, M=1, new IPL */
            save_sp(c);                      /* MSP holds the format 0 frame */
            c->m = 0;
            load_sp(c);                      /* now on the ISP */
            c->a[7] -= 2; wr16(c, c->a[7], 0x1000 | vec * 4);
            c->a[7] -= 4; wr32(c, c->a[7], c->pc);
            c->a[7] -= 2; wr16(c, c->a[7], (uint16_t)(sr & ~0x1000));
        }
        if (!c->iack_hook) c->irq = 0;
        return 1;
    }
    return 0;
}

int ac090_step(ac090 *c) {
    handler *volatile t = TABLE(c);
    if (c->halted) return 2;
    if (!c->probe) take_irq(c);         /* JIT_VERIFY's pass: translated code takes interrupts only between blocks */
    if (c->stopped) { c->clock += 4; return 1; }
    uint64_t paced_from = c->clock, waited_from = c->bus_waited;   /* timing accurate: this instruction's own cycles, scaled below; its bus waits are not */
    c->fault = 0;
    if (setjmp(c->abort)) {
        return c->fault ? 3 : 0;
    }
    if (c->mmu_on | c->berr_regs) { memcpy(c->mmu_regs, c->d, sizeof c->mmu_regs); c->mmu_regs_valid = 1; }   /* an access fault restarts the instruction */
    c->op_pc = c->pc;
    c->op = (uint16_t)fetch16(c);
    c->clock += clk020[c->op];
#ifdef AC090_TRACE
    c->trace_pc[c->trace_pos & 8191] = c->op_pc; c->trace_op[c->trace_pos & 8191] = c->op; c->trace_a7[c->trace_pos & 8191] = c->a[7]; c->trace_pos++;
#endif
    {
        uint32_t tr = c->t1 | c->t0;
        if (tr) { c->flow = 0; c->excepted = 0; }
        t[c->op](c);
        c->mmu_regs_valid = 0;
        if (tr && !c->excepted && (c->t1 || (c->t0 && c->flow)))
            exception(c, V_TRACE, c->pc);
    }
#ifdef AC090_TRACE
    if (c->pc < c->op_pc || c->pc > c->op_pc + 10) {
        unsigned k = c->jump_pos++ & 1023;
        c->jump_from[k] = c->op_pc; c->jump_to[k] = c->pc; c->jump_op[k] = c->op;
    }
#endif
    c->instructions++;
    if (c->pace1024) { uint64_t waits = c->bus_waited - waited_from, own = (c->clock - paced_from - waits) * c->pace1024 + c->pace_rem; c->clock = paced_from + (own >> 10) + waits; c->pace_rem = (uint32_t)(own & 1023); }
    return 0;
}

/* One instruction at c->pc, for translated code: no interrupt check, no
 * trace. Sets c->flow if it jumped or changed SR. */
uint32_t ac090_inline_ops[65536];      /* instructions run by ac090_exec_inline, by opcode */
void ac090_exec_inline(ac090 *c) {
    handler *volatile t = TABLE(c);
    /* In hyper mode real time sets the clock: translated code adds no cycles
     * of its own, and neither does an instruction it runs here. Charged at
     * 68020 rates, an FPU memory operand or FBcc put the clock up to 36
     * cycles ahead of real time each, and the runtime slept it off (FPU
     * instructions at 0.7-6 us on OS 3.2.3, 5 Oct 2026). Bus waits stay. */
    volatile uint64_t clock_from = c->clock, waited_from = c->bus_waited;
    c->flow = 0; c->excepted = 0;
    if (setjmp(c->abort)) { if (c->jit_wall) c->clock = clock_from + (c->bus_waited - waited_from); return; }
    if (c->mmu_on | c->berr_regs) { memcpy(c->mmu_regs, c->d, sizeof c->mmu_regs); c->mmu_regs_valid = 1; }   /* an access fault restarts the instruction */
    c->op_pc = c->pc;
    c->op = (uint16_t)fetch16(c);
    ac090_inline_ops[c->op]++;
    c->clock += clk020[c->op];
#ifdef AC090_TRACE
    c->trace_pc[c->trace_pos & 8191] = c->op_pc; c->trace_op[c->trace_pos & 8191] = c->op; c->trace_a7[c->trace_pos & 8191] = c->a[7]; c->trace_pos++;
#endif
    t[c->op](c);
    c->mmu_regs_valid = 0;
#ifdef AC090_TRACE
    if (c->pc < c->op_pc || c->pc > c->op_pc + 10) {
        unsigned k = c->jump_pos++ & 1023;
        c->jump_from[k] = c->op_pc; c->jump_to[k] = c->pc; c->jump_op[k] = c->op;
    }
#endif
    c->instructions++;
    if (c->jit_wall) c->clock = clock_from + (c->bus_waited - waited_from);
}

void *ac090_handler(const ac090 *c, uint16_t op) {
    handler *t = TABLE(c);
    return (void *)t[op];
}

int ac090_legal(const ac090 *c, uint16_t op) {
    handler *t = TABLE(c);
    return t[op] != illegal;
}

/* For the board and debugging: physical addresses, never translated (they
 * are called outside the core's fault handling). */
uint32_t ac090_read(ac090 *c, uint32_t addr, int size) {
    return size == 1 ? prd8(c, addr) : size == 2 ? prd16(c, addr) : prd32(c, addr);
}
void ac090_write(ac090 *c, uint32_t addr, uint32_t value, int size) {
    if (size == 1) pwr8(c, addr, value); else if (size == 2) pwr16(c, addr, value); else pwr32(c, addr, value);
}

/* One run of plain instructions (run_loop, below), paced and cached as
 * constants so that each of the four kinds is its own loop: 1 when an
 * opcode word is not in a page of memory (or at an odd address) and the
 * way below must take it, 0 when the run is over. */
static inline __attribute__((always_inline)) int plain_run(ac090 *c, handler *t, const int paced, const int cached) {
    const uint32_t amask = c->addr_mask, pace = c->pace1024;
    uint32_t pg = ~0u, w = 0;
    const uint8_t *p = NULL;
    for (;;) {
        uint64_t paced_from = 0, waited_from = 0;
        if (paced) { paced_from = c->clock; waited_from = c->bus_waited; }
        uint32_t op;
        if (!cached) {
            uint32_t a = c->pc & amask;
            if (__builtin_expect((a >> 16) != pg, 0)) {      /* (run_page goes NULL only where the run then ends) */
                pg = a >> 16; p = c->bus.rpage[pg]; w = c->bus.wait[pg];
                c->run_page = p; c->run_base = pg << 16; c->run_w = w;   /* fetch16's, for the extension words */
            }
            if (__builtin_expect(!p || (a & 1), 0)) return 1;
            op = (uint32_t)p[a & 0xffff] << 8 | p[(a & 0xffff) + 1];
            c->op_pc = c->pc;
            c->pc += 2;
            c->clock += 3u + w + clk020[op];
            if (w) c->bus_waited += w;
        } else {
            c->op_pc = c->pc;
            op = fetch_cached(c, c->pc);
            c->pc += 2;
            c->clock += clk020[op];
        }
        c->op = (uint16_t)op;
        int last = op_recheck[op];
        if (last) c->run_page = NULL;           /* it may change what the run assumes: its words the general way */
        t[op](c);
        if (paced) { uint64_t waits = c->bus_waited - waited_from, own = (c->clock - paced_from - waits) * pace + c->pace_rem; c->clock = paced_from + (own >> 10) + waits; c->pace_rem = (uint32_t)(own & 1023); }
        c->instructions++;
        if (c->clock >= c->run_end || c->halted || last || c->recheck) return 0;
    }
}

/* The loop proper, apart from ac090_run, which holds the setjmp: a
 * function that calls setjmp keeps its locals in memory, so c and the
 * table were reloaded from the stack all through the loop. A fault
 * longjmps back to ac090_run, which calls this again. */
/* The debugger: stop before the instruction at c->pc? */
static int dbg_hit(ac090 *c) {
    if (c->dbg_stop) return 1;
    uint32_t pc = c->pc;
    if (pc == c->dbg_skip) { c->dbg_skip = 1; return 0; }
    int why = 0;
    if (pc == c->dbg_temp) { why = AC090_DBG_TEMP; c->dbg_temp = 1; }
    for (int i = 0; i < c->dbg_nbp && !why; i++) if (c->dbg_bp[i] == pc) why = AC090_DBG_BP;
    if (!why) return 0;
    c->dbg_stop = 1;
    c->dbg_why = why;
    return 1;
}

static __attribute__((noinline)) void run_loop(ac090 *c, handler *t) {
    while (c->clock < c->run_end && !c->halted) {
#ifndef AC090_TRACE
        /* Nothing special (no interrupt to take, not stopped, no MMU or
         * bus-error registers, no prefetch from RESET, no trace): a run of
         * instructions, checking none of that again until an instruction
         * may have changed it: one that op_recheck names (SR, MOVEC, STOP,
         * RESET, line A/F...), or anything that sets c->recheck (an
         * exception, a device access, an interrupt raised). Timing accurate
         * (pace1024) and the 68020's I-cache model, which neither changes
         * but through those, are fixed for the run:
         *  - without the cache model, each opcode word comes from the code
         *    page, kept with its wait through the run (the board changes
         *    its page table and waits only between runs or in a device
         *    access), with one clock update for its bus cycle, its wait and
         *    its own clocks, the sum the way below makes one by one; the
         *    extension words come the same way (fetch16, run_page). An
         *    opcode word outside a page of memory, or at an odd address,
         *    goes the way below;
         *  - with it, every word goes through fetch_cached, as below;
         *  - paced, each instruction's own cycles are scaled exactly as
         *    below, its bus waits not. */
        if (!(((c->irq > (int)c->ipl) | (c->irq == 7)) | c->stopped | c->mmu_on | c->berr_regs | c->pf_len | c->t1 | c->t0 | c->dbg_on)) {
            c->recheck = 0;
            const int paced = c->pace1024 != 0, cached = (c->cacr & 1) && !c->jit_on;
            int general = cached ? (paced ? plain_run(c, t, 1, 1) : plain_run(c, t, 0, 1))
                                 : (paced ? plain_run(c, t, 1, 0) : plain_run(c, t, 0, 0));
            c->run_page = NULL;
            if (!general) continue;
        }
#endif
        if (c->irq) take_irq(c);
        if (c->stopped) { c->clock = c->run_end; break; }
        if (c->dbg_on && dbg_hit(c)) break;
        if (c->mmu_on | c->berr_regs) { memcpy(c->mmu_regs, c->d, sizeof c->mmu_regs); c->mmu_regs_valid = 1; }   /* an access fault restarts the instruction */
        c->op_pc = c->pc;
        uint64_t paced_from = c->clock, waited_from = c->bus_waited;
        c->op = (uint16_t)fetch16(c);
        c->clock += clk020[c->op];
#ifdef AC090_TRACE
        c->trace_pc[c->trace_pos & 8191] = c->op_pc; c->trace_op[c->trace_pos & 8191] = c->op; c->trace_a7[c->trace_pos & 8191] = c->a[7]; c->trace_pos++;
#endif
        {
            /* Trace: T1 traces every instruction, T0 (68020) only a change
             * of flow. An instruction that took an exception is not traced. */
            uint32_t tr = c->t1 | c->t0;
            if (tr) { c->flow = 0; c->excepted = 0; }
            t[c->op](c);
            c->mmu_regs_valid = 0;
            if (tr && !c->excepted && (c->t1 || (c->t0 && c->flow)))
                exception(c, V_TRACE, c->pc);
        }
#ifdef AC090_TRACE
        if (c->pc < c->op_pc || c->pc > c->op_pc + 10) {
            unsigned k = c->jump_pos++ & 1023;
            c->jump_from[k] = c->op_pc; c->jump_to[k] = c->pc; c->jump_op[k] = c->op;
        }
#endif
        if (c->pace1024) { uint64_t waits = c->bus_waited - waited_from, own = (c->clock - paced_from - waits) * c->pace1024 + c->pace_rem; c->clock = paced_from + (own >> 10) + waits; c->pace_rem = (uint32_t)(own & 1023); }
        c->instructions++;
        if (c->dbg_steps && !--c->dbg_steps) { c->dbg_stop = 1; c->dbg_why = AC090_DBG_STEP; break; }
    }
}

uint64_t ac090_run(ac090 *c, uint64_t clocks) {
    uint64_t start = c->instructions;
    if (c->dbg_on && c->dbg_stop) return 0;  /* stopped in the debugger: not even an interrupt taken */
    c->run_end = c->clock + clocks;
    if (setjmp(c->abort)) {
        c->run_page = NULL;                  /* a fault out of a run */
        if (c->fault) { c->halted = 1; return c->instructions - start; }
    }
    run_loop(c, TABLE(c));
    return c->instructions - start;
}
