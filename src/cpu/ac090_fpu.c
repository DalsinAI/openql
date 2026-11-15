/*
 * AC090's floating-point unit: the 68881/68882 instruction set as the
 * 68040 runs it, including the 040's single- and double-rounding forms.
 * The operations the real 040 leaves to software (transcendentals, packed
 * decimal, FMOVECR) are done here too, so software never needs its
 * emulation package. Included by ac090.c.
 *
 * On x86 hosts FP0-FP7 are held as the host's 80-bit extended type, which
 * is the 68881 extended format without its 16 unused bits, and the x87
 * does the arithmetic. Elsewhere (ARM64) fpu_soft.c holds the 80 bits
 * themselves and does it in integers, to the same bits.
 */
#include <math.h>
#include <float.h>
#include "fpu_special.h"                 /* extended values from their bits */
#include <stdio.h>
#include "fpu_decimal.c"                 /* packed decimal */

/* FPSR condition code bits */
#define FPCC_N   0x08000000u
#define FPCC_Z   0x04000000u
#define FPCC_I   0x02000000u
#define FPCC_NAN 0x01000000u

#if AC090_FPU_SOFT
#include "fpu_soft.c"
#else
typedef long double fpv;
#define fv_isnan(v) isnan(v)
typedef union { long double v; struct { uint64_t mant; uint16_t se; } b; } x87;

static long double from_ext(const uint8_t *p) {    /* 12 bytes, big-endian */
    x87 x;
    x.b.se = (uint16_t)(p[0] << 8 | p[1]);
    x.b.mant = 0;
    for (int i = 4; i < 12; i++) x.b.mant = x.b.mant << 8 | p[i];
    if ((x.b.se & 0x7fff) == 0x7fff) {
        /* Infinity or NaN: the FPU ignores the integer bit here, the x87
         * does not (clear, it is an invalid operand, a NaN). An infinity
         * saved with FMOVE.X and loaded back compared unequal to infinity
         * (the WebKit session's fpedge test, 4 Oct 2026). */
        x.b.mant |= 1ull << 63;
        return x.v;
    }
    if ((x.b.se & 0x7fff) != 0 && !(x.b.mant >> 63)) {
        long double m = (long double)x.b.mant;       /* unnormal: normalise the value */
        int e = (x.b.se & 0x7fff) - 16383 - 63;
        long double v = ldexpl(m, e);
        return (x.b.se & 0x8000) ? -v : v;
    }
    return x.v;
}
static void to_ext(long double v, uint8_t *p) {
    x87 x; x.v = v;
    p[0] = (uint8_t)(x.b.se >> 8); p[1] = (uint8_t)x.b.se; p[2] = p[3] = 0;
    for (int i = 0; i < 8; i++) p[4 + i] = (uint8_t)(x.b.mant >> (56 - 8 * i));
}

/* Packed decimal: 12 bytes (fpu_decimal.c) */
static long double from_packed(const uint8_t *p) { return fx_ld(fs_from_packed(p)); }
static void to_packed(long double v, int k, uint8_t *p) { fs_to_packed(fx_of(v), k, p); }

static void fpu_cc(ac090 *c, long double v) {
    uint32_t cc = 0;
    if (isnan(v)) cc |= FPCC_NAN;
    else if (isinf(v)) cc |= FPCC_I;
    else if (v == 0) cc |= FPCC_Z;
    if (signbit(v)) cc |= FPCC_N;
    c->fpsr = (c->fpsr & 0x00ffffffu) | cc;
}

/* Round a result to the precision FPCR asks for (or the instruction's own). */
static long double ext_round(long double v) { return v; }
static long double near_int(long double v) { return nearbyintl(v); }
static long double fpu_round(ac090 *c, long double v, int prec) {
    if (prec == 0) prec = c->fpcr >> 6 & 3;
    if (prec == 1) return (long double)(float)v;
    if (prec == 2) return (long double)(double)v;
    return ext_round(v);
}
static long double round_mode(ac090 *c, long double v) {
    if (isnan(v)) return v + v;                     /* quiet, as the FPU makes it: musl's truncl and nearbyintl hand a signalling NaN back as it was */
    switch (c->fpcr >> 4 & 3) {
    case 1: return truncl(v);
    case 2: return floorl(v);
    case 3: return ceill(v);
    }
    return near_int(v);                             /* to nearest, ties to even */
}

static int32_t to_int(ac090 *c, long double v, int64_t lo, int64_t hi) {
    if (isnan(v)) return (int32_t)hi;
    long double r = round_mode(c, v);
    if (r < (long double)lo) return (int32_t)lo;
    if (r > (long double)hi) return (int32_t)hi;
    return (int32_t)(int64_t)r;
}


