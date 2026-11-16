/* openql's debugger, apart from its window (debug.h). */
#include "debug.h"
#include "cpu/dis68k.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int dbg_stopped(const ac090 *c) { return c->dbg_on && c->dbg_stop; }

const char *dbg_why(const ac090 *c) {
    if (c->halted) return "The CPU halted (a double fault): reset the machine";
    if (!dbg_stopped(c)) return "Running";
    switch (c->dbg_why) {
    case AC090_DBG_BP: return "Stopped at a breakpoint";
    case AC090_DBG_STEP: return "Stepped";
    case AC090_DBG_TEMP: return "Stopped where asked";
    default: return "Stopped";
    }
}

static int any_bp(const ac090 *c) { return c->dbg_nbp > 0 || !(c->dbg_temp & 1); }

void dbg_break(ac090 *c) {
    c->dbg_on = 1;
    if (!c->dbg_stop) { c->dbg_stop = 1; c->dbg_why = AC090_DBG_BREAK; }
    c->dbg_steps = 0;
}

/* Run on from where the CPU is, its own breakpoint passed once. */
static void go(ac090 *c) {
    c->dbg_skip = c->pc;
    c->dbg_stop = 0;
    c->dbg_why = AC090_DBG_NONE;
    c->dbg_on = any_bp(c) || c->dbg_steps;
}

void dbg_continue(ac090 *c) { c->dbg_steps = 0; go(c); }
void dbg_step(ac090 *c) { c->dbg_steps = 1; go(c); }
void dbg_run_to(ac090 *c, uint32_t addr) { c->dbg_temp = addr & ~1u; c->dbg_steps = 0; go(c); }

/* the code's words: through the MMU's program side */
static uint16_t rd16(void *ctx, uint32_t a) {
    const ac090 *c = ctx;
    uint32_t pa;
    if (ac090_mmu_peek(c, a, 1, &pa)) return 0xffff;
    pa &= c->addr_mask;
    const uint8_t *p = c->bus.rpage[pa >> 16];
    if (!p || (pa & 0xffff) == 0xffff) return 0xffff;
    return (uint16_t)(p[pa & 0xffff] << 8 | p[(pa & 0xffff) + 1]);
}

static int dis_model(const ac090 *c) { return c->model == AC090_68040 ? DIS68K_68040 : c->model == AC090_68020 ? DIS68K_68020 : DIS68K_68000; }

void dbg_step_over(ac090 *c) {
    char t[160];
    if (!dis68k_is_call(c->pc, rd16, (void *)c, dis_model(c))) { dbg_step(c); return; }
    dbg_run_to(c, c->pc + (uint32_t)dis68k(c->pc, rd16, (void *)c, dis_model(c), t, sizeof t));
}

void dbg_release(ac090 *c) {
    c->dbg_temp = 1;
    c->dbg_steps = 0;
    if (c->dbg_stop) go(c);
    c->dbg_on = any_bp(c);
}

int dbg_bp_at(const ac090 *c, uint32_t addr) {
    for (int i = 0; i < c->dbg_nbp; i++) if (c->dbg_bp[i] == addr) return 1;
    return 0;
}

int dbg_bp_toggle(ac090 *c, uint32_t addr) {
    addr &= ~1u;
    for (int i = 0; i < c->dbg_nbp; i++)
        if (c->dbg_bp[i] == addr) {
            c->dbg_bp[i] = c->dbg_bp[--c->dbg_nbp];
            if (!c->dbg_stop) c->dbg_on = any_bp(c);
            return 0;
        }
    if (c->dbg_nbp >= AC090_DBG_BPS) return -1;
    c->dbg_bp[c->dbg_nbp++] = addr;
    c->dbg_on = 1;
    return 1;
}

void dbg_bp_clear(ac090 *c) {
    c->dbg_nbp = 0;
    if (!c->dbg_stop) c->dbg_on = any_bp(c);
}

int dbg_peek(const ac090 *c, uint32_t addr, uint8_t *v) {
    if (ac090_mmu_peek(c, addr, 0, &addr)) return 0;
    addr &= c->addr_mask;
    const uint8_t *p = c->bus.rpage[addr >> 16];
    if (!p) return 0;
    *v = p[addr & 0xffff];
    return 1;
}

int dbg_poke(ac090 *c, uint32_t addr, uint8_t v) {
    if (ac090_mmu_peek(c, addr, 0, &addr)) return 0;
    addr &= c->addr_mask;
    uint8_t *p = c->bus.wpage[addr >> 16];
    if (!p) return 0;
    p[addr & 0xffff] = v;
    return 1;
}

int dbg_disasm(const ac090 *c, uint32_t addr, char *out, size_t outsz) {
    return dis68k(addr & ~1u, rd16, (void *)c, dis_model(c), out, outsz);
}

uint32_t dbg_back(const ac090 *c, uint32_t addr, int count) {
    char t[160];
    for (uint32_t k = 2 * (uint32_t)count; k <= 22u * (uint32_t)count; k += 2) {
        uint32_t s = addr - k, a = s, starts[64];
        int n = 0, clean = 1;
        while (a < addr && n < 64 && clean) {
            starts[n++] = a;
            int len = dis68k(a, rd16, (void *)c, dis_model(c), t, sizeof t);
            if (!strncmp(t, "dc.w", 4)) clean = 0;
            a += (uint32_t)len;
        }
        if (clean && a == addr && n >= count) return starts[n - count];
    }
    return addr - 2u * (uint32_t)count;
}

