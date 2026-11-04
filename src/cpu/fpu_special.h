/*
 * The x87's results for NaN and infinite operands, worked out from the bits
 * instead of by the x87 itself. On these an x87 FADD, FSUB, FMUL, FDIV,
 * FSQRT or FRNDINT takes a microcode assist: 120-130 ns each on the cloud
 * Xeon, against about 1 ns. SysInfo's MFLOPS loop adds and subtracts the
 * NaNs a 68040's registers hold after Kickstart 3.2.3 restores a null FPU
 * frame, so it ran at 51 MFLOPS where AROS (ordinary numbers in the
 * registers) gave 661.
 *
 * The answers are the x87's own, bit for bit (fpu_special_test.c holds them
 * to the hardware): a NaN operand comes out quiet; of two NaNs the quiet one
 * wins, else the larger significand, else the positive one; infinities by
 * IEEE. But an invalid operation (infinity minus infinity, zero times
 * infinity, infinity by infinity, the root of minus infinity) gives the
 * 68040's default NaN, 7FFF FFFFFFFF FFFFFFFF, where the x87 gives its
 * indefinite (27 Oct 2026; fpu_defnan_test.c). Only for x87
 * hosts, where long double is the x87's 80-bit format; elsewhere nothing is
 * worked out here. Odd encodings (pseudo-NaNs, unnormals) are left to the
 * x87 too.
 */
#ifndef FPU_SPECIAL_H
#define FPU_SPECIAL_H
#include <stdint.h>
#include <string.h>

#if defined(__x86_64__) || defined(__i386__)
#define FX_HOST 1
#define FX_INLINE static inline __attribute__((always_inline))   /* the JIT's NaN path calls these per instruction */

typedef struct { uint64_t m; uint16_t se; } fx80;     /* significand, sign and exponent */
FX_INLINE fx80 fx_load(const void *p) { fx80 x; memcpy(&x.m, p, 8); memcpy(&x.se, (const char *)p + 8, 2); return x; }
FX_INLINE void fx_store(void *p, fx80 x) { memcpy(p, &x.m, 8); memcpy((char *)p + 8, &x.se, 2); }
FX_INLINE fx80 fx_of(long double v) { return fx_load(&v); }
FX_INLINE long double fx_ld(fx80 x) { long double v = 0; fx_store(&v, x); return v; }
FX_INLINE fx80 fx_bits(uint64_t m, uint16_t se) { fx80 x = { m, se }; return x; }
FX_INLINE long double fx_make(uint64_t m, uint16_t se) { return fx_ld(fx_bits(m, se)); }
enum { FX_FINITE, FX_ZERO, FX_INF, FX_QNAN, FX_SNAN, FX_ODD };
FX_INLINE int fx_class(fx80 x) {
    int e = x.se & 0x7fff, ib = (int)(x.m >> 63);
    if (e == 0x7fff) {
        if (!ib) return FX_ODD;                          /* pseudo-NaN or pseudo-infinity */
        if (!(x.m << 1)) return FX_INF;
        return (x.m >> 62 & 1) ? FX_QNAN : FX_SNAN;
    }
    if (e == 0) return x.m ? FX_FINITE : FX_ZERO;        /* a denormal is finite */
    return ib ? FX_FINITE : FX_ODD;                      /* an unnormal */
}
FX_INLINE fx80 fx_inf_b(int neg) { return fx_bits(1ull << 63, (uint16_t)(neg ? 0xffff : 0x7fff)); }
FX_INLINE fx80 fx_zero_b(int neg) { return fx_bits(0, (uint16_t)(neg ? 0x8000 : 0)); }
FX_INLINE fx80 fx_indefinite_b(void) { return fx_bits(0xc000000000000000ull, 0xffff); }
/* the 68040's default NaN: sign clear, every significand bit set */
FX_INLINE fx80 fx_defnan_b(void) { return fx_bits(0xffffffffffffffffull, 0x7fff); }
FX_INLINE fx80 fx_quiet_b(fx80 x) { return fx_bits(x.m | 1ull << 62, x.se); }
FX_INLINE long double fx_inf(int neg) { return fx_ld(fx_inf_b(neg)); }
FX_INLINE long double fx_zero(int neg) { return fx_ld(fx_zero_b(neg)); }
FX_INLINE long double fx_indefinite(void) { return fx_ld(fx_indefinite_b()); }

