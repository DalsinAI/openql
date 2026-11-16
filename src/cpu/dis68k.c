/* A 68000 to 68040 disassembler (dis68k.h). */
#include "dis68k.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    dis68k_read16 rd;
    void *ctx;
    uint32_t pc, p;            /* the instruction's address; the next word's */
    int model, bad;
    char *o;
    size_t n, len;
} dis;

static void put(dis *d, const char *fmt, ...) {
    if (d->len + 1 >= d->n) return;
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(d->o + d->len, d->n - d->len, fmt, ap);
    va_end(ap);
    if (k > 0) d->len += (size_t)k;
    if (d->len >= d->n) d->len = d->n - 1;
}

static uint16_t w16(dis *d) { uint16_t v = d->rd(d->ctx, d->p); d->p += 2; return v; }
static uint32_t w32(dis *d) { uint32_t v = (uint32_t)w16(d) << 16; return v | w16(d); }
static int need020(dis *d) { if (d->model < DIS68K_68020) d->bad = 1; return !d->bad; }
static int need040(dis *d) { if (d->model < DIS68K_68040) d->bad = 1; return !d->bad; }

/* a signed number in hex: $10, -$10 */
static void sx(dis *d, int32_t v) { if (v < 0) put(d, "-$%x", (unsigned)-(int64_t)v); else put(d, "$%x", (unsigned)v); }

static const char sizes[] = "bwl";

static void reg(dis *d, int r) { put(d, "%c%d", r & 8 ? 'a' : 'd', r & 7); }

/* An index register: d0.w*4 */
static void xn(dis *d, uint16_t ext) {
    reg(d, ext >> 12);
    put(d, ".%c", ext & 0x800 ? 'l' : 'w');
    int scale = ext >> 9 & 3;
    if (scale && d->model >= DIS68K_68020) put(d, "*%d", 1 << scale);   /* (the 68000 has no scale: it ignores the bits) */
}

/* The brief and full extension words of (d8,An,Xn) and (d8,PC,Xn); base is
 * "an" text or NULL for the PC, whose value at the extension is pcbase. */
#ifndef RESERVED
#define RESERVED(x) (x)
#endif
static void indexed(dis *d, int an, uint32_t pcbase) {
    uint16_t ext = w16(d);
    if (!(ext & 0x100) || d->model < DIS68K_68020) {       /* brief (the 68000 knows no other) */
        int32_t d8 = (int8_t)(ext & 0xff);
        if (an < 0) put(d, "$%08x(pc,", (unsigned)(pcbase + (uint32_t)d8));
        else { sx(d, d8); put(d, "(a%d,", an); }
        xn(d, ext);
        put(d, ")");
        return;
    }
    if (!need020(d)) return;
    int bs = ext >> 7 & 1, is = ext >> 6 & 1, bdsz = ext >> 4 & 3, iis = ext & 7;
    if (RESERVED(ext & 8) || bdsz == 0 || (is && iis > 3) || iis == 4) { d->bad = 1; return; }
    int32_t bd = bdsz == 2 ? (int16_t)w16(d) : bdsz == 3 ? (int32_t)w32(d) : 0;
    int32_t od = 0;
    int odsz = iis & 3, mem = iis != 0;
    if (odsz == 2) od = (int16_t)w16(d);
    else if (odsz == 3) od = (int32_t)w32(d);
    int post = !is && iis >= 5;
    /* the base: bd and An or PC */
    char base[48] = "", *b = base;
    size_t bn = sizeof base;
    int k = 0;
    if (an < 0 && !bs) k = snprintf(b, bn, "$%08x,pc", (unsigned)(pcbase + (uint32_t)bd));
    else {
        if (bdsz > 1) k = bd < 0 ? snprintf(b, bn, "-$%x", (unsigned)-(int64_t)bd) : snprintf(b, bn, "$%x", (unsigned)bd);
        if (!bs) k += snprintf(b + k, bn - (size_t)k, "%sa%d", k ? "," : "", an);
        else if (an < 0) k += snprintf(b + k, bn - (size_t)k, "%szpc", k ? "," : "");
    }
    put(d, "(");
    if (mem) put(d, "[");
    put(d, "%s", base);
    if (!is && !post) { put(d, "%s", base[0] ? "," : ""); xn(d, ext); }
    if (mem) put(d, "]");
    if (!is && post) { put(d, ","); xn(d, ext); }
    if (mem && odsz > 1) { put(d, ","); sx(d, od); }
    if (!mem && !base[0] && is) put(d, "0");
    put(d, ")");
}

