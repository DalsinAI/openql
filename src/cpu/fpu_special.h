/*
 * The 68881's extended values worked out from their bits.
 *
 * The x87's results for NaN and infinite operands, instead of by the x87
 * itself. On these an x87 FADD, FSUB, FMUL, FDIV, FSQRT or FRNDINT takes a
 * microcode assist: 120-130 ns each on the cloud Xeon, against about 1 ns.
 * SysInfo's MFLOPS loop adds and subtracts the NaNs a 68040's registers
 * hold after Kickstart 3.2.3 restores a null FPU frame, so it ran at 51
 * MFLOPS where AROS (ordinary numbers in the registers) gave 661.
 *
 * The answers are the x87's own, bit for bit: a NaN operand comes out
 * quiet; of two NaNs the quiet one wins, else the larger significand, else
 * the positive one; infinities by IEEE. But an invalid operation (infinity
 * minus infinity, zero times infinity, infinity by infinity, the root of
 * minus infinity) gives the 68040's default NaN, 7FFF FFFFFFFF FFFFFFFF,
 * where the x87 gives its indefinite (27 Oct 2026). Odd encodings
 * (pseudo-NaNs, unnormals) are left to the x87.
 *
 * And the arithmetic itself, in integers, for hosts with no 80-bit long
 * double (ARM64): FADD, FSUB, FMUL, FDIV, FSQRT, FMOD, FREM, FSCALE, FINT
 * and the roundings to single and double, each rounded once to a 64-bit
 * significand as the x87 and the 68040 round it. This is AmiCradle's AC090
 * work for the Raspberry Pi 5 (Team 5, 9 Nov 2026, machines/cpus/m68k-ac090/
 * native/fpu_special.h there), where it replaced libgcc's soft quad;
 * fpu_soft.c builds OpenQL's whole FPU on it.
 */
#ifndef FPU_SPECIAL_H
#define FPU_SPECIAL_H
#include <stdint.h>
#include <string.h>

#define FX_INLINE static inline __attribute__((always_inline))
typedef ac090_fx80 fx80;                              /* significand, sign and exponent */
FX_INLINE fx80 fx_bits(uint64_t m, uint16_t se) { fx80 x = { m, se }; return x; }
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
/* Round an extended value to IEEE single (prec 1) or double (prec 2), then
 * return the exact extended representation of that value. This is what a
 * float/double cast followed by a long-double conversion does, including
 * the format's own denormal and exponent range. Nearest, ties to even: the
 * host path uses the default C floating-point mode for these casts too. */
FX_INLINE fx80 fx_format(fx80 x, int prec) {
    int c = fx_class(x);
    if (c == FX_ZERO || c == FX_INF) return x;
    if (c == FX_QNAN || c == FX_SNAN) return fx_round_bits(fx_quiet_b(x), prec);
    if (c == FX_ODD || (prec != 1 && prec != 2)) return x;
    int p = prec == 1 ? 24 : 53;
    int emin = prec == 1 ? -126 : -1022, emax = prec == 1 ? 127 : 1023;
    int e = x.se & 0x7fff;
    uint64_t m = x.m;
    if (!e) { int z = __builtin_clzll(m); m <<= z; e = 1 - z; }
    int ue = e - 0x3fff, sign = x.se >> 15;
    if (ue > emax) return fx_inf_b(sign);
    if (ue >= emin) {
        int d = 64 - p;
        uint64_t q = m >> d, rest = m & ((1ull << d) - 1), half = 1ull << (d - 1);
        if (rest > half || (rest == half && (q & 1))) {
            if (++q == 1ull << p) { q >>= 1; if (++ue > emax) return fx_inf_b(sign); }
        }
        return fx_bits(q << d, (uint16_t)(sign << 15 | (ue + 0x3fff)));
    }
    int d = 64 - p + emin - ue;
    uint64_t q = 0;
    if (d < 64) {
        q = m >> d;
        uint64_t rest = m & ((1ull << d) - 1), half = 1ull << (d - 1);
        if (rest > half || (rest == half && (q & 1))) q++;
    } else if (d == 64 && m > 1ull << 63) q = 1;    /* exactly half rounds to even zero */
    if (!q) return fx_zero_b(sign);
    int bit = 63 - __builtin_clzll(q);
    return fx_bits(q << (63 - bit), (uint16_t)(sign << 15 | (0x3fff + emin - (p - 1) + bit)));
}

/* Round a finite extended value to an integer in extended format. mode is
 * FPCR 5-4: nearest/even, zero, -infinity, +infinity. */
