/*
 * openql's debugger, apart from its window: breakpoints, stepping, the
 * registers by name, memory read and written without touching the
 * machine's hardware, and the disassembler (cpu/dis68k.h), for either
 * machine's CPU. gui.c shows it; the tests drive it straight.
 *
 * The CPU stops between instructions (ac090.h, dbg_*): the board's run
 * loop then ends its slice early, and openql stops running frames until
 * the debugger lets the CPU go again.
 */
#ifndef OPENQL_DEBUG_H
#define OPENQL_DEBUG_H

#include <stddef.h>
#include "cpu/ac090.h"

int dbg_stopped(const ac090 *c);              /* 1 while the debugger holds the CPU */
const char *dbg_why(const ac090 *c);          /* why it stopped, in words */

void dbg_break(ac090 *c);                     /* stop before the next instruction */
void dbg_continue(ac090 *c);                  /* run on, to a breakpoint */
void dbg_step(ac090 *c);                      /* one instruction (an interrupt taken first counts as its handler's) */
void dbg_step_over(ac090 *c);                 /* one, or a whole call (bsr, jsr, trap, dbcc...) */
void dbg_run_to(ac090 *c, uint32_t addr);     /* run on until addr, or a breakpoint */
void dbg_release(ac090 *c);                   /* the debugger closed: run on, the breakpoints kept */

int dbg_bp_toggle(ac090 *c, uint32_t addr);   /* 1 set, 0 cleared, -1 none left */
int dbg_bp_at(const ac090 *c, uint32_t addr);
void dbg_bp_clear(ac090 *c);

/* Memory as the CPU's data accesses see it now (through its MMU, when that
 * is on), in the board's pages of RAM and ROM only: hardware registers are
 * never read or written (a read there could take a key from the
 * keyboard's queue). 0: not memory, or nothing mapped there. */
int dbg_peek(const ac090 *c, uint32_t addr, uint8_t *v);
int dbg_poke(ac090 *c, uint32_t addr, uint8_t v);   /* RAM only */

/* The instruction at addr, as text; returns its length. */
int dbg_disasm(const ac090 *c, uint32_t addr, char *out, size_t outsz);
/* The start of the instruction that ends at addr, going back count of
 * them where the code decodes cleanly (else 2 bytes each). */
uint32_t dbg_back(const ac090 *c, uint32_t addr, int count);

/* Registers by name: d0-d7, a0-a7 (sp), pc, sr, ccr, usp, ssp (isp), msp,
 * vbr, cacr. 0, or -1: no such register. */
int dbg_get_reg(const ac090 *c, const char *name, uint32_t *v);
int dbg_set_reg(ac090 *c, const char *name, uint32_t v);

/* An address as typed: hex with or without $ (or 0x), a register's name,
 * or either with +hex or -hex after it ("a6+$20"). 0, or -1. */
int dbg_parse(const ac090 *c, const char *text, uint32_t *v);

/* SR's flags as text: "T S M I7 XNZVC" with - for each flag clear. */
void dbg_sr_text(uint16_t sr, char *out, size_t outsz);

#endif