/* sz: 0 b, 1 w, 2 l, 3 s (single), 4 d, 5 x, 6 p (for immediates) */
static void ea(dis *d, int mode, int r, int sz) {
    switch (mode) {
    case 0: put(d, "d%d", r); return;
    case 1: put(d, "a%d", r); return;
    case 2: put(d, "(a%d)", r); return;
    case 3: put(d, "(a%d)+", r); return;
    case 4: put(d, "-(a%d)", r); return;
    case 5: sx(d, (int16_t)w16(d)); put(d, "(a%d)", r); return;
    case 6: indexed(d, r, 0); return;
    default: break;
    }
    switch (r) {
    case 0: put(d, "$%x.w", w16(d)); return;
    case 1: put(d, "$%08x", w32(d)); return;
    case 2: { uint32_t at = d->p; put(d, "$%08x(pc)", (unsigned)(at + (uint32_t)(int16_t)w16(d))); return; }
    case 3: { uint32_t at = d->p; indexed(d, -1, at); return; }
    case 4:
        switch (sz) {
        case 0: put(d, "#$%x", w16(d) & 0xff); return;
        case 1: put(d, "#$%x", w16(d)); return;
        case 2: case 3: put(d, "#$%x", w32(d)); return;
        case 4: { uint32_t h = w32(d); put(d, "#$%08x%08x", h, w32(d)); return; }
        default: { uint32_t a = w32(d), b2 = w32(d); put(d, "#$%08x%08x%08x", a, b2, w32(d)); return; }
        }
    default: d->bad = 1; return;
    }
}

/* Which addressing modes an operand may use: a bit per mode, 7.0 to 7.4 as 7 to 11 */
enum { M_DN = 1, M_AN = 2, M_IND = 4, M_POST = 8, M_PRE = 16, M_D16 = 32, M_IDX = 64,
       M_ABSW = 128, M_ABSL = 256, M_PC16 = 512, M_PCIDX = 1024, M_IMM = 2048 };
#define V_ALL      0xfff
#define V_DATA     (V_ALL & ~M_AN)
#define V_MEM      (V_DATA & ~M_DN)
#define V_CTRL     (M_IND | M_D16 | M_IDX | M_ABSW | M_ABSL | M_PC16 | M_PCIDX)
#define V_DALT     (M_DN | M_IND | M_POST | M_PRE | M_D16 | M_IDX | M_ABSW | M_ABSL)
#define V_MALT     (V_DALT & ~M_DN)
#define V_ALT      (V_DALT | M_AN)
#define V_CALT     (V_CTRL & ~(M_PC16 | M_PCIDX))

static int ok(dis *d, uint16_t op, int allowed) {
    int mode = op >> 3 & 7, r = op & 7, bit = mode < 7 ? mode : r <= 4 ? 7 + r : 16;
    if (bit > 11 || !(allowed & 1 << bit)) { d->bad = 1; return 0; }
    return 1;
}

static void ea6(dis *d, uint16_t op, int sz) { ea(d, op >> 3 & 7, op & 7, sz); }
/* an operand that must be one of allowed */
static void eav(dis *d, uint16_t op, int sz, int allowed) { if (ok(d, op, allowed)) ea6(d, op, sz); }

/* movem's list: d0-d3/a6; rev for -(an) */
static void reglist(dis *d, uint16_t m, int rev) {
    if (rev) { uint16_t r = 0; for (int i = 0; i < 16; i++) if (m & 1 << i) r |= (uint16_t)(1 << (15 - i)); m = r; }
    int first = 1;
    for (int i = 0; i < 16; i++) {
        if (!(m & 1 << i)) continue;
        int j = i;
        while (j + 1 < 16 && (j + 1) % 8 && (m & 1 << (j + 1))) j++;
        put(d, "%s", first ? "" : "/");
        reg(d, i);
        if (j > i) { put(d, "-"); reg(d, j); }
        first = 0;
        i = j;
    }
    if (first) put(d, "0");
}

static const char *const cc[16] = { "t", "f", "hi", "ls", "cc", "cs", "ne", "eq", "vc", "vs", "pl", "mi", "ge", "lt", "gt", "le" };
static const char *const fcc[32] = { "f", "eq", "ogt", "oge", "olt", "ole", "ogl", "or", "un", "ueq", "ugt", "uge", "ult", "ule", "ne", "t",
                                     "sf", "seq", "gt", "ge", "lt", "le", "gl", "gle", "ngle", "ngl", "nle", "nlt", "nge", "ngt", "sne", "st" };

static void mn(dis *d, const char *m) { put(d, "%-7s ", m); }
static void mns(dis *d, const char *m, int size) { char t[24]; snprintf(t, sizeof t, "%s.%c", m, sizes[size]); mn(d, t); }

static void ctrlreg(dis *d, int r) {
    static const struct { int r; const char *n; } t[] = {
        { 0x000, "sfc" }, { 0x001, "dfc" }, { 0x002, "cacr" }, { 0x003, "tc" }, { 0x004, "itt0" }, { 0x005, "itt1" },
        { 0x006, "dtt0" }, { 0x007, "dtt1" }, { 0x800, "usp" }, { 0x801, "vbr" }, { 0x802, "caar" }, { 0x803, "msp" },
        { 0x804, "isp" }, { 0x805, "mmusr" }, { 0x806, "urp" }, { 0x807, "srp" },
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) if (t[i].r == r) { put(d, "%s", t[i].n); return; }
    put(d, "$%03x", r);
}