FX_INLINE fx80 fx_integer(fx80 x, int mode) {
    int c = fx_class(x);
    if (c == FX_ZERO || c == FX_INF) return x;
    if (c == FX_QNAN || c == FX_SNAN) return fx_quiet_b(x);
    if (c == FX_ODD) return fx_defnan_b();
    int e = x.se & 0x7fff, sign = x.se >> 15;
    uint64_t m = x.m;
    if (!e) { int z = __builtin_clzll(m); m <<= z; e = 1 - z; }
    if (e >= 0x3fff + 63) return x;
    if (e < 0x3fff) {
        int up = mode == 2 ? sign : mode == 3 ? !sign
               : mode == 0 && (e > 0x3ffe || (e == 0x3ffe && m > 1ull << 63));
        return up ? fx_bits(1ull << 63, (uint16_t)(sign << 15 | 0x3fff)) : fx_zero_b(sign);
    }
    int f = 0x3fff + 63 - e;
    uint64_t q = m >> f, rest = m & ((1ull << f) - 1), half = 1ull << (f - 1);
    int up = mode == 2 ? sign && rest : mode == 3 ? !sign && rest
           : mode == 0 && (rest > half || (rest == half && (q & 1)));
    if (up && ++q == 1ull << (64 - f)) return fx_bits(1ull << 63, (uint16_t)(sign << 15 | (e + 1)));
    return fx_bits(q << f, (uint16_t)(sign << 15 | e));
}

/* FADD, FSUB, FMUL and FDIV of two finite nonzero normal or denormal
 * numbers at extended precision,
 * in integers (Team 5, 9 Nov 2026): the result rounded once, to nearest
 * (ties to even), to a 64-bit significand, as the 68040 and the x87 do.
 * The result may be signed zero, denormal, normal or infinity. 1 and *r,
 * or 0 when an operand is zero, infinity, NaN or an odd encoding: the
 * caller's own way then. For ARM64, whose long
 * double is soft-float quad: there an FADD was libgcc's __addtf3 and the
 * checks around it as many calls again, and SysInfo's MFLOPS loop ran at
 * 62 MFLOPS on a Pi 5 at 2 GHz. fpu_fast_test.c holds every result to the
 * x87's on x86-64 (and to the quad's, rounded, on ARM64). */
typedef unsigned __int128 fx_u128;
/* 128-bit shifts by a variable count as two 64-bit ones (GCC calls
 * libgcc's __lshrti3 and __ashlti3 for them on ARM64); d from 0 to 127 */
