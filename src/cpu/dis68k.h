/*
 * A 68000 to 68040 disassembler, with the 68040's FPU and MMU instructions,
 * for openql's debugger. Motorola syntax in lower case (move.l (a0)+,d1),
 * hex numbers with $; a branch or a PC-relative operand shows the address
 * it reaches ($00012344(pc)).
 */
#ifndef DIS68K_H
#define DIS68K_H

#include <stddef.h>
#include <stdint.h>

/* The instruction stream: the word at addr (even). */
typedef uint16_t (*dis68k_read16)(void *ctx, uint32_t addr);

enum { DIS68K_68000 = 0, DIS68K_68020 = 1, DIS68K_68040 = 2 };   /* as ac090's models */

/* The instruction at pc, as text in out (at most outsz bytes with its 0);
 * returns its length in bytes, 2 or more. On the 68000 the 68020's and
 * 68040's own instructions and addressing modes come out as dc.w. */
int dis68k(uint32_t pc, dis68k_read16 rd, void *ctx, int model, char *out, size_t outsz);

/* What the instruction at pc is, for "step over": 1 a call that returns
 * to the next instruction (bsr, jsr, trap, trapv, trapcc, chk, chk2, the
 * line A and F traps the guest's system takes and returns from, dbcc,
 * fdbcc), else 0. */
int dis68k_is_call(uint32_t pc, dis68k_read16 rd, void *ctx, int model);

#endif