static void group0(dis *d, uint16_t op) {
    int size = op >> 6 & 3, mode = op >> 3 & 7, r = op & 7;
    if (op & 0x100) {                                       /* dynamic bits, movep */
        if (mode == 1) {
            int om = op >> 6 & 3;
            mn(d, om & 1 ? "movep.l" : "movep.w");
            int16_t disp = (int16_t)w16(d);
            if (om & 2) { put(d, "d%d,", op >> 9 & 7); sx(d, disp); put(d, "(a%d)", r); }
            else { sx(d, disp); put(d, "(a%d),d%d", r, op >> 9 & 7); }
            return;
        }
        static const char *const b[4] = { "btst", "bchg", "bclr", "bset" };
        mn(d, b[size]);
        put(d, "d%d,", op >> 9 & 7);
        eav(d, op, 0, size == 0 ? V_DATA : V_DALT);
        return;
    }
    int k = op >> 9 & 7;
    if (k == 4) {                                           /* static bits */
        static const char *const b[4] = { "btst", "bchg", "bclr", "bset" };
        mn(d, b[size]);
        put(d, "#%d,", w16(d) & 0xff);
        eav(d, op, 0, size == 0 ? V_DATA & ~M_IMM : V_DALT);
        return;
    }
    if (size == 3) {
        if (k == 7 || k == 3) { d->bad = 1; return; }     /* (rtm, callm: the 68020's alone) */
        if (k <= 2) {                                       /* chk2, cmp2 */
            if (!need020(d)) return;
            uint16_t ext = w16(d);
            if (ext & 0x7ff) { d->bad = 1; return; }
            mns(d, ext & 0x800 ? "chk2" : "cmp2", k);
            eav(d, op, k, V_CTRL);
            put(d, ",");
            reg(d, ext >> 12);
            return;
        }
        /* cas, cas2: k 5 .b, 6 .w, 7 .l */
        if (!need020(d)) return;
        int s = k - 5;                                      /* .b .w .l */
        if ((op & 0x3f) == 0x3c && s) {
            uint16_t e1 = w16(d), e2 = w16(d);
            if ((e1 | e2) & 0x0e38) { d->bad = 1; return; }
            mns(d, "cas2", s);
            put(d, "d%d:d%d,d%d:d%d,(", e1 & 7, e2 & 7, e1 >> 6 & 7, e2 >> 6 & 7);
            reg(d, e1 >> 12); put(d, "):("); reg(d, e2 >> 12); put(d, ")");
            return;
        }
        uint16_t ext = w16(d);
        if (ext & 0xfe38) { d->bad = 1; return; }
        mns(d, "cas", s);
        put(d, "d%d,d%d,", ext & 7, ext >> 6 & 7);
        eav(d, op, s, V_MALT);
        return;
    }
    if (k == 7) {                                           /* moves */
        if (!need020(d)) return;
        uint16_t ext = w16(d);
        if (ext & 0x7ff) { d->bad = 1; return; }
        mns(d, "moves", size);
        if (ext & 0x800) { reg(d, ext >> 12); put(d, ","); eav(d, op, size, V_MALT); }
        else { eav(d, op, size, V_MALT); put(d, ","); reg(d, ext >> 12); }
        return;
    }
    static const char *const im[8] = { "ori", "andi", "subi", "addi", NULL, "eori", "cmpi", NULL };
    if (!im[k]) { d->bad = 1; return; }
    if ((op & 0x3f) == 0x3c && (k == 0 || k == 1 || k == 5)) {   /* to ccr, sr */
        if (size > 1) { d->bad = 1; return; }
        mns(d, im[k], size);
        put(d, size ? "#$%x,sr" : "#$%x,ccr", size ? w16(d) : w16(d) & 0xff);
        return;
    }
    mns(d, im[k], size);
    ea(d, 7, 4, size);
    put(d, ",");
    if (k == 6 && mode == 7 && (r == 2 || r == 3)) need020(d);   /* cmpi to (pc) */
    eav(d, op, size, k == 6 ? V_DATA & ~M_IMM : V_DALT);
}

static void move(dis *d, uint16_t op) {
    static const int sz[4] = { -1, 0, 2, 1 };
    int s = sz[op >> 12 & 3], dm = op >> 6 & 7, dr = op >> 9 & 7;
    if ((dm == 1 && !s) || (dm == 7 && dr > 1)) { d->bad = 1; return; }
    if (dm == 1) mns(d, "movea", s);
    else mns(d, "move", s);
    eav(d, op, s, s == 0 ? V_DATA : V_ALL);
    put(d, ",");
    ea(d, dm, dr, s);
}