/* ---- registers ------------------------------------------------------------ */

static uint32_t *sp_slot(ac090 *c, const char *n) {
    if (!strcmp(n, "usp")) return c->s ? &c->usp : &c->a[7];
    if (!strcmp(n, "ssp") || !strcmp(n, "isp")) return c->s && !c->m ? &c->a[7] : &c->isp;
    if (!strcmp(n, "msp")) return c->s && c->m ? &c->a[7] : &c->msp;
    return NULL;
}

static int reg_name(const char *in, char *n, size_t nn) {
    size_t k = 0;
    while (*in == ' ') in++;
    while (in[k] && !isspace((unsigned char)in[k]) && k + 1 < nn) { n[k] = (char)tolower((unsigned char)in[k]); k++; }
    n[k] = 0;
    return (int)k;
}

int dbg_get_reg(const ac090 *c, const char *name, uint32_t *v) {
    char n[8];
    reg_name(name, n, sizeof n);
    if ((n[0] == 'd' || n[0] == 'a') && n[1] >= '0' && n[1] <= '7' && !n[2]) { *v = n[0] == 'd' ? c->d[n[1] - '0'] : c->a[n[1] - '0']; return 0; }
    if (!strcmp(n, "sp")) { *v = c->a[7]; return 0; }
    if (!strcmp(n, "pc")) { *v = c->pc; return 0; }
    if (!strcmp(n, "sr")) { *v = ac090_get_sr(c); return 0; }
    if (!strcmp(n, "ccr")) { *v = ac090_get_sr(c) & 0xff; return 0; }
    if (!strcmp(n, "vbr")) { *v = c->vbr; return 0; }
    if (!strcmp(n, "cacr")) { *v = c->cacr; return 0; }
    uint32_t *p = sp_slot((ac090 *)c, n);
    if (p) { *v = *p; return 0; }
    return -1;
}

int dbg_set_reg(ac090 *c, const char *name, uint32_t v) {
    char n[8];
    reg_name(name, n, sizeof n);
    if ((n[0] == 'd' || n[0] == 'a') && n[1] >= '0' && n[1] <= '7' && !n[2]) { (n[0] == 'd' ? c->d : c->a)[n[1] - '0'] = v; return 0; }
    if (!strcmp(n, "sp")) { c->a[7] = v; return 0; }
    if (!strcmp(n, "pc")) { c->pc = v & ~1u; c->dbg_skip = c->pc; c->recheck = 1; return 0; }
    if (!strcmp(n, "sr")) { ac090_set_sr(c, (uint16_t)v); c->recheck = 1; return 0; }
    if (!strcmp(n, "ccr")) { ac090_set_sr(c, (uint16_t)((ac090_get_sr(c) & 0xff00) | (v & 0x1f))); return 0; }
    if (!strcmp(n, "vbr") && c->model != AC090_68000) { c->vbr = v; return 0; }
    uint32_t *p = sp_slot(c, n);
    if (p) { *p = v; return 0; }
    return -1;
}

int dbg_parse(const ac090 *c, const char *text, uint32_t *v) {
    const char *p = text;
    while (isspace((unsigned char)*p)) p++;
    uint32_t base = 0;
    char *end;
    char n[8];
    int k = 0;
    while (p[k] && (isalnum((unsigned char)p[k])) && k < 7) { n[k] = (char)tolower((unsigned char)p[k]); k++; }
    n[k] = 0;
    uint32_t r;
    if (k && !isalnum((unsigned char)p[k]) && dbg_get_reg(c, n, &r) == 0) {
        /* a register's name ("a0" is never hex here: type $a0 for that) */
        base = r;
        p += k;
    } else {
        if (*p == '$') p++;
        else if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
        if (!isxdigit((unsigned char)*p)) return -1;
        unsigned long long x = strtoull(p, &end, 16);
        if (x > 0xffffffffull) return -1;
        base = (uint32_t)x;
        p = end;
    }
    while (isspace((unsigned char)*p)) p++;
    if (*p == '+' || *p == '-') {
        int neg = *p == '-';
        p++;
        while (isspace((unsigned char)*p)) p++;
        if (*p == '$') p++;
        else if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
        if (!isxdigit((unsigned char)*p)) return -1;
        uint32_t off = (uint32_t)strtoul(p, &end, 16);
        p = end;
        base = neg ? base - off : base + off;
    }
    while (isspace((unsigned char)*p)) p++;
    if (*p) return -1;
    *v = base;
    return 0;
}

void dbg_sr_text(uint16_t sr, char *out, size_t outsz) {
    snprintf(out, outsz, "%s %s %s I%d %c%c%c%c%c",
             sr & 0x8000 ? "T1" : sr & 0x4000 ? "T0" : "--", sr & 0x2000 ? "S" : "-", sr & 0x1000 ? "M" : "-", sr >> 8 & 7,
             sr & 0x10 ? 'X' : '-', sr & 8 ? 'N' : '-', sr & 4 ? 'Z' : '-', sr & 2 ? 'V' : '-', sr & 1 ? 'C' : '-');
}
