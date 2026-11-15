/*
 * AC090 in C. Not a Motorola part and not an Apollo core.
 *
 * One interpreter, built natively for the x86 board and as Wasm for the
 * browser board from the same source. Opcodes dispatch through a 64K table
 * built once. Memory goes through a 64 KB page table: a page that is plain
 * RAM or ROM is read directly, anything else goes to the board.
 *
 * Time is kept in CPU clocks (c->clock). The board schedules its chipset on
 * that clock, not on an instruction count.
 */
#ifndef AC090_H
#define AC090_H

#include <stdint.h>
#include <setjmp.h>

enum ac090_model { AC090_68000 = 0, AC090_68020 = 1, AC090_68040 = 2 };
/* ac090.clr_n: the guest's cache operations, by kind. */
enum { AC090_CLR_IC_ALL,     /* CINVA/CPUSHA of the instruction cache: every translation checked (cache_epoch) */
       AC090_CLR_IC_LINE,    /* CINVL/CPUSHL of it: the line's (clr_lo, clr_hi) */
       AC090_CLR_IC_PAGE,    /* CINVP/CPUSHP of it: the page's */
       AC090_CLR_DC,         /* any of them of the data cache alone: translations untouched */
       AC090_CLR_CACR,       /* 68040 CACR writes that turned IE on or off (cache_epoch) */
       AC090_CLR_CACR_SAME,  /* ones that left IE as it was: translations untouched */
       AC090_CLR_MMU,        /* TC, URP or SRP loads that changed the tables seen (cache_epoch) */
       AC090_CLR_MMU_SAME }; /* ones that loaded the same tables again: translations untouched */

/* The FPU's registers. On x86 Linux (glibc) long double is the x87's 80-bit
 * extended format, the 68881's own less its 16 unused bits, and FP0-FP7
 * are held in it. Elsewhere they are held as the 80 bits themselves and
 * fpu_soft.c does the arithmetic in integers: on ARM64 (Apple silicon,
 * where long double is binary64, and Linux, where it is IEEE quad), and on
 * x86 macOS and Windows too, whose C libraries round FMOD, FINT, FSIN and
 * the rest differently from glibc's, so that every build gives Linux's bits.
 * -DAC090_FPU_SOFT=0 or 1 chooses either way (the tests build both). */
#ifndef AC090_FPU_SOFT
#if (defined(__x86_64__) || defined(__i386__)) && defined(__GLIBC__)
#define AC090_FPU_SOFT 0
#else
#define AC090_FPU_SOFT 1
#endif
#endif
typedef struct ac090_fx80 { uint64_t m; uint16_t se; } ac090_fx80;   /* significand; sign and exponent */
#if AC090_FPU_SOFT
typedef ac090_fx80 ac090_fpreg;
#else
typedef long double ac090_fpreg;
#endif

typedef struct ac090_bus {
    /* 64 KB pages over the address space (256 cover a 24-bit CPU, all 65536
     * a 32-bit one). A NULL entry sends the access to read/write below.
     * wpage is NULL for ROM and chipset pages. */
    uint8_t *rpage[65536];
    uint8_t *wpage[65536];
    /* Extra CPU clocks each access to a direct page costs beyond a zero-wait
     * bus cycle. The board sets these (chip RAM, ROM) and may change them
     * as DMA load changes. Accesses through read/write add their own. */
    uint8_t wait[65536];
    void *opaque;
    uint32_t (*read)(void *opaque, uint32_t addr, int size);
    void (*write)(void *opaque, uint32_t addr, uint32_t value, int size);
    /* Set: a write to a read-only direct page (rpage without wpage) goes to
     * write above instead of being dropped as a write to ROM. */
    int rom_writes;
} ac090_bus;

/* A device the JIT calls straight (SPD-9): a read or a write at one fixed
 * physical address, done as the bus functions do it, with the access's wait
 * states and ac090_end_slice once the clock reaches run_end. */
typedef uint32_t (*ac090_io_rd)(void *opaque, uint32_t pa);
typedef void (*ac090_io_wr)(void *opaque, uint32_t pa, uint32_t v);