static void group4(dis *d, uint16_t op) {
    int size = op >> 6 & 3, mode = op >> 3 & 7, r = op & 7;
    if (op & 0x100) {
        if ((op & 0xfff8) == 0x49c0) { if (!need020(d)) return; mn(d, "extb.l"); put(d, "d%d", r); return; }
        if (size == 3) { mn(d, "lea"); eav(d, op, 2, V_CTRL); put(d, ",a%d", op >> 9 & 7); return; }
        if (size == 2) { mn(d, "chk.w"); eav(d, op, 1, V_DATA); put(d, ",d%d", op >> 9 & 7); return; }
        if (size == 0) { if (!need020(d)) return; mn(d, "chk.l"); eav(d, op, 2, V_DATA); put(d, ",d%d", op >> 9 & 7); return; }
        d->bad = 1;
        return;
    }
    switch (op >> 8 & 0xf) {
    case 0x0: case 0x2: case 0x4: case 0x6:
        if (size == 3) {
            switch (op >> 9 & 3) {
            case 0: mn(d, "move.w"); put(d, "sr,"); eav(d, op, 1, V_DALT); return;
            case 1: if (!need020(d)) return; mn(d, "move.w"); put(d, "ccr,"); eav(d, op, 1, V_DALT); return;
            case 2: mn(d, "move.w"); eav(d, op, 1, V_DATA); put(d, ",ccr"); return;
            default: mn(d, "move.w"); eav(d, op, 1, V_DATA); put(d, ",sr"); return;
            }
        }
        { static const char *const n[4] = { "negx", "clr", "neg", "not" }; mns(d, n[op >> 9 & 3], size); eav(d, op, size, V_DALT); }
        return;
    case 0x8:
        if (size == 0) {
            if (mode == 1) { if (!need020(d)) return; mn(d, "link.l"); put(d, "a%d,#", r); sx(d, (int32_t)w32(d)); return; }
            mn(d, "nbcd"); eav(d, op, 0, V_DALT); return;
        }
        if (size == 1) {
            if (mode == 0) { mn(d, "swap"); put(d, "d%d", r); return; }
            if (mode == 1) { if (!need020(d)) return; mn(d, "bkpt"); put(d, "#%d", r); return; }
            mn(d, "pea"); eav(d, op, 2, V_CTRL); return;
        }
        if (mode == 0) { mn(d, size == 2 ? "ext.w" : "ext.l"); put(d, "d%d", r); return; }
        {   /* movem regs to memory */
            uint16_t m = w16(d);
            mn(d, size == 2 ? "movem.w" : "movem.l");
            reglist(d, m, mode == 4);
            put(d, ",");
            eav(d, op, 2, V_CALT | M_PRE);
        }
        return;
    case 0xa:
        if (op == 0x4afc) { put(d, "illegal"); return; }
        if (size == 3) { mn(d, "tas"); eav(d, op, 0, V_DALT); return; }
        mns(d, "tst", size);
        if (mode == 1 || (mode == 7 && r >= 2)) need020(d);
        eav(d, op, size, size == 0 ? V_DATA : V_ALL);
        return;
    case 0xc:
        if (size >= 2) {
            uint16_t m = w16(d);
            mn(d, size == 2 ? "movem.w" : "movem.l");
            eav(d, op, 2, V_CTRL | M_POST);
            put(d, ",");
            reglist(d, m, 0);
            return;
        }
        {   /* mull, divl */
            if (!need020(d)) return;
            uint16_t ext = w16(d);
            int dl = ext >> 12 & 7, dh = ext & 7, sg = ext >> 11 & 1, q = ext >> 10 & 1;
            if ((ext & 0x83f8) || (q && dh == dl)) { d->bad = 1; return; }
            if (size == 0) {
                mn(d, sg ? "muls.l" : "mulu.l");
                eav(d, op, 2, V_DATA);
                if (q) put(d, ",d%d:d%d", dh, dl); else put(d, ",d%d", dl);
            } else {
                mn(d, q ? (sg ? "divs.l" : "divu.l") : dh != dl ? (sg ? "divsl.l" : "divul.l") : (sg ? "divs.l" : "divu.l"));
                eav(d, op, 2, V_DATA);
                if (q || dh != dl) put(d, ",d%d:d%d", dh, dl); else put(d, ",d%d", dl);
            }
        }
        return;
    case 0xe:
        if (size == 1) {
            switch (mode) {
            case 0: case 1: mn(d, "trap"); put(d, "#%d", op & 15); return;
            case 2: mn(d, "link.w"); put(d, "a%d,#", r); sx(d, (int16_t)w16(d)); return;
            case 3: mn(d, "unlk"); put(d, "a%d", r); return;
            case 4: mn(d, "move.l"); put(d, "a%d,usp", r); return;
            case 5: mn(d, "move.l"); put(d, "usp,a%d", r); return;
            case 6: {
                static const char *const n[8] = { "reset", "nop", "stop", "rte", "rtd", "rts", "trapv", "rtr" };
                if (r == 2) { mn(d, "stop"); put(d, "#$%x", w16(d)); return; }
                if (r == 4) { if (!need020(d)) return; mn(d, "rtd"); put(d, "#"); sx(d, (int16_t)w16(d)); return; }
                put(d, "%s", n[r]);
                return;
            }
            default:
                if (r == 2 || r == 3) {
                    if (!need020(d)) return;
                    uint16_t ext = w16(d);
                    mn(d, "movec");
                    if (r == 2) { ctrlreg(d, ext & 0xfff); put(d, ","); reg(d, ext >> 12); }
                    else { reg(d, ext >> 12); put(d, ","); ctrlreg(d, ext & 0xfff); }
                    return;
                }
                d->bad = 1;
                return;
            }
        }
        if (size == 2) { mn(d, "jsr"); eav(d, op, 2, V_CTRL); return; }
        if (size == 3) { mn(d, "jmp"); eav(d, op, 2, V_CTRL); return; }
        d->bad = 1;
        return;
    default: d->bad = 1; return;
    }
}