FX_INLINE fx_u128 fx_shr(fx_u128 v, int d) {
    uint64_t hi = (uint64_t)(v >> 64), lo = (uint64_t)v;
    if (d >= 64) { lo = hi >> (d - 64); hi = 0; }
    else if (d) { lo = lo >> d | hi << (64 - d); hi >>= d; }
    return (fx_u128)hi << 64 | lo;
}
FX_INLINE fx_u128 fx_shl(fx_u128 v, int d) {
    uint64_t hi = (uint64_t)(v >> 64), lo = (uint64_t)v;
    if (d >= 64) { hi = lo << (d - 64); lo = 0; }
    else if (d) { hi = hi << d | lo >> (64 - d); lo <<= d; }
    return (fx_u128)hi << 64 | lo;
}
FX_INLINE int fx_clz128(fx_u128 v) { uint64_t h = (uint64_t)(v >> 64); return h ? __builtin_clzll(h) : 64 + __builtin_clzll((uint64_t)v); }
/* n normalised (bit 127 set; bit 0 may hold the sticky bit), the value n / 2^127 x 2^(e - 16383) */
FX_INLINE int fx_pack(int sign, int32_t e, fx_u128 n, fx80 *r) {
    if (e <= 0) {
        int d = 1 - e;
        if (d >= 128) { *r = fx_zero_b(sign); return 1; }
        int sticky = fx_shl(n, 128 - d) != 0;
        n = fx_shr(n, d) | (fx_u128)sticky;
        uint64_t m = (uint64_t)(n >> 64), rest = (uint64_t)n;
        if (rest > 1ull << 63 || (rest == 1ull << 63 && (m & 1))) m++;
        if (!m) { *r = fx_zero_b(sign); return 1; }
        r->m = m; r->se = (uint16_t)(sign << 15 | (m >> 63));
        return 1;
    }
    uint64_t m = (uint64_t)(n >> 64), rest = (uint64_t)n;
    if (rest > 1ull << 63 || (rest == 1ull << 63 && (m & 1))) { if (!++m) { m = 1ull << 63; e++; } }
    if (e >= 0x7fff) { *r = fx_inf_b(sign); return 1; }
    r->m = m; r->se = (uint16_t)(sign << 15 | e);
    return 1;
}
FX_INLINE int fx_normal(fx80 x) { int e = x.se & 0x7fff; return e && e != 0x7fff && (x.m >> 63); }
FX_INLINE int fx_operand(fx80 *x, int32_t *e) {
    int32_t xe = x->se & 0x7fff;
    if (xe == 0x7fff || !x->m || (xe && !(x->m >> 63))) return 0;
    if (!xe) { int z = __builtin_clzll(x->m); x->m <<= z; xe = 1 - z; }
    *e = xe; return 1;
}
FX_INLINE int fx_arith_fast(int op, fx80 a, fx80 b, fx80 *r) {
    int32_t ea, eb;
    if (!fx_operand(&a, &ea) || !fx_operand(&b, &eb)) return 0;
    int sa = a.se >> 15, sb = b.se >> 15;
    switch (op) {
    case 0x28: sb ^= 1;                                  /* a - b is a + -b */
        /* fall through */
    case 0x22: {
        fx_u128 A = (fx_u128)a.m << 64, B = (fx_u128)b.m << 64;
        if (eb > ea || (eb == ea && b.m > a.m)) { fx_u128 t = A; A = B; B = t; int32_t te = ea; ea = eb; eb = te; int ts = sa; sa = sb; sb = ts; }
        int32_t d = ea - eb;
        int sticky = 0;
        if (d >= 128) { sticky = 1; B = 0; }
        else if (d) { sticky = fx_shl(B, 128 - d) != 0; B = fx_shr(B, d); }
        fx_u128 S;
        if (sa == sb) {
            S = A + B;
            if (S < A) { sticky |= (int)(S & 1); S = S >> 1 | (fx_u128)1 << 127; ea++; }   /* the carry */
        } else {
            S = A - B - (fx_u128)sticky;                 /* the exact difference lies between S and S + 1 */
            if (!S) { *r = fx_bits(0, 0); return 1; }    /* x - x: +0, to nearest */
            int z = fx_clz128(S);
            S = fx_shl(S, z); ea -= z;
        }
        return fx_pack(sa, ea, S | (fx_u128)sticky, r);
    }
    case 0x23: {
        fx_u128 P = (fx_u128)a.m * b.m;
        int32_t e = ea + eb - 16383;
        if (P >> 127) e++; else P <<= 1;
        return fx_pack(sa ^ sb, e, P, r);
    }
    case 0x20: {
        int hi = a.m >= b.m;
        uint64_t r0 = hi ? a.m - b.m : a.m;
        fx_u128 n1 = (fx_u128)r0 << 64;
        uint64_t q1 = (uint64_t)(n1 / b.m), r1 = (uint64_t)(n1 % b.m);
        fx_u128 n2 = (fx_u128)r1 << 64;
        uint64_t q2 = (uint64_t)(n2 / b.m), r2 = (uint64_t)(n2 % b.m);
        fx_u128 Q;
        int sticky = r2 != 0;
        if (hi) { Q = (fx_u128)1 << 127 | (fx_u128)q1 << 63 | q2 >> 1; sticky |= (int)(q2 & 1); }
        else Q = (fx_u128)q1 << 64 | q2;
        return fx_pack(sa ^ sb, ea - eb + 16383 - !hi, Q | (fx_u128)sticky, r);
    }
    }
    return 0;
}
FX_INLINE int fx_scale_fast(fx80 a, int n, fx80 *r) {
    int32_t e;
    if (!fx_operand(&a, &e)) return 0;
    return fx_pack(a.se >> 15, e + n, (fx_u128)a.m << 64, r);
}
FX_INLINE int fx_pack_mant(int sign, int32_t e, uint64_t m, fx80 *r) {
    return fx_pack(sign, e, (fx_u128)m << 64, r);
}
FX_INLINE int fx_sqrt_round(uint64_t m, int odd, uint64_t *root) {
    fx_u128 n = (fx_u128)m << (odd ? 64 : 63), res = 0, bit = (fx_u128)1 << 126;
    while (bit) {
        if (n >= res + bit) { n -= res + bit; res = (res >> 1) + bit; }
        else res >>= 1;
        bit >>= 2;
    }
    *root = (uint64_t)res;
    return n > *root;
}
/* FMOD/FREM of two finite nonzero extended values. The quotient is formed
 * a bit at a time, so the full exponent range works. FREM increments the
 * truncated quotient when the fraction is above one half, or at an exact
 * half when that quotient is odd. */