/* The NaN out of two operands, at least one of them a NaN. */
FX_INLINE fx80 fx_nan2(fx80 a, int ca, fx80 b, int cb) {
    int an = ca == FX_QNAN || ca == FX_SNAN, bn = cb == FX_QNAN || cb == FX_SNAN;
    if (!(an && bn)) return fx_quiet_b(an ? a : b);
    if (ca != cb) return fx_quiet_b(ca == FX_QNAN ? a : b);       /* quiet beats signalling */
    if (a.m != b.m) return fx_quiet_b(a.m > b.m ? a : b);         /* the larger significand */
    return fx_quiet_b((a.se & 0x8000) ? b : a);                    /* equal: the positive one */
}

/* *r for an operation on a (the destination) and b (the source) when one of
 * them is a NaN or infinite, as the x87 gives it, but for an invalid
 * operation: the 68040's default NaN, and 2 (it is not rounded to single
 * or double: an FSADD's is the same); 0 when neither is, or for an encoding
 * the x87 should see. op: 0x22 FADD, 0x28 FSUB (a - b), 0x23 FMUL, 0x20
 * FDIV (a / b), 0x04 FSQRT (b), 0x01 FINT and 0x03 FINTRZ (b). */
FX_INLINE int fx_special_bits(int op, fx80 a, fx80 b, fx80 *r) {
    int unary = op == 0x04 || op == 0x01 || op == 0x03;
    int ca = unary ? FX_FINITE : fx_class(a), cb = fx_class(b);
    if (ca == FX_ODD || cb == FX_ODD) return 0;
    if (ca == FX_QNAN || ca == FX_SNAN || cb == FX_QNAN || cb == FX_SNAN) {
        *r = unary ? fx_quiet_b(b) : fx_nan2(a, ca, b, cb);
        return 1;
    }
    if (ca != FX_INF && cb != FX_INF) return 0;
    int sa = a.se >> 15, sb = b.se >> 15;
    switch (op) {
    case 0x28: sb ^= 1;                                  /* a - b is a + -b */
        /* fall through */
    case 0x22:
        if (ca == FX_INF && cb == FX_INF && sa != sb) { *r = fx_defnan_b(); return 2; }
        *r = fx_inf_b(ca == FX_INF ? sa : sb);
        return 1;
    case 0x23:
        if (ca == FX_ZERO || cb == FX_ZERO) { *r = fx_defnan_b(); return 2; }
        *r = fx_inf_b(sa ^ sb);
        return 1;
    case 0x20:
        if (ca == FX_INF && cb == FX_INF) { *r = fx_defnan_b(); return 2; }
        *r = ca == FX_INF ? fx_inf_b(sa ^ sb) : fx_zero_b(sa ^ sb);
        return 1;
    case 0x04: if (sb) { *r = fx_defnan_b(); return 2; } *r = fx_inf_b(0); return 1;
    case 0x01: case 0x03: *r = b; return 1;              /* an infinity is whole */
    }
    return 0;
}
FX_INLINE int fx_special(int op, long double d, long double s, long double *r) {
    fx80 x;
    if (!fx_special_bits(op, fx_of(d), fx_of(s), &x)) return 0;
    *r = fx_ld(x);
    return 1;
}
/* A NaN, infinity or zero rounded to single (1) or double (2) and back, as
 * (long double)(float) and (long double)(double) do it: a NaN keeps the top
 * 23 or 52 bits of its fraction. */
FX_INLINE fx80 fx_round_bits(fx80 x, int prec) {
    if ((x.se & 0x7fff) == 0x7fff && (x.m << 1)) {
        if (prec == 1) x.m &= 0xffffff0000000000ull;
        else if (prec == 2) x.m &= 0xfffffffffffff800ull;
    }
    return x;
}
#else
#define FX_HOST 0
static inline int fx_special(int op, long double d, long double s, long double *r) { (void)op; (void)d; (void)s; (void)r; return 0; }
#endif
#endif