static void group5(dis *d, uint16_t op) {
    int size = op >> 6 & 3, mode = op >> 3 & 7;
    if (size != 3) {
        int q = op >> 9 & 7;
        mns(d, op & 0x100 ? "subq" : "addq", size);
        put(d, "#%d,", q ? q : 8);
        eav(d, op, size, size == 0 ? V_DALT : V_ALT);
        return;
    }
    char m[16];
    int c = op >> 8 & 15;
    if (mode == 1) {
        snprintf(m, sizeof m, "db%s", c == 1 ? "ra" : cc[c]);
        mn(d, m);
        uint32_t at = d->p;
        put(d, "d%d,$%08x", op & 7, (unsigned)(at + (uint32_t)(int16_t)w16(d)));
        return;
    }
    if (mode == 7 && (op & 7) >= 2 && (op & 7) <= 4) {
        if (!need020(d)) return;
        int k = op & 7;
        snprintf(m, sizeof m, "trap%s%s", cc[c], k == 2 ? ".w" : k == 3 ? ".l" : "");
        if (k == 4) { put(d, "%s", m); return; }
        mn(d, m);
        put(d, "#$%x", k == 2 ? w16(d) : w32(d));
        return;
    }
    snprintf(m, sizeof m, "s%s", cc[c]);
    mn(d, m);
    eav(d, op, 0, V_DALT);
}

static void branch(dis *d, uint16_t op) {
    int c = op >> 8 & 15;
    char m[16];
    int32_t disp = (int8_t)(op & 0xff);
    const char *sfx = ".s";
    uint32_t at = d->p;
    if ((op & 0xff) == 0) { disp = (int16_t)w16(d); sfx = ".w"; }
    else if ((op & 0xff) == 0xff) { if (!need020(d)) return; disp = (int32_t)w32(d); sfx = ".l"; }
    snprintf(m, sizeof m, "b%s%s", c == 0 ? "ra" : c == 1 ? "sr" : cc[c], sfx);
    mn(d, m);
    put(d, "$%08x", (unsigned)(at + (uint32_t)disp));
}

/* or, and, sub, add, cmp, eor with their a, x and bcd forms */
static void arith(dis *d, uint16_t op) {
    int g = op >> 12, opm = op >> 6 & 7, dr = op >> 9 & 7, mode = op >> 3 & 7, r = op & 7;
    int size = opm & 3;
    const char *name = g == 8 ? "or" : g == 9 ? "sub" : g == 0xb ? "cmp" : g == 0xc ? "and" : "add";
    if (size == 3) {                                        /* the word ops: divu/s, mulu/s, suba, adda, cmpa */
        int l = opm & 4;
        if (g == 8) { mn(d, l ? "divs.w" : "divu.w"); eav(d, op, 1, V_DATA); put(d, ",d%d", dr); return; }
        if (g == 0xc) { mn(d, l ? "muls.w" : "mulu.w"); eav(d, op, 1, V_DATA); put(d, ",d%d", dr); return; }
        char m[16];
        snprintf(m, sizeof m, "%sa.%c", name, l ? 'l' : 'w');
        mn(d, m);
        eav(d, op, l ? 2 : 1, V_ALL);
        put(d, ",a%d", dr);
        return;
    }
    if (opm & 4) {                                          /* to memory, or the register forms */
        if (mode <= 1) {
            if (g == 0xb) {
                if (mode == 1) { mns(d, "cmpm", size); put(d, "(a%d)+,(a%d)+", r, dr); return; }
                mns(d, "eor", size); put(d, "d%d,d%d", dr, r); return;
            }
            if (g == 8 || g == 0xc) {
                if (size == 0) { mn(d, g == 8 ? "sbcd" : "abcd"); }
                else if (g == 8) {                          /* pack, unpk */
                    if (!need020(d)) return;
                    mn(d, size == 1 ? "pack" : "unpk");
                    if (mode) put(d, "-(a%d),-(a%d),#$%x", r, dr, w16(d)); else put(d, "d%d,d%d,#$%x", r, dr, w16(d));
                    return;
                } else {                                    /* exg */
                    int t = op >> 3 & 0x1f;
                    mn(d, "exg");
                    if (t == 0x08) put(d, "d%d,d%d", dr, r);
                    else if (t == 0x09) put(d, "a%d,a%d", dr, r);
                    else if (t == 0x11) put(d, "d%d,a%d", dr, r);
                    else d->bad = 1;
                    return;
                }
                if (mode) put(d, "-(a%d),-(a%d)", r, dr); else put(d, "d%d,d%d", r, dr);
                return;
            }
            mns(d, g == 9 ? "subx" : "addx", size);         /* addx, subx */
            if (mode) put(d, "-(a%d),-(a%d)", r, dr); else put(d, "d%d,d%d", r, dr);
            return;
        }
        mns(d, g == 0xb ? "eor" : name, size);
        put(d, "d%d,", dr);
        eav(d, op, size, V_MALT);
        return;
    }
    if (mode == 1 && (size == 0 || g == 8 || g == 0xc)) { d->bad = 1; return; }
    mns(d, name, size);
    eav(d, op, size, V_ALL);
    put(d, ",d%d", dr);
}