FX_INLINE int fx_modrem(fx80 a, fx80 b, int nearest, fx80 *r, uint8_t *qbyte) {
    int32_t ea, eb;
    if (!fx_operand(&a, &ea) || !fx_operand(&b, &eb)) return 0;
    int sa = a.se >> 15, sb = b.se >> 15, rsign = sa;
    int32_t d = ea - eb, er;
    uint64_t rem;
    uint8_t q = 0;
    if (d < 0) {
        rem = a.m; er = ea;
        if (nearest && d == -1 && a.m > b.m) {
            q = 1;
            rem = (b.m << 1) - a.m;
            rsign ^= 1;
        }
    } else {
        q = (uint8_t)(a.m / b.m);
        rem = a.m % b.m;
        for (int32_t i = 0; i < d; i++) {
            int carry = (int)(rem >> 63);
            rem <<= 1;
            int bit = carry || rem >= b.m;
            if (bit) rem -= b.m;
            q = (uint8_t)((q << 1 | bit) & 0x7f);
        }
        er = eb;
        if (nearest && rem) {
            int carry = (int)(rem >> 63);
            uint64_t twice = rem << 1;
            int above = carry || twice > b.m;
            int half = !carry && twice == b.m;
            if (above || (half && (q & 1))) {
                q = (uint8_t)((q + 1) & 0x7f);
                rem = b.m - rem;
                rsign ^= 1;
            }
        }
    }
    *qbyte = (uint8_t)(q | ((sa ^ sb) << 7));
    if (!rem) { *r = fx_zero_b(sa); return 1; }
    int z = __builtin_clzll(rem);
    return fx_pack_mant(rsign, er - z, rem << z, r);
}
/* Square root of one positive finite extended value. After making the
 * unbiased exponent even, the required 64-bit significand is the integer
 * square root of at most 128 bits. The restoring square-root remainder
 * decides round-to-nearest; an exact half cannot occur for an integer
 * radicand and a half-integer root. */
FX_INLINE int fx_sqrt_fast(fx80 x, fx80 *r) {
    int32_t e;
    if (!fx_operand(&x, &e)) return 0;
    int32_t ue = e - 0x3fff;
    int odd = ue & 1;
    if (odd) ue--;
    uint64_t q;
    int up = fx_sqrt_round(x.m, odd, &q);
    int32_t re = 0x3fff + ue / 2;
    if (up && ++q == 0) { q = 1ull << 63; re++; }
    return fx_pack_mant(0, re, q, r);
}
/* The FPSR condition codes of x (N, Z, I, NaN in bits 27-24), from its bits */
FX_INLINE uint32_t fx_cc(fx80 x) {
    int e = x.se & 0x7fff;
    uint32_t cc = e == 0x7fff ? ((x.m << 1) ? 0x01000000u : 0x02000000u) : (!e && !x.m ? 0x04000000u : 0);
    return cc | (x.se & 0x8000 ? 0x08000000u : 0);
}

#if !AC090_FPU_SOFT
/* x87 hosts: long double is the x87's 80-bit format, FP0-FP7 are held in it */
#define FX_HOST 1
FX_INLINE fx80 fx_load(const void *p) { fx80 x; memcpy(&x.m, p, 8); memcpy(&x.se, (const char *)p + 8, 2); return x; }
FX_INLINE void fx_store(void *p, fx80 x) { memcpy(p, &x.m, 8); memcpy((char *)p + 8, &x.se, 2); }
FX_INLINE fx80 fx_of(long double v) { return fx_load(&v); }
FX_INLINE long double fx_ld(fx80 x) { long double v = 0; fx_store(&v, x); return v; }
FX_INLINE long double fx_make(uint64_t m, uint16_t se) { return fx_ld(fx_bits(m, se)); }
FX_INLINE long double fx_inf(int neg) { return fx_ld(fx_inf_b(neg)); }
FX_INLINE long double fx_zero(int neg) { return fx_ld(fx_zero_b(neg)); }
FX_INLINE long double fx_indefinite(void) { return fx_ld(fx_indefinite_b()); }
FX_INLINE int fx_special(int op, long double d, long double s, long double *r) {
    fx80 x;
    if (!fx_special_bits(op, fx_of(d), fx_of(s), &x)) return 0;
    *r = fx_ld(x);
    return 1;
}
#else
#define FX_HOST 0
#endif
#endif