/* The other formats, in and out, as C converts them on the x87 */
static long double fv_int(int32_t n) { return (long double)n; }
static long double fv_f32(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static long double fv_f64(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }
static uint32_t fv_to_f32(long double v) { float f = (float)v; uint32_t u; memcpy(&u, &f, 4); return u; }
static uint64_t fv_to_f64(long double v) { double d = (double)v; uint64_t u; memcpy(&u, &d, 8); return u; }
static long double fv_nan(void) { return NAN; }

static long double fmovecr(int off) {
    switch (off) {
    case 0x00: return 3.14159265358979323846264338327950288L;
    case 0x0b: return 0.301029995663981195213738894724493027L;   /* log10(2) */
    case 0x0c: return 2.71828182845904523536028747135266250L;    /* e */
    case 0x0d: return 1.44269504088896340735992468100189214L;    /* log2(e) */
    case 0x0e: return 0.434294481903251827651128918916605082L;   /* log10(e) */
    case 0x0f: return 0.0L;
    case 0x30: return 0.693147180559945309417232121458176568L;   /* ln 2 */
    case 0x31: return 2.30258509299404568401799145468436421L;    /* ln 10 */
    case 0x32: return 1.0L;
    case 0x33: return 10.0L;
    case 0x34: return 100.0L;
    case 0x35: return 1e4L;
    case 0x36: return 1e8L;
    case 0x37: return 1e16L;
    case 0x38: return 1e32L;
    case 0x39: return 1e64L;
    case 0x3a: return 1e128L;
    case 0x3b: return 1e256L;
    case 0x3c: return 1e512L;
    case 0x3d: return 1e1024L;
    case 0x3e: return 1e2048L;
    case 0x3f: return 1e4096L;
    }
    return 0.0L;
}

/* The FPU's default NaN, sign clear and every significand bit set: what an
 * operand error gives on the 68881 and 68882, and from the 68040's FPSP for
 * the instructions the 68040 leaves to it. The host's libm gives its own NaN
 * there (the x87's: 7FFF or FFFF C000 0000 0000 0000), which OS 3.2's
 * mathieeesingtrans and mathieeedoubtrans passed on whenever no
 * fpsp.resource was there to call instead (MathTransABITest, 24 Oct 2026). */
static long double fpu_default_nan(void) {
    static const uint8_t b[12] = { 0x7f, 0xff, 0, 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    return from_ext(b);
}
#endif

static const int fsize[8] = { 4, 4, 12, 12, 2, 8, 1, 12 };   /* L S X P W D B P(dynamic k) */

/* Read an operand of format fmt from <ea>. */
static fpv fpu_read_ea(ac090 *c, int mode, int reg, int fmt) {
    int sz = fsize[fmt];
    uint8_t b[12];
    if (mode == 0) {                                /* Dn: L, S, W, B only */
        uint32_t d = c->d[reg];
        switch (fmt) {
        case 0: return fv_int((int32_t)d);
        case 1: return fv_f32(d);
        case 4: return fv_int((int16_t)d);
        case 6: return fv_int((int8_t)d);
        }
        illegal(c); longjmp(c->abort, 2);
    }
    if (mode == 7 && reg == 4) {                    /* #immediate */
        if (sz <= 2) { uint32_t w = fetch16(c); b[0] = (uint8_t)(w >> 8); b[1] = (uint8_t)w; if (sz == 1) b[0] = b[1]; }
        else for (int i = 0; i < sz; i += 2) { uint32_t w = fetch16(c); b[i] = (uint8_t)(w >> 8); b[i + 1] = (uint8_t)w; }
    } else {
        ea_t e = ea(c, mode, reg, sz);
        if (e.kind != EA_MEM) { illegal(c); longjmp(c->abort, 2); }
        for (int i = 0; i < sz; i++) b[i] = (uint8_t)rd8(c, e.addr + (uint32_t)i);
    }
    switch (fmt) {
    case 0: return fv_int((int32_t)((uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3]));
    case 1: return fv_f32((uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3]);
    case 2: return from_ext(b);
    case 3: return from_packed(b);
    case 4: return fv_int((int16_t)(b[0] << 8 | b[1]));
    case 5: { uint64_t u = 0; for (int i = 0; i < 8; i++) u = u << 8 | b[i]; return fv_f64(u); }
    case 6: return fv_int((int8_t)b[0]);
    }
    return fv_int(0);
}

/* Write FPn in format fmt to <ea> (k: packed k-factor). */
static void fpu_write_ea(ac090 *c, int mode, int reg, int fmt, fpv v, int k) {
    uint8_t b[12];
    int sz = fsize[fmt];
    uint32_t u = 0;
    switch (fmt) {
    case 0: u = (uint32_t)to_int(c, v, INT32_MIN, INT32_MAX); break;
    case 1: u = fv_to_f32(v); break;
    case 4: u = (uint32_t)to_int(c, v, INT16_MIN, INT16_MAX) & 0xffff; break;
    case 6: u = (uint32_t)to_int(c, v, INT8_MIN, INT8_MAX) & 0xff; break;
    }
    if (mode == 0) {
        if (sz > 4) { illegal(c); longjmp(c->abort, 2); }
        uint32_t m = sz == 4 ? 0xffffffffu : sz == 2 ? 0xffffu : 0xffu;
        c->d[reg] = (c->d[reg] & ~m) | (u & m);
        return;
    }
    switch (fmt) {
    case 0: case 1: b[0] = (uint8_t)(u >> 24); b[1] = (uint8_t)(u >> 16); b[2] = (uint8_t)(u >> 8); b[3] = (uint8_t)u; break;
    case 4: b[0] = (uint8_t)(u >> 8); b[1] = (uint8_t)u; break;
    case 6: b[0] = (uint8_t)u; break;
    case 2: to_ext(v, b); break;
    case 3: case 7: to_packed(v, k, b); break;
    case 5: { uint64_t q = fv_to_f64(v); for (int i = 0; i < 8; i++) b[i] = (uint8_t)(q >> (56 - 8 * i)); break; }
    }
    ea_t e = ea(c, mode, reg, sz);
    if (e.kind != EA_MEM) { illegal(c); longjmp(c->abort, 2); }
    for (int i = 0; i < sz; i++) wr8(c, e.addr + (uint32_t)i, b[i]);
}

/* The FPSP's instructions (the transcendentals, FGETEXP, FGETMAN, FMOD,
 * FREM): a NaN out of operands that are not NaNs is an operand error. */
static int fpsp_operr(int base, fpv r, fpv d, fpv src) {
    switch (base) {
    case 0x02: case 0x06: case 0x08: case 0x09: case 0x0a: case 0x0c: case 0x0d: case 0x0e: case 0x0f:
    case 0x10: case 0x11: case 0x12: case 0x14: case 0x15: case 0x16: case 0x19: case 0x1c: case 0x1d:
    case 0x1e: case 0x1f:
        return fv_isnan(r) && !fv_isnan(src);
    case 0x21: case 0x25:
        return fv_isnan(r) && !fv_isnan(src) && !fv_isnan(d);
    }
    return (base & 0x78) == 0x30 && fv_isnan(r) && !fv_isnan(src);   /* FSINCOS */
}

/* The 68040's own arithmetic (27 Oct 2026): FADD, FSUB, FMUL, FDIV and
 * FSQRT (their FS and FD forms too), FSGLMUL and FSGLDIV. A NaN out of
 * operands that are not NaNs is an invalid operation (infinity minus
 * infinity, zero times infinity, zero by zero, infinity by infinity, the
 * root of a number below zero), which gives the default NaN as the
 * FPSP's operand errors do (fpsp_operr), whatever the precision: the
 * host's NaN there was the x87's indefinite (FFFF C000 0000 0000 0000),
 * on ARM64 7FFF C000 0000 0000 0000. The M68000 Family Programmer's
 * Reference Manual, 1.6.5: "When the FPU creates a NAN, the NAN always
 * contains the same bit pattern in the mantissa. All bits of the mantissa
 * are ones for any precision." The sign, clear, is the one the FPSP's and
 * the soft core's (amicradle-guest's libraries/open68040) default NaN
 * has; fpu_defnan_test.c. */
static int fpu_invalid(int base, fpv r, fpv d, fpv src) {
    switch (base) {
    case 0x04:
        return fv_isnan(r) && !fv_isnan(src);
    case 0x20: case 0x22: case 0x23: case 0x24: case 0x27: case 0x28:
        return fv_isnan(r) && !fv_isnan(src) && !fv_isnan(d);
    }
    return 0;
}

#if !AC090_FPU_SOFT
/* One arithmetic operation: dst op src into FPn. */
static void fpu_arith(ac090 *c, int opmode, long double src, int dn) {
    long double *d = &c->fp[dn], r = 0;
    int prec = 0, base = opmode;
    if (opmode & 0x40) {                                         /* 040 FSxxx (single), FDxxx (double) */
        prec = (opmode & 4) ? 2 : 1;
        base = opmode & ~0x44;
        if (base == 0x01) base = 0x04;                           /* FSSQRT/FDSQRT */
    }
    switch (base) {
    case 0x00: r = src; break;                                   /* FMOVE */
    case 0x01: if (!fx_special(0x01, 0, src, &r)) r = round_mode(c, src); break;   /* FINT */
    case 0x02: r = sinhl(src); break;
    case 0x03: if (!fx_special(0x03, 0, src, &r)) r = isnan(src) ? src + src : truncl(src); break;   /* FINTRZ; a NaN quiet (round_mode) */
    case 0x04: if (!fx_special(0x04, 0, src, &r)) r = sqrtl(src); break;           /* FSQRT */
    case 0x06: r = log1pl(src); break;                           /* FLOGNP1 */
    case 0x08: r = expm1l(src); break;                           /* FETOXM1 */
    case 0x09: r = tanhl(src); break;
    case 0x0a: r = atanl(src); break;
    case 0x0c: r = asinl(src); break;
    case 0x0d: r = atanhl(src); break;
    case 0x0e: r = sinl(src); break;
    case 0x0f: r = tanl(src); break;
    case 0x10: r = expl(src); break;
    case 0x11: r = exp2l(src); break;
    case 0x12: r = powl(10.0L, src); break;
    case 0x14: r = logl(src); break;
    case 0x15: r = log10l(src); break;
    case 0x16: r = log2l(src); break;
    case 0x18: r = fabsl(src); break;
    case 0x19: r = coshl(src); break;
    case 0x1a: r = -src; break;
    case 0x1c: r = acosl(src); break;
    case 0x1d: r = cosl(src); break;
    case 0x1e: r = (src == 0 || isinf(src) || isnan(src)) ? (isinf(src) ? NAN : src) : (long double)ilogbl(src); break;   /* FGETEXP */
    case 0x1f: if (src == 0 || isinf(src) || isnan(src)) r = isinf(src) ? NAN : src; else { int e; r = frexpl(src, &e) * 2; } break;   /* FGETMAN */
    case 0x20: if (!fx_special(0x20, *d, src, &r)) r = *d / src; break;            /* FDIV */
    case 0x21: case 0x25: {                                      /* FMOD (truncated), FREM (nearest) */
        long double q = opmode == 0x21 ? truncl(*d / src) : nearbyintl(*d / src);
        r = opmode == 0x21 ? fmodl(*d, src) : remainderl(*d, src);
        uint32_t qb = (uint32_t)((int64_t)fabsl(q) & 0x7f) | (signbit(q) ? 0x80 : 0);
        c->fpsr = (c->fpsr & ~0x00ff0000u) | qb << 16;
        break;
    }
    case 0x22: if (!fx_special(0x22, *d, src, &r)) r = *d + src; break;            /* FADD */
    case 0x23: if (!fx_special(0x23, *d, src, &r)) r = *d * src; break;            /* FMUL */
    case 0x24: r = (long double)(float)(*d / src); break;        /* FSGLDIV */
    case 0x26: r = ldexpl(*d, (int)truncl(src)); break;          /* FSCALE */
    case 0x27:                                                   /* FSGLMUL; of two NaNs the destination's, as the 68881 */
        r = isnan(*d) && isnan(src) ? (long double)(float)*d     /* picks it (GCC's SSE multiply gave that, Clang's x87 one not) */
                                    : (long double)(float)((long double)(float)*d * (long double)(float)src);
        break;
    case 0x28: if (!fx_special(0x28, *d, src, &r)) r = *d - src; break;            /* FSUB */
    case 0x38:                                                   /* FCMP */
        /* The FPU compares; it does not subtract. Two infinities of one
         * sign are equal (Z, and N if negative), where the subtraction
         * would give a NaN and every comparison would say "unordered".
         * WebKit's hash tables mark empty slots with +infinity: with the
         * NaN, no slot was ever empty and a lookup looped for ever (BBC's
         * page in GFOpenBrowser, 4 Oct 2026). */
        if (isinf(*d) && isinf(src) && signbit(*d) == signbit(src)) fpu_cc(c, signbit(*d) ? -0.0L : 0.0L);
        else if (fx_special(0x28, *d, src, &r)) fpu_cc(c, r);
        else fpu_cc(c, *d - src);
        return;
    case 0x3a: fpu_cc(c, src); return;                           /* FTST */
    default:
        if ((base & 0x78) == 0x30) {                           /* FSINCOS: sin to FPn, cos to FPc */
            long double co = cosl(src);
            c->fp[opmode & 7] = fpsp_operr(base, co, 0, src) ? fpu_default_nan() : fpu_round(c, co, prec);
            r = sinl(src);
            break;
        }
        illegal(c); longjmp(c->abort, 2);
    }
    r = fpsp_operr(base, r, *d, src) || fpu_invalid(base, r, *d, src) ? fpu_default_nan() : fpu_round(c, r, prec);
    *d = r;
    fpu_cc(c, r);
}

#endif

/* The 32 floating-point predicates (the signalling half as the quiet one),
 * on FPSR s. Translated code tests them from a table made with this. */
int ac090_fpu_cond_cc(uint32_t s, int p) {
    int n = !!(s & FPCC_N), z = !!(s & FPCC_Z), nan = !!(s & FPCC_NAN);
    switch (p & 15) {
    case 0: return 0;
    case 1: return z;                                  /* EQ */
    case 2: return !(nan || z || n);                   /* OGT */
    case 3: return z || !(nan || n);                   /* OGE */
    case 4: return n && !(nan || z);                   /* OLT */
    case 5: return z || (n && !nan);                   /* OLE */
    case 6: return !(nan || z);                        /* OGL */
    case 7: return !nan;                               /* OR */
    case 8: return nan;                                /* UN */
    case 9: return nan || z;                           /* UEQ */
    case 10: return nan || !(n || z);                  /* UGT */
    case 11: return nan || z || !n;                    /* UGE */
    case 12: return nan || (n && !z);                  /* ULT */
    case 13: return nan || z || n;                     /* ULE */
    case 14: return !z;                                /* NE */
    }
    return 1;                                          /* T */
}
static int fpu_cond(ac090 *c, int p) { return ac090_fpu_cond_cc(c->fpsr, p); }

/* AC090_TRACE_FPCR=1 logs every change of FPCR to stderr with the PC, the
 * Exec task and, for a CLI process, the command it is running: to find who
 * leaves FPCR's rounding precision set (4 Oct 2026, FPCR $40 before main()). */
static int trace_fpcr_on = -1;
static void trace_fpcr(ac090 *c, uint32_t old) {
    if (trace_fpcr_on < 0) trace_fpcr_on = getenv("AC090_TRACE_FPCR") != NULL;
    if (!trace_fpcr_on || c->fpcr == old) return;
    uint32_t eb = ac090_read(c, 4, 4), t = eb ? ac090_read(c, eb + 0x114, 4) : 0;
    char task[40] = "", cmd[40] = "";
    uint32_t nm = t ? ac090_read(c, t + 10, 4) : 0;
    for (int i = 0; nm && i < 39; i++) { uint8_t ch = (uint8_t)ac090_read(c, nm + (uint32_t)i, 1); if (!ch) break; task[i] = ch >= 32 && ch < 127 ? (char)ch : '?'; }
    if (t && ac090_read(c, t + 8, 1) == 13) {                   /* NT_PROCESS: pr_CLI -> cli_CommandName */
        uint32_t cli = ac090_read(c, t + 0xac, 4) << 2, bs = cli ? ac090_read(c, cli + 16, 4) << 2 : 0;
        int len = bs ? (int)ac090_read(c, bs, 1) : 0;
        for (int i = 0; i < len && i < 39; i++) { uint8_t ch = (uint8_t)ac090_read(c, bs + 1 + (uint32_t)i, 1); cmd[i] = ch >= 32 && ch < 127 ? (char)ch : '?'; }
    }
    fprintf(stderr, "ac090: FPCR %04x -> %04x at %08x, task %08x \"%s\" cmd \"%s\"\n",
            (unsigned)old, (unsigned)c->fpcr, (unsigned)c->op_pc, (unsigned)t, task, cmd);
}

static void fpu_reset(ac090 *c) {
    uint32_t old = c->fpcr;
    for (int i = 0; i < 8; i++) c->fp[i] = fv_nan();
    c->fpcr = c->fpsr = c->fpiar = 0;
    trace_fpcr(c, old);
}

/* FMOVEM of data registers: 12 bytes each, extended. */
static void fmovem_data(ac090 *c, uint16_t ext) {
    int to_mem = ext >> 13 & 1, m = ext >> 11 & 3, mode = MODE, reg = REG;
    uint8_t list = (m & 1) ? (uint8_t)c->d[ext >> 4 & 7] : (uint8_t)ext;
    int count = __builtin_popcount(list);
    if (m < 2) {                                     /* predecrement: -(An), list bit n = FPn */
        if (!to_mem || mode != 4) { illegal(c); return; }
        uint32_t a = c->a[reg] - 12u * (uint32_t)count, p = a;
        for (int i = 0; i < 8; i++) if (list & (1 << i)) { uint8_t b[12]; to_ext(c->fp[i], b); for (int k = 0; k < 12; k++) wr8(c, p + (uint32_t)k, b[k]); p += 12; }
        c->a[reg] = a;
        return;
    }
    uint32_t a;
    if (mode == 3) a = c->a[reg];
    else { ea_t e = ea(c, mode, reg, 4); if (e.kind != EA_MEM) { illegal(c); return; } a = e.addr; }
    for (int i = 0; i < 8; i++) if (list & (0x80 >> i)) {
        uint8_t b[12];
        if (to_mem) { to_ext(c->fp[i], b); for (int k = 0; k < 12; k++) wr8(c, a + (uint32_t)k, b[k]); }
        else { for (int k = 0; k < 12; k++) b[k] = (uint8_t)rd8(c, a + (uint32_t)k); c->fp[i] = from_ext(b); }
        a += 12;
    }
    if (mode == 3) c->a[reg] = a;
}

/* FMOVE(M) of FPCR/FPSR/FPIAR. */
static void fmovem_ctrl(ac090 *c, uint16_t ext) {
    uint32_t old_fpcr = c->fpcr;
    int to_mem = ext >> 13 & 1, list = ext >> 10 & 7, mode = MODE, reg = REG;
    uint32_t *r[3] = { &c->fpcr, &c->fpsr, &c->fpiar };   /* list bits 2, 1, 0 */
    int count = __builtin_popcount((unsigned)list);
    if (mode == 0 || mode == 1) {                          /* Dn/An: one register only */
        if (count != 1 || (mode == 1 && list != 1)) { illegal(c); return; }
        for (int i = 0; i < 3; i++) if (list & (4 >> i)) {
            if (to_mem) { if (mode == 0) c->d[reg] = *r[i]; else c->a[reg] = *r[i]; }
            else *r[i] = mode == 0 ? c->d[reg] : c->a[reg];
        }
        trace_fpcr(c, old_fpcr);
        return;
    }
    if (mode == 7 && reg == 4) {                           /* #imm to control registers */
        if (to_mem) { illegal(c); return; }
        for (int i = 0; i < 3; i++) if (list & (4 >> i)) *r[i] = fetch32(c);
        c->fpcr &= 0xfff0; trace_fpcr(c, old_fpcr); return;
    }
    uint32_t a;
    if (mode == 4) { c->a[reg] -= 4u * (uint32_t)count; a = c->a[reg]; }
    else if (mode == 3) { a = c->a[reg]; c->a[reg] += 4u * (uint32_t)count; }
    else { ea_t e = ea(c, mode, reg, 4); a = e.addr; }
    for (int i = 0; i < 3; i++) if (list & (4 >> i)) {
        if (to_mem) wr32(c, a, *r[i]); else *r[i] = rd32(c, a);
        a += 4;
    }
    c->fpcr &= 0xfff0;
    trace_fpcr(c, old_fpcr);
}

/* F-line, coprocessor 1: the FPU. */
static void op_fpu_general(ac090 *c) {
    uint16_t ext = (uint16_t)fetch16(c);
    int opclass = ext >> 13, rx = ext >> 10 & 7, ry = ext >> 7 & 7, opmode = ext & 0x7f;
    switch (opclass) {
    case 0:                                           /* FPm to FPn */
        c->fpiar = c->op_pc;
        fpu_arith(c, opmode, c->fp[rx], ry);
        return;
    case 2:
        c->fpiar = c->op_pc;
        if (rx == 7) {                                /* FMOVECR */
            c->fp[ry] = fpu_round(c, fmovecr(opmode), 0);
            fpu_cc(c, c->fp[ry]);
            return;
        }
        fpu_arith(c, opmode, fpu_read_ea(c, MODE, REG, rx), ry);
        return;
    case 3: {                                         /* FMOVE FPn to <ea> */
        c->fpiar = c->op_pc;
        int k = (int)(int8_t)(opmode << 1) >> 1;      /* static k-factor (7-bit signed) */
        if (rx == 7) k = (int)(int8_t)(c->d[opmode >> 4 & 7]);
        fpu_write_ea(c, MODE, REG, rx, c->fp[ry], k);
        return;
    }
    case 4: case 5: fmovem_ctrl(c, ext); return;
    case 6: case 7: fmovem_data(c, ext); return;
    }
    illegal(c);
}

static void op_fbcc(ac090 *c) {
    uint32_t base = c->pc;
    int32_t disp = (OP & 0x40) ? (int32_t)fetch32(c) : (int32_t)(int16_t)fetch16(c);
    if (fpu_cond(c, OP & 0x3f)) jump(c, base + (uint32_t)disp);
}
static void op_fscc(ac090 *c) {
    uint16_t ext = (uint16_t)fetch16(c);
    ea_t e = ea(c, MODE, REG, 1);
    ea_write(c, &e, 1, fpu_cond(c, ext & 0x3f) ? 0xff : 0);
}
static void op_fdbcc(ac090 *c) {
    uint16_t ext = (uint16_t)fetch16(c);
    uint32_t base = c->pc;
    int32_t disp = (int16_t)fetch16(c);
    if (fpu_cond(c, ext & 0x3f)) return;
    uint16_t w = (uint16_t)(c->d[REG] - 1);
    c->d[REG] = (c->d[REG] & 0xffff0000u) | w;
    if (w != 0xffff) jump(c, base + (uint32_t)disp);
}
static void op_ftrapcc(ac090 *c) {
    uint16_t ext = (uint16_t)fetch16(c);
    int m = REG;
    if (m == 2) fetch16(c); else if (m == 3) fetch32(c);
    if (fpu_cond(c, ext & 0x3f)) exception(c, V_TRAPV, c->pc);
}
/* FSAVE/FRESTORE, 68040 frames: null (FPU reset) or idle, 4 bytes. */
static void op_fsave(ac090 *c) {
    if (!c->s) { privilege(c); return; }
    ea_t e = ea(c, MODE, REG, 4);
    wr32(c, e.addr, 0x41000000u);                  /* idle frame, version 0x41 */
}
static void op_frestore(ac090 *c) {
    if (!c->s) { privilege(c); return; }
    uint32_t a;
    if (MODE == 3) { a = c->a[REG]; }
    else { ea_t e = ea(c, MODE, REG, 4); a = e.addr; }
    uint32_t w = rd32(c, a);
    if ((w >> 24) == 0) fpu_reset(c);              /* null frame */
    if (MODE == 3) c->a[REG] += 4 + (w >> 16 & 0xff);
}

/* Opmodes fpu_arith accepts. */
static int fpu_opmode_ok(int m) {
    if (m & 0x40) {
        int b = m & ~0x44;
        return b == 0x00 || b == 0x01 || b == 0x18 || b == 0x1a || b == 0x20 || b == 0x22 || b == 0x23 || b == 0x28;
    }
    if (m >= 0x30 && m <= 0x38) return 1;
    switch (m) {
    case 0x05: case 0x07: case 0x0b: case 0x13: case 0x17: case 0x1b: case 0x29: case 0x2a: case 0x2b:
    case 0x2c: case 0x2d: case 0x2e: case 0x2f: case 0x39: case 0x3b: case 0x3c: case 0x3d: case 0x3e: case 0x3f:
        return 0;
    }
    return m <= 0x3a;
}

/* For the translator: the FPU is there (a 68040, not AC090_NOFPU), and
 * AC090_TRACE_FPCR is off (writes to FPCR must then go through here). */
int ac090_fpu_on(const ac090 *c) { return c->model == AC090_68040 && fpu_present; }
int ac090_fpu_traced(void) { if (trace_fpcr_on < 0) trace_fpcr_on = getenv("AC090_TRACE_FPCR") != NULL; return trace_fpcr_on; }

/* For the translator: 1 if op/ext only touches registers and can take no
 * exception, so its handler can run inside a translated block with no setjmp:
 * FPm to FPn, FMOVECR, FMOVE between Dn and FPn (L/S/W/B), and FNOP. */
int ac090_fpu_inline_ok(const ac090 *c, uint16_t op, uint16_t ext) {
    if (c->model != AC090_68040 || !fpu_present) return 0;
    if (op == 0xf280) return ext == 0;                           /* FNOP */
    if ((op & 0xfff8) != 0xf200) return 0;
    int opclass = ext >> 13, rx = ext >> 10 & 7, dn_fmt = rx == 0 || rx == 1 || rx == 4 || rx == 6;
    switch (opclass) {
    case 0: return op == 0xf200 && fpu_opmode_ok(ext & 0x7f);
    case 2: if (rx == 7) return op == 0xf200;                    /* FMOVECR */
            return dn_fmt && fpu_opmode_ok(ext & 0x7f);          /* <op>.L/S/W/B Dn,FPn */
    case 3: return dn_fmt;                                       /* FMOVE.L/S/W/B FPn,Dn */
    }
    return 0;
}

/* An FPU instruction with a memory operand that translated code may run in
 * the block: <op>.fmt <ea>,FPn and FMOVE.fmt FPn,<ea> (not packed decimal).
 * *bytes is the operand size, for the instruction's length. */
int ac090_fpu_mem_ok(const ac090 *c, uint16_t op, uint16_t ext, int *bytes) {
    if (c->model != AC090_68040 || !fpu_present) return 0;
    if ((op & 0xffc0) != 0xf200) return 0;
    int mode = op >> 3 & 7, reg = op & 7, opclass = ext >> 13, rx = ext >> 10 & 7;
    if (mode < 2) return 0;
    *bytes = fsize[rx];
    if (opclass == 2) return rx != 7 && rx != 3 && fpu_opmode_ok(ext & 0x7f) && ea_ok(mode, reg, "m+-oxwlpi");
    if (opclass == 3) return rx != 7 && rx != 3 && ea_ok(mode, reg, "m+-oxwl");
    return 0;
}

/* FPm to FPn from translated code: the extension word and the address are
 * known when the block is built. */
void ac090_fpu_rr(ac090 *c, uint32_t ext, uint32_t pc) {
    c->fpiar = pc;
    fpu_arith(c, ext & 0x7f, c->fp[ext >> 10 & 7], ext >> 7 & 7);
}

/* FPm to FPn from translated code on the x87 when an operand is a NaN or
 * infinite: FADD, FSUB, FMUL, FDIV, FSQRT, FINT and FINTRZ (and their FS
 * and FD forms) worked out from the registers' bits (fpu_special.h), with
 * no long double loaded, so no x87 assist and no 80-bit store forwarded.
 * The result is fpu_arith's, bit for bit: the x87's answer, rounded to the
 * precision as the casts round it, and its condition codes. Anything else
 * goes to ac090_fpu_rr. */
void ac090_fpu_special_rr(ac090 *c, uint32_t ext, uint32_t pc) {
#if FX_HOST
    int m = ext & 0x7f, base = (m & 0x40) ? m & ~0x44 : m, prec = (m & 0x40) ? ((m & 4) ? 2 : 1) : 0;
    if ((m & 0x40) && base == 0x01) base = 0x04;                 /* FSSQRT/FDSQRT */
    int dn = ext >> 7 & 7;
    fx80 r;
    /* the commonest case first: two quiet NaNs (what a null FPU frame leaves
     * in every register) through FADD, FSUB, FMUL or FDIV. The larger
     * significand wins, else the positive one; quiet already */
    if (base == 0x22 || base == 0x28 || base == 0x23 || base == 0x20) {
        fx80 a = fx_load(&c->fp[dn]), b = fx_load(&c->fp[ext >> 10 & 7]);
        if ((a.se & 0x7fff) == 0x7fff && (b.se & 0x7fff) == 0x7fff && a.m >> 62 == 3 && b.m >> 62 == 3) {
            r = a.m != b.m ? (a.m > b.m ? a : b) : ((a.se & 0x8000) ? b : a);
            c->fpiar = pc;
            if (prec == 0) prec = c->fpcr >> 6 & 3;
            r = fx_round_bits(r, prec);
            fx_store(&c->fp[dn], r);
            c->fpsr = (c->fpsr & 0x00ffffffu) | FPCC_NAN | (r.se & 0x8000 ? FPCC_N : 0);
            return;
        }
    }
    int k;
    if ((base == 0x22 || base == 0x28 || base == 0x23 || base == 0x20 || base == 0x04 || base == 0x01 || base == 0x03) &&
        (k = fx_special_bits(base, fx_load(&c->fp[dn]), fx_load(&c->fp[ext >> 10 & 7]), &r))) {
        c->fpiar = pc;
        if (prec == 0) prec = c->fpcr >> 6 & 3;
        if (k == 1) r = fx_round_bits(r, prec);                  /* the default NaN (2) is not rounded */
        fx_store(&c->fp[dn], r);
        int e = r.se & 0x7fff;
        uint32_t cc = e == 0x7fff ? ((r.m << 1) ? FPCC_NAN : FPCC_I) : (!e && !r.m ? FPCC_Z : 0);
        if (r.se & 0x8000) cc |= FPCC_N;
        c->fpsr = (c->fpsr & 0x00ffffffu) | cc;
        return;
    }
#endif
    ac090_fpu_rr(c, ext, pc);
}

/* An extended value in fp_tmp as three big-endian longs held as host longs
 * (FMOVEM.X and K_FTMP on a host without the x87): from them, and into
 * them. As fmovem_data does it (to_ext, from_ext). */
static fpv tmp_x_in(const ac090 *c) {
    uint8_t b[12];
    uint32_t w[3];
    memcpy(w, &c->fp_tmp, 12);
    for (int k = 0; k < 3; k++) { b[4 * k] = (uint8_t)(w[k] >> 24); b[4 * k + 1] = (uint8_t)(w[k] >> 16); b[4 * k + 2] = (uint8_t)(w[k] >> 8); b[4 * k + 3] = (uint8_t)w[k]; }
    return from_ext(b);
}
static void tmp_x_out(ac090 *c, fpv v) {
    uint8_t b[12];
    uint32_t w[3];
    to_ext(v, b);
    for (int k = 0; k < 3; k++) w[k] = (uint32_t)b[4 * k] << 24 | (uint32_t)b[4 * k + 1] << 16 | (uint32_t)b[4 * k + 2] << 8 | b[4 * k + 3];
    memcpy(&c->fp_tmp, w, 12);
}

/* For translated code, an FPU instruction with a memory operand in L, S,
 * X, W, B or D, the operand moved by translated loads and stores through
 * fp_tmp, in host order (L and S as 32 bits, W and B as 16, D as 64; X on
 * the x87 as the value itself, which jit.c's x87_fetch has made from the
 * 12 bytes, and elsewhere as three longs): what op_fpu_general does with
 * it. On ARM64 every such instruction; on the x87 those whose operation it
 * does not do (FSGLDIV, FMOD, the transcendentals...), and those with a
 * NaN or infinite operand. <op>.fmt <ea>,FPn, the operand in fp_tmp: */
void ac090_fpu_tmp_op(ac090 *c, uint32_t ext, uint32_t pc) {
    const uint8_t *t = (const uint8_t *)&c->fp_tmp;
    uint32_t u32; uint16_t u16; uint64_t u64;
    fpv src = fv_int(0);
    switch (ext >> 10 & 7) {
    case 0: memcpy(&u32, t, 4); src = fv_int((int32_t)u32); break;
    case 1: memcpy(&u32, t, 4); src = fv_f32(u32); break;
#if !AC090_FPU_SOFT
    case 2: src = c->fp_tmp; break;
#else
    case 2: src = tmp_x_in(c); break;
#endif
    case 4: case 6: memcpy(&u16, t, 2); src = fv_int((int16_t)u16); break;   /* a byte comes widened */
    case 5: memcpy(&u64, t, 8); src = fv_f64(u64); break;
    }
    c->fpiar = pc;
    fpu_arith(c, (int)(ext & 0x7f), src, (int)(ext >> 7 & 7));
}
/* FMOVE.fmt FPn,<ea>: the value into fp_tmp, for translated stores (on a
 * host without the x87; jit.c's x87_store makes its own). */
void ac090_fpu_tmp_store(ac090 *c, uint32_t ext, uint32_t pc) {
    uint8_t *t = (uint8_t *)&c->fp_tmp;
    fpv v = c->fp[ext >> 7 & 7];
    uint32_t u = 0;
    c->fpiar = pc;
    switch (ext >> 10 & 7) {
    case 0: u = (uint32_t)to_int(c, v, INT32_MIN, INT32_MAX); break;
    case 4: u = (uint32_t)to_int(c, v, INT16_MIN, INT16_MAX) & 0xffff; break;
    case 6: u = (uint32_t)to_int(c, v, INT8_MIN, INT8_MAX) & 0xff; break;
    case 1: u = fv_to_f32(v); break;
    case 2: tmp_x_out(c, v); return;
    case 5: { uint64_t q = fv_to_f64(v); memcpy(t, &q, 8); return; }
    }
    memcpy(t, &u, 4);
}

/* FMOVEM.X from translated code on a host without the x87: FPn as the 12
 * bytes FMOVEM writes, three big-endian longs in fp_tmp as host longs;
 * and back. */
void ac090_fpu_x_out(ac090 *c, uint32_t fp) { tmp_x_out(c, c->fp[fp & 7]); }
void ac090_fpu_x_in(ac090 *c, uint32_t fp) { c->fp[fp & 7] = tmp_x_in(c); }

static handler fpu_decode(uint16_t op) {
    int mode = op >> 3 & 7, reg = op & 7;
    switch (op & 0x01c0) {
    case 0x0000: return op_fpu_general;
    case 0x0040:
        if (mode == 1) return op_fdbcc;
        if (mode == 7 && reg >= 2) return reg <= 4 ? op_ftrapcc : 0;
        return ea_ok(mode, reg, DALT) ? op_fscc : 0;
    case 0x0080: case 0x00c0: return op_fbcc;
    case 0x0100: return ea_ok(mode, reg, "m-oxwl") ? op_fsave : 0;
    case 0x0140: return ea_ok(mode, reg, "m+oxwlp") ? op_frestore : 0;
    }
    return 0;
}