static void shift(dis *d, uint16_t op) {
    static const char *const n[4] = { "as", "ls", "rox", "ro" };
    int size = op >> 6 & 3;
    char m[16];
    if (size == 3) {
        if (op & 0x800) {                                   /* bit fields */
            if (!need020(d)) return;
            static const char *const bf[8] = { "bftst", "bfextu", "bfchg", "bfexts", "bfclr", "bfffo", "bfset", "bfins" };
            int k = op >> 8 & 7;
            uint16_t ext = w16(d);
            if ((ext & 0x8000) || (!(k & 1) && k != 7 && (ext & 0xf000))) { d->bad = 1; return; }
            mn(d, bf[k]);
            if (k == 7) put(d, "d%d,", ext >> 12 & 7);
            eav(d, op, 2, (k == 0 || k == 1 || k == 3 || k == 5 ? V_CTRL : V_CALT) | M_DN);
            put(d, "{");
            if (ext & 0x800) put(d, "d%d", ext >> 6 & 7); else put(d, "%d", ext >> 6 & 31);
            put(d, ":");
            if (ext & 0x20) put(d, "d%d", ext & 7); else put(d, "%d", (ext & 31) ? ext & 31 : 32);
            put(d, "}");
            if (k == 1 || k == 3 || k == 5) put(d, ",d%d", ext >> 12 & 7);
            return;
        }
        snprintf(m, sizeof m, "%s%c.w", n[op >> 9 & 3], op & 0x100 ? 'l' : 'r');
        mn(d, m);
        eav(d, op, 1, V_MALT);
        return;
    }
    snprintf(m, sizeof m, "%s%c.%c", n[op >> 3 & 3], op & 0x100 ? 'l' : 'r', sizes[size]);
    mn(d, m);
    int c = op >> 9 & 7;
    if (op & 0x20) put(d, "d%d,d%d", c, op & 7);
    else put(d, "#%d,d%d", c ? c : 8, op & 7);
}

/* ---- the FPU, the 68040's caches and MMU, move16 --------------------------- */

static const char *fop(int o) {
    static const struct { int o; const char *n; } t[] = {
        { 0x00, "fmove" }, { 0x01, "fint" }, { 0x02, "fsinh" }, { 0x03, "fintrz" }, { 0x04, "fsqrt" }, { 0x06, "flognp1" },
        { 0x08, "fetoxm1" }, { 0x09, "ftanh" }, { 0x0a, "fatan" }, { 0x0c, "fasin" }, { 0x0d, "fatanh" }, { 0x0e, "fsin" },
        { 0x0f, "ftan" }, { 0x10, "fetox" }, { 0x11, "ftwotox" }, { 0x12, "ftentox" }, { 0x14, "flogn" }, { 0x15, "flog10" },
        { 0x16, "flog2" }, { 0x18, "fabs" }, { 0x19, "fcosh" }, { 0x1a, "fneg" }, { 0x1c, "facos" }, { 0x1d, "fcos" },
        { 0x1e, "fgetexp" }, { 0x1f, "fgetman" }, { 0x20, "fdiv" }, { 0x21, "fmod" }, { 0x22, "fadd" }, { 0x23, "fmul" },
        { 0x24, "fsgldiv" }, { 0x25, "frem" }, { 0x26, "fscale" }, { 0x27, "fsglmul" }, { 0x28, "fsub" }, { 0x38, "fcmp" },
        { 0x3a, "ftst" }, { 0x40, "fsmove" }, { 0x41, "fssqrt" }, { 0x44, "fdmove" }, { 0x45, "fdsqrt" }, { 0x58, "fsabs" },
        { 0x5a, "fsneg" }, { 0x5c, "fdabs" }, { 0x5e, "fdneg" }, { 0x60, "fsdiv" }, { 0x62, "fsadd" }, { 0x63, "fsmul" },
        { 0x64, "fddiv" }, { 0x66, "fdadd" }, { 0x67, "fdmul" }, { 0x68, "fssub" }, { 0x6c, "fdsub" },
    };
    if (o >= 0x30 && o <= 0x37) return "fsincos";
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) if (t[i].o == o) return t[i].n;
    return NULL;
}

static int fdn(int fmt) { return fmt == 0 || fmt == 1 || fmt == 4 || fmt == 6; }   /* a data register may hold it */
static const char ffmt[8] = { 'l', 's', 'x', 'p', 'w', 'd', 'b', 'p' };
static const int fimm[8] = { 2, 3, 5, 6, 1, 4, 0, 6 };   /* ea()'s immediate size for each format */

static void fpregs(dis *d, int m, int rev) {   /* fmovem's list: bit 7 is fp0 (bit 0 for -(an)) */
    int first = 1;
    for (int i = 0; i < 8; i++) {
        int bit = rev ? i : 7 - i;
        if (!(m & 1 << bit)) continue;
        int j = i;
        while (j + 1 < 8 && (m & 1 << (rev ? j + 1 : 7 - (j + 1)))) j++;
        put(d, "%sfp%d", first ? "" : "/", i);
        if (j > i) put(d, "-fp%d", j);
        first = 0;
        i = j;
    }
    if (first) put(d, "0");
}