typedef struct ac090 {
    uint32_t d[8];
    uint32_t a[8];
    uint32_t pc;
    uint32_t usp, isp, msp;
    uint32_t x, n, z, v, c;      /* each 0 or 1 */
    uint32_t s, m, t1, t0, ipl;  /* SR system byte */
    uint32_t vbr, sfc, dfc, cacr, caar;
    uint32_t tc, itt0, itt1, dtt0, dtt1, mmusr, urp, srp;   /* 68040 MMU registers (translation: ac090_mmu.c) */
    ac090_fpreg fp[8];           /* FPU (68040 only): FP0-FP7, extended */
    uint32_t fpcr, fpsr, fpiar;
    ac090_fpreg fp_tmp;          /* scratch for translated FPU code */
    uint32_t addr_mask;          /* 0x00ffffff on the EC020 and 68000 */
    int model;
    int stopped;
    int irq;                     /* pending interrupt level, 0 = none */
    uint64_t clock;              /* CPU clocks since reset */
    uint64_t run_end;            /* ac090_run stops once clock passes this; the
                                    board may lower it to end a slice early */
    uint64_t instructions;
    uint32_t op_pc;              /* address of the instruction being run */
    uint16_t op;
    int fault;                   /* 68000 address error taken this step */
    int flow;                    /* this instruction changed the flow (T0 trace) */
    int excepted;                /* this instruction took an exception */
    /* 68020 instruction cache: 64 longwords, direct mapped on A7..A2, tagged
     * with A23..A8 and FC2 (supervisor). */
    uint32_t icache_tag[64];
    uint32_t icache_data[64];
    uint8_t icache_valid[64];
    uint64_t icache_hits, icache_misses;
    int halted;                  /* double fault */
#ifdef AC090_TRACE
    uint32_t trace_pc[8192];       /* last instructions, for debugging only */
    uint16_t trace_op[8192];
    uint32_t trace_a7[8192];
    unsigned trace_pos;
    int trace_vector;            /* print the trace when this vector is taken */
    uint32_t jump_from[1024], jump_to[1024];
    uint16_t jump_op[1024];
    unsigned jump_pos;
#endif
    jmp_buf abort;
    ac090_bus bus;

    /* JIT (jit.c). budget counts down in instructions inside translated
     * code, where it lives in a host register; it is in this struct
     * whenever C code runs. jit_on routes ac090_run through the translator. */
    int32_t budget;              /* when to leave translated code; end early with ac090_end_slice */
    int32_t budget_lost;         /* budget given up by ac090_end_slice, not run */
    uint64_t jit_icount;         /* instructions run in translated code */
    uint32_t cache_epoch;        /* bumped when software clears the caches */
    /* A CINVL/CINVP/CPUSHL/CPUSHP of the instruction cache: the lines it
     * names, gathered as one range [clr_lo, clr_hi] (inclusive) until the
     * JIT next looks. Only translations meeting it are checked, not every
     * one as a CINVA/CPUSHA (cache_epoch) has them. The address is physical. */
    int clr_pending;
    uint32_t clr_lo, clr_hi;
    /* What the guest did to the caches, by kind (AC090_CLR_*), since the start */
    uint64_t clr_n[8];
    uint64_t jit_rsp;            /* host stack at entry, restored on exit */
    uint64_t jit_ret_miss;       /* RTS whose PC or A7 did not match the call frame */
    uint64_t irq_taken[8];       /* interrupts taken, by level */
    uint32_t exit_why;           /* JIT: why translated code last went back to the dispatcher */
    /* 68040 MMU (ac090_mmu.c). mmu_on is TC E; mmu_identity says
     * translated code, which uses guest addresses as they are, still sees
     * the right memory: every valid page maps to itself, or has no host
     * memory behind it (page_backed says), so translated code reaching it
     * faults into the slow path, which translates (stage B). Otherwise the
     * machine runs in the interpreter. The ATC holds 64 pages each for data
     * and program. */
    int mmu_on, mmu_identity;
    uint32_t mmu_blank_pages;    /* remapped pages with no host memory behind them, at the last look */
    uint64_t mmu_slow, mmu_dropped;   /* JIT slow-path accesses translated; ones the tables refused (read open bus, write dropped) */
    int (*page_backed)(uint32_t la);  /* the JIT: host memory translated code reaches directly at la: 0 none, 1 RAM, 2 ROM (read-only). NULL: RAM everywhere */
    const uint32_t *backed_gen;  /* the JIT: bumped whenever page_backed's answer may change (NULL: never) */
    int (*range_backed)(uint32_t la, uint32_t size);   /* the JIT: page_backed's strictest answer over a range, at once (NULL: page by page) */
    /* The tables as the last full look at them saw them (mmu_jit_ok), so a
     * PFLUSH that changed nothing need not look at 4 GB again: OS 3.2's
     * 68040.library flushes several times in each CachePreDMA. */
    uint8_t *mmu_snap;           /* records: u32 address, u32 length, the bytes */
    uint32_t mmu_snap_len, mmu_snap_cap;
    int mmu_snap_ok;             /* the snapshot covers every table the result depends on (a look cut short: what it read) */
    uint32_t mmu_snap_tc, mmu_snap_urp, mmu_snap_srp, mmu_snap_gen;
    void *mmu_tables;            /* the tables one look has met (ac090_mmu.c, mmu_table): each once in the snapshot */
    uint32_t mmu_tables_n;
    uint64_t mmu_jit_walks, mmu_jit_walks_saved;   /* full looks, and PFLUSHes that needed none */
    int mmu_fc;                  /* MOVES: SFC or DFC for the access in progress, else 0 */
    int mmu_in_fault;            /* stacking an access fault: another is a double fault */
    uint32_t mmu_regs[16];       /* D0-D7/A0-A7 as the instruction began: restored on an access fault, which restarts it */
    int mmu_regs_valid;          /* an instruction is in progress (not an interrupt or exception being stacked) */
    uint32_t atc_key[2][64];     /* logical page << 1 | S, + 1 (0: empty) */
    uint32_t atc_pa[2][64];      /* physical page base */
    uint8_t atc_bits[2][64];     /* 1 write protected, 2 supervisor only, 4 modified, 8 global */
    uint64_t mmu_walks, mmu_faults;
    uint32_t mmu_last_fa, mmu_last_pc, mmu_last_ssw;   /* the last access fault, for reports */
    /* JIT verification: RAM writes logged byte by byte (address, old
     * value) so they can be undone; with probe set, a hardware access
     * aborts the instruction before it happens. */
    int wlog_on, probe, probe_hit;
    uint32_t probe_io_lo, probe_io_hi;   /* while probing, hardware here may be read and written, */
    uint32_t probe_io_deny[4];           /* except these register addresses (0 = none) */
    /* or the board decides: 1 if the access may happen while probing; and
     * saves/restores whatever such accesses change */
    void (*reset_hook)(void *opaque);   /* RESET: the board's /RST line; the CPU goes on */
    /* RESET: the words after it, already in the pipeline when /RST takes
     * the board's memory away. Exec's ColdReboot runs RESET, then a JMP
     * into the ROM that must still be fetched. */
    uint32_t pf_base; uint16_t pf_word[8]; int pf_len;
    int (*probe_io)(void *opaque, uint32_t a, int size, int write);
    void (*probe_save)(void *opaque);
    void (*probe_restore)(void *opaque);
    uint32_t wlog_n, wlog_cap;
    uint32_t *wlog_addr;
    uint8_t *wlog_old;
    int jit_handback;            /* translated code handed this instruction back */
    void (*fault_hook)(struct ac090 *c, int vector, uint32_t pc);
    void (*irq_hook)(struct ac090 *c, int level, uint32_t pc);    /* an interrupt taken (tools: the board's frame trace) */
    void (*block_hook)(struct ac090 *c);   /* JIT_HIST: called as the dispatcher enters each block */   /* called as a bus/address error, illegal or line-A/F exception starts */
    int jit_on;
    int jit_wall;                /* 1: the clock follows the host's real time */
    uint64_t wall_ref_clock;     /* clock and host time at the last re-anchor */
    uint64_t wall_ref_ns;
    uint64_t jit_blocks_run, jit_fallbacks, jit_compiled;
    /* Last, so the hot fields above keep their places (two fields inserted
     * after clock cost hyper mode 2.8% of its instructions, 2 Oct 2026). */
    uint32_t pace1024;           /* timing accurate: each instruction's own cycles scaled by this /1024 (the board's clock over the CPU's chosen clock); 0 as counted */
    uint32_t pace_rem;           /* the scaled cycles' fraction (/1024) carried to the next instruction: above the board's clock a 2-cycle instruction is under one board clock, and rounded down it cost nothing */
    uint64_t bus_waited;         /* the bus waits in the clock so far: the chipset's time, never scaled by the pace */
    uint64_t wall_jumps, wall_jump_max;   /* hyper mode: slices after which real time had run on more than 5 ms (a host stall); the longest, in clocks.
                                             Here, not in jit_stats: growing that moved the JIT's tables and cost hyper mode 3.5% */
    /* SPD-9: the board's function for a read or write of this size at pa, or
     * NULL. Asked as a block is translated, for an absolute address on a page
     * with no host memory, so only a device that never moves may answer (the
     * CIAs, the custom chips). Translated code calls it only with the MMU off
     * and no probe; otherwise the access goes the general way. NULL hooks:
     * never. */
    ac090_io_rd (*io_read_fn)(void *opaque, uint32_t pa, int size);
    ac090_io_wr (*io_write_fn)(void *opaque, uint32_t pa, int size);
    /* SPD-19, phase 1: tables that move pages with host memory behind them
     * (MuFastROM's ROM in Fast RAM, MuFastZero's page zero) may run
     * translated, through a view of guest RAM the host's own MMU translates
     * (jit.c, the view; AC090_HOST_MMU_DESIGN.md). mmu_can_view: the JIT can
     * build one (set by jit_init). mmu_view: the tables need one; the pages
     * it moves are in mmu_remap, three longs each (logical page, physical
     * page, 1 if write protected), and mmu_view_gen changes with them. */
    int mmu_can_view, mmu_view;
    char mmu_why_buf[64];
    const char *mmu_why;         /* why the last look left the tables to the interpreter (NULL: it did not), for the runtime's mmu line */
    uint32_t mmu_view_gen;
    uint32_t *mmu_remap, mmu_remap_n, mmu_remap_cap;
    uint32_t *mmu_remap_shown, mmu_remap_shown_n;   /* the moves the view was last asked for: the same again needs no new view */
    /* Device pages translated code may reach without the tables (Lupus
     * Reditus' custom-chip polling spent 38% of the host in the MMU's slow
     * path): a bit per 4K page, set when both trees (or a DTT) map it to
     * itself for user and supervisor alike; mmu_io_ok's first AC090_IO_WORDS
     * for reads, the next for writes (not write protected). A page the tables
     * (not a DTT) decide is only a candidate at first: its first access walks
     * the current mode's tree as the 68040 would, setting U, and M for a
     * write, and the page goes fast once each mode has walked it or a DTT
     * passes it (ac090_mmu_io_touch); PFLUSH takes it back for the modes and
     * pages it flushes. The layout is IO_* in ac090_mmu.c. mmu_io_tree keeps each
     * tree's own answer (user read, user write, supervisor read and write)
     * so a MOVEC to DTT0/1 combines them again without a walk. Allocated
     * when the MMU first comes on, then kept: translated code holds its
     * address. */
    uint32_t *mmu_io_ok, *mmu_io_tree;
#define AC090_IO_WORDS (1u << 15)   /* a bitmap's words: 1M pages of 4K */
    /* The task guard (the board's taskguard.c): asked as an address error,
     * illegal instruction or line A exception starts (never a privilege
     * violation: exec's Supervisor() works by taking one, nor line F, which
     * 68040.library uses for its FPU emulation).
     * Nonzero: the guard has dealt with it (set PC and the registers
     * itself) and nothing is stacked. NULL: never asked. */
    int (*guard_hook)(struct ac090 *c, int vector, uint32_t pc);
    /* Adaptive stacks (docs/design/Design-Adaptive-Stack.md). Translated
     * code checks A7 at the start of every block that lowers it: with the
     * block's deepest push d, A7 - d must lie in [stk_lo, stk_lo + stk_span),
     * else stack_hook(c, d) runs first. 0 from the hook: the block runs; 1:
     * the hook moved the CPU on (c->pc), and the block does not run. The
     * window is everything (0, $FFFFFFFF) while nobody watches; with a hook,
     * load_sp makes it everything in supervisor mode and empty (span 0) on
     * the way back to user mode, so the first such block asks again.
     * stack_watched: asked as a block is translated, 1 if stack_entry(c, pc)
     * must run as it starts (a program's first instruction, which the board
     * may move onto a stack of its own). stack_fault: an access translated
     * code made where no host memory is; 1 if the board put memory there
     * (a stack's next chunk), and the access runs again. NULL hooks: never.
     * A block may check deeper than its own pushes (a routine it calls, the
     * code ahead): only what the block itself pushes is certain to be
     * written. So the JIT asks about the deeper part softly first: with
     * stk_soft set, the hook never stops the program; where it would, it
     * returns 2 and changes nothing. The JIT then asks about the block's own
     * pushes as before, and runs that block alone (the next one is entered
     * at its check). A board sets stk_soft_ok when its hook knows stk_soft;
     * with other boards the JIT asks only about the block's own pushes. */
    uint32_t stk_lo, stk_span;
    int (*stack_hook)(struct ac090 *c, uint32_t descent);
    int (*stack_watched)(struct ac090 *c, uint32_t pc);
    int (*stack_entry)(struct ac090 *c, uint32_t pc);
    int (*stack_fault)(struct ac090 *c, uint32_t a, int size, int write);
    uint64_t stk_slow, stk_entries, stk_faults;   /* hook calls: the check, program entries, accesses below the committed stack */
    uint64_t stk_alone;                           /* blocks run alone: their check passed only for their own pushes */
    uint8_t stk_soft, stk_soft_ok;
    /* The JIT's slow path under the MMU (ac090_mmu_probe, jit.c xc_hit):
     * 4K pages whose last look was an ATC hit (or a DTT match) that would
     * hit again with no side effect, by write and page, tag (page << 1 | S)
     * + 1. An entry counts only while its gen is mmu_xgen, which moves on
     * whenever the ATC, the DTTs or the device-page candidates change, so a
     * hit here is what mmu_lookup would answer, without its look. Kept
     * only while mmu_xc_on (jit_init sets it; JIT_NOMMUCACHE=1 clears it). */
    struct { uint32_t tag, gen, pa; } mmu_xc[2][128];
    uint32_t mmu_xgen;
    int mmu_xc_on;
    /* A board whose bus answers some addresses with a bus error (the Falcon:
     * EmuTOS finds its hardware by reading and catching them) sets berr_regs,
     * so the registers are kept as each instruction begins, as with the MMU
     * on, and calls ac090_bus_error from its read or write function. */
    int berr_regs;
    /* Interrupt acknowledge: the vector for an interrupt at this level being
     * taken (an MFP's vectored interrupt), or -1 for the autovector. c->irq
     * is 0 while it runs, so the board may raise the next level pending
     * there. NULL: always the autovector. */
    int (*iack_hook)(struct ac090 *c, int level);
    /* Task cores (docs/design/Design-Exec-Across-Cores.md, phase 1). Kept at
     * the end: translated code's offsets into this struct do not move.
     * block_gate: asked by the JIT's dispatcher before it translates a block
     * (never for a block it has already), and by its look-ahead; 1: code at
     * pc must not run on this CPU now. The dispatcher then stops with
     * gate_stop set and c->pc at that block, the registers exact.
     * exc_hook: asked first by every exception, before anything is stacked;
     * nonzero: the exception is not taken here (the hook stops the CPU and
     * the instruction runs again elsewhere). NULL hooks: never. */
    int (*block_gate)(struct ac090 *c, uint32_t pc);
    int gate_stop;
    int (*exc_hook)(struct ac090 *c, int vector, uint32_t pc);
    /* Task cores, step 4 (native library bodies on the calling core):
     * asked by the dispatcher before block_gate, for a block it has not
     * translated (never by the look-ahead). 1: the hook took the call there
     * itself (a library's jump-table entry, run as its native body: the
     * results in the registers, the return address popped, c->pc the
     * caller's next instruction); 0: nothing changed. NULL: never. */
    int (*call_hook)(struct ac090 *c, uint32_t pc);
    /* Set by a board whose call_hook counts the calls it took there: a
     * library call's stub on a task core then runs the call's native body
     * straight from translated code (jit_magic_core_fast), the hook not
     * asked, and counts it here (only this CPU's thread writes it). NULL:
     * every call asks the hook. */
    uint64_t *call_count;
#define AC090_CALL_COUNT 1                 /* (for boards built against an ac090.h without it) */
    /* The interpreter's run of plain instructions (ac090_run): set by
     * anything that may change what the loop checks before each
     * instruction (an exception, a device access, an interrupt raised), so
     * the loop checks again. At the end: the hot fields keep their places. */
    int recheck;
    /* In such a run: the code page in host memory (NULL outside a run),
     * where it starts (masked) and its wait, so that fetch16 takes an
     * extension word without the checks the run has made. */
    const uint8_t *run_page;
    uint32_t run_base, run_w;
} ac090;