static void fctl(dis *d, int l) {
    int first = 1;
    if (l & 4) { put(d, "fpcr"); first = 0; }
    if (l & 2) { put(d, "%sfpsr", first ? "" : "/"); first = 0; }
    if (l & 1) { put(d, "%sfpiar", first ? "" : "/"); first = 0; }
    if (first) put(d, "0");
}

static void fpu(dis *d, uint16_t op) {
    int type = op >> 6 & 7, mode = op >> 3 & 7, r = op & 7;
    char m[24];
    if (!need040(d)) return;
    if (type == 0) {
        uint16_t ext = w16(d);
        int cls = ext >> 13, rx = ext >> 10 & 7, ry = ext >> 7 & 7, o = ext & 0x7f;
        switch (cls) {
        case 0: case 2: {
            if (cls == 2 && rx == 7) { if (op & 0x3f) { d->bad = 1; return; } mn(d, "fmovecr.x"); put(d, "#$%x,fp%d", o, ry); return; }
            const char *n = fop(o);
            if (!n) { d->bad = 1; return; }
            snprintf(m, sizeof m, "%s.%c", n, cls == 0 ? 'x' : ffmt[rx]);
            mn(d, m);
            if (cls == 0) put(d, "fp%d", rx); else eav(d, op, fimm[rx], fdn(rx) ? V_DATA : V_MEM);
            if (o >= 0x30 && o <= 0x37) put(d, ",fp%d:fp%d", o & 7, ry);
            else if (o != 0x3a) put(d, ",fp%d", ry);
            return;
        }
        case 3: {
            if ((rx != 3 && rx != 7 && o) || (rx == 7 && (o & 0x0f))) { d->bad = 1; return; }
            snprintf(m, sizeof m, "fmove.%c", ffmt[rx]);
            mn(d, m);
            put(d, "fp%d,", ry);
            eav(d, op, fimm[rx], fdn(rx) ? V_DALT : V_MALT);
            if (rx == 3) { int k = o & 0x40 ? o - 128 : o; put(d, "{#%d}", k); }
            else if (rx == 7) put(d, "{d%d}", o >> 4 & 7);
            return;
        }
        case 4: case 5: {
            int l = rx, many = (l & (l - 1)) != 0;
            if (!l || o || (ext & 0x380)) { d->bad = 1; return; }
            mn(d, many ? "fmovem.l" : "fmove.l");
            if (cls == 4) { eav(d, op, 2, l == 1 ? V_ALL : many ? V_MEM : V_DATA); put(d, ","); fctl(d, l); }
            else { fctl(d, l); put(d, ","); eav(d, op, 2, l == 1 ? V_ALT : many ? V_MALT : V_DALT); }
            return;
        }
        default: {
            int fm = ext >> 11 & 3, list = ext & 0xff;
            if ((ext & 0x700) || ((fm & 1) && (list & 0x8f)) || (cls == 6 ? fm < 2 : (mode == 4) != (fm < 2))) { d->bad = 1; return; }
            mn(d, "fmovem.x");
            if (cls == 6) {
                eav(d, op, 5, V_CTRL | M_POST);
                put(d, ",");
                if (fm & 1) put(d, "d%d", list >> 4 & 7); else fpregs(d, list, 0);
            } else {
                if (fm & 1) put(d, "d%d", list >> 4 & 7); else fpregs(d, list, !(fm & 2));
                put(d, ",");
                eav(d, op, 5, V_CALT | M_PRE);
            }
            return;
        }
        }
    }
    if (type == 1) {                                        /* fscc, fdbcc, ftrapcc */
        uint16_t ext = w16(d);
        if (ext & 0xffe0) { d->bad = 1; return; }
        const char *c = fcc[ext & 31];
        if (mode == 1) {
            snprintf(m, sizeof m, "fdb%s", c);
            mn(d, m);
            uint32_t at = d->p;
            put(d, "d%d,$%08x", r, (unsigned)(at + (uint32_t)(int16_t)w16(d)));
            return;
        }
        if (mode == 7 && r >= 2 && r <= 4) {
            snprintf(m, sizeof m, "ftrap%s%s", c, r == 2 ? ".w" : r == 3 ? ".l" : "");
            if (r == 4) { put(d, "%s", m); return; }
            mn(d, m);
            put(d, "#$%x", r == 2 ? w16(d) : w32(d));
            return;
        }
        snprintf(m, sizeof m, "fs%s", c);
        mn(d, m);
        eav(d, op, 0, V_DALT);
        return;
    }
    if (type == 2 || type == 3) {                           /* fbcc, fnop */
        uint32_t at = d->p;
        int32_t disp = type == 2 ? (int16_t)w16(d) : (int32_t)w32(d);
        if (op & 0x20) { d->bad = 1; return; }
        if (op == 0xf280 && disp == 0) { put(d, "fnop"); return; }
        snprintf(m, sizeof m, "fb%s.%c", fcc[op & 31], type == 2 ? 'w' : 'l');
        mn(d, m);
        put(d, "$%08x", (unsigned)(at + (uint32_t)disp));
        return;
    }
    if (type == 4) { mn(d, "fsave"); eav(d, op, 2, V_CALT | M_PRE); return; }
    if (type == 5) { mn(d, "frestore"); eav(d, op, 2, V_CTRL | M_POST); return; }
    d->bad = 1;
}

static void linef(dis *d, uint16_t op) {
    if ((op & 0xfe00) == 0xf200) { fpu(d, op); return; }
    if (!need040(d)) return;
    if ((op & 0xff00) == 0xf400) {                          /* cinv, cpush */
        static const char *const cache[4] = { "nc", "dc", "ic", "bc" };
        static const char sc[4] = { 0, 'l', 'p', 'a' };
        int c = op >> 6 & 3, s = op >> 3 & 3;
        if (!s) { d->bad = 1; return; }
        char m[16];
        snprintf(m, sizeof m, "%s%c", op & 0x20 ? "cpush" : "cinv", sc[s]);
        mn(d, m);
        put(d, "%s", cache[c]);
        if (s != 3) put(d, ",(a%d)", op & 7);
        return;
    }
    if ((op & 0xffe0) == 0xf500) {                          /* pflush */
        static const char *const n[4] = { "pflushn", "pflush", "pflushan", "pflusha" };
        int k = op >> 3 & 3;
        if (k < 2) { mn(d, n[k]); put(d, "(a%d)", op & 7); } else put(d, "%s", n[k]);
        return;
    }
    if ((op & 0xffd8) == 0xf548) { mn(d, op & 0x20 ? "ptestr" : "ptestw"); put(d, "(a%d)", op & 7); return; }
    if ((op & 0xfff8) == 0xf620) {
        uint16_t ext = w16(d);
        if ((ext & 0x8fff) != 0x8000) { d->bad = 1; return; }
        mn(d, "move16");
        put(d, "(a%d)+,(a%d)+", op & 7, ext >> 12 & 7);
        return;
    }
    if ((op & 0xffe0) == 0xf600) {
        uint32_t a = w32(d);
        int r = op & 7;
        mn(d, "move16");
        switch (op >> 3 & 3) {
        case 0: put(d, "(a%d)+,$%08x", r, a); break;
        case 1: put(d, "$%08x,(a%d)+", a, r); break;
        case 2: put(d, "(a%d),$%08x", r, a); break;
        default: put(d, "$%08x,(a%d)", a, r); break;
        }
        return;
    }
    d->bad = 1;
}

int dis68k(uint32_t pc, dis68k_read16 rd, void *ctx, int model, char *out, size_t outsz) {
    char buf[160];
    dis d = { rd, ctx, pc, pc, model, 0, buf, sizeof buf, 0 };
    buf[0] = 0;
    uint16_t op = w16(&d);
    switch (op >> 12) {
    case 0x0: group0(&d, op); break;
    case 0x1: case 0x2: case 0x3: move(&d, op); break;
    case 0x4: group4(&d, op); break;
    case 0x5: group5(&d, op); break;
    case 0x6: branch(&d, op); break;
    case 0x7:
        if (op & 0x100) d.bad = 1;
        else { mn(&d, "moveq"); put(&d, "#"); sx(&d, (int8_t)(op & 0xff)); put(&d, ",d%d", op >> 9 & 7); }
        break;
    case 0x8: case 0x9: case 0xb: case 0xc: case 0xd: arith(&d, op); break;
    case 0xe: shift(&d, op); break;
    case 0xf: linef(&d, op); break;
    default: d.bad = 1; break;                              /* line A */
    }
    /* trailing blanks of a mnemonic with no operand */
    while (d.len && buf[d.len - 1] == ' ') buf[--d.len] = 0;
    if (d.bad || d.len >= sizeof buf - 1) {
        snprintf(out, outsz, "dc.w    $%04x", op);
        return 2;
    }
    snprintf(out, outsz, "%s", buf);
    return (int)(d.p - pc);
}

int dis68k_is_call(uint32_t pc, dis68k_read16 rd, void *ctx, int model) {
    uint16_t op = rd(ctx, pc);
    (void)model;
    if ((op & 0xff00) == 0x6100) return 1;                  /* bsr */
    if ((op & 0xffc0) == 0x4e80) return 1;                  /* jsr */
    if ((op & 0xfff0) == 0x4e40 || op == 0x4e76) return 1;  /* trap, trapv */
    if ((op & 0xf0f8) == 0x50c8) return 1;                  /* dbcc */
    if ((op & 0xf0ff) == 0x50fa || (op & 0xf0ff) == 0x50fb || (op & 0xf0ff) == 0x50fc) return 1;   /* trapcc */
    if ((op & 0xf140) == 0x4100) return 1;                  /* chk */
    if ((op & 0xf9c0) == 0x00c0) return 1;                  /* chk2, cmp2 */
    if ((op & 0xfff8) == 0xf248) return 1;                  /* fdbcc */
    return 0;
}