void ac090_init(ac090 *c, int model);
void ac090_reset(ac090 *c);               /* SSP and PC from address 0 and 4 */
int ac090_step(ac090 *c);                  /* one instruction; 0 ok, 1 stopped, 2 halted */
/* Make translated code leave at its next block. */
static inline void ac090_end_slice(ac090 *c) { if (c->budget > 0) c->budget_lost += c->budget; c->budget = 0; }
uint64_t ac090_run(ac090 *c, uint64_t clocks); /* until c->clock passes the budget */
void ac090_set_irq(ac090 *c, int level);

uint16_t ac090_get_sr(const ac090 *c);

/* Memory access with the bus's page table and waits, as the interpreter
 * does it; the JIT calls these for accesses it cannot make directly. */
uint32_t ac090_read(ac090 *c, uint32_t addr, int size);
/* 1 if the opcode decodes to an instruction on this model. */
/* The interpreter's handler for op (called by translated code for some
 * register-only instructions). */
void *ac090_handler(const ac090 *c, uint16_t op);
int ac090_legal(const ac090 *c, uint16_t op);
int ac090_fpu_inline_ok(const ac090 *c, uint16_t op, uint16_t ext);
int ac090_fpu_mem_ok(const ac090 *c, uint16_t op, uint16_t ext, int *bytes);
void ac090_fpu_rr(ac090 *c, uint32_t ext, uint32_t pc);
void ac090_fpu_special_rr(ac090 *c, uint32_t ext, uint32_t pc);   /* FPm to FPn with a NaN or infinite operand, from the bits (x87 hosts) */
void ac090_fpu_tmp_op(ac090 *c, uint32_t ext, uint32_t pc);
void ac090_fpu_tmp_store(ac090 *c, uint32_t ext, uint32_t pc);
void ac090_fpu_x_out(ac090 *c, uint32_t fp);
void ac090_fpu_x_in(ac090 *c, uint32_t fp);
int ac090_fpu_on(const ac090 *c);
int ac090_fpu_traced(void);
int ac090_fpu_cond_cc(uint32_t fpsr, int p);
/* Run the instruction at c->pc through the interpreter, for translated
 * code; c->flow says it jumped or changed SR. */
void ac090_exec_inline(ac090 *c);
void ac090_write(ac090 *c, uint32_t addr, uint32_t value, int size);
void ac090_set_sr(ac090 *c, uint16_t sr);

/* The 68040 MMU's data translation for the current mode, without taking an
 * exception: 0 and the physical address in *pa, or nonzero when the tables
 * refuse the access (invalid, write protected, supervisor only). The
 * descriptors' U and M bits and the ATC are updated as by a real access. */
int ac090_mmu_probe(ac090 *c, uint32_t a, int write, uint32_t *pa);
void ac090_mmu_io_touch(ac090 *c, uint32_t a, int write);
/* Translation off (TC E clear), as when the system bus resets: the tables
 * may live in memory the reset takes away. */
void ac090_mmu_off(ac090 *c);
/* A bus error on the access in progress, from the board's read or write
 * function (berr_regs set): the instruction is abandoned with its registers
 * as it found them and a 68040 access error frame (format 7, vector 2) is
 * stacked, as mmu_access_fault does without the ATC bit. Does not return. */
void ac090_bus_error(ac090 *c, uint32_t addr, int write, int size);

/* Task cores: TAS, CAS and CAS2 as the host's atomic operations, so that a
 * guest lock taken on one core holds against another (Design-Exec-Across-
 * Cores, phase 1). Off (0) by default: the instructions as before. With it
 * on, an aligned operand in host RAM is changed with one host atomic
 * operation (TAS a fetch-or, CAS a compare-and-exchange); CAS2, and a CAS
 * operand that is not aligned to its size, take the host lock kept for that
 * guest address. Process-wide: every CPU on the host sees the same. */
extern int ac090_atomic;
/* The host's locks, kept by guest address (never in a guest structure: a
 * 68k MsgPort or SignalSemaphore has no room for one). One lock covers
 * every address of a 16-byte line; lines share a fixed table of locks. */
void ac090_hostlock(uint32_t addr);
void ac090_hostunlock(uint32_t addr);
uint64_t ac090_atomic_ops(void);          /* TAS, CAS and CAS2 done the atomic way so far */

/* instructions run by ac090_exec_inline, by opcode (the runtime's 5-second report) */
extern uint32_t ac090_inline_ops[65536];

#endif
