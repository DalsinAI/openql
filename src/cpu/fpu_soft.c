/*
 * AC090's FPU on a host with no 80-bit long double: Apple Silicon, where
 * long double is binary64, and Linux on ARM64, where it is IEEE quad in
 * software. Included by ac090_fpu.c in place of its x87 half.
 *
 * FP0-FP7 are held as the 68881's extended values themselves, 64-bit
 * significand and 16-bit sign and exponent (ac090_fx80), and every
 * instruction gives the bits the x87 build gives:
 *
 * - FADD, FSUB, FMUL, FDIV, FSQRT, FMOD, FREM, FSCALE, FINT, FINTRZ,
 *   FGETEXP, FGETMAN, FCMP, the FS and FD forms, FSGLMUL and FSGLDIV, the
 *   conversions to and from L, W, B, S, D and X, FMOVECR, and packed
 *   decimal in and out: worked out in integers (fpu_special.h, the Pi 5
 *   work), each result rounded once as the x87 rounds it, NaNs and
 *   infinities as the x87 has them. The same bits as the x87 build, which
 *   tests/fpu_test.c checks on every run.
 * - The transcendentals (FSIN to FTENTOX): in double-double, about 106
 *   bits, so within about 2^-100 of the true value, then rounded once to
 *   the guest's precision. That is the true value correctly rounded except
 *   when it lies within that distance of a rounding boundary. The x87
 *   build's glibc gets these to an ulp or so, so the two can differ in the
 *   last bit; this build gives the same bits on every host it runs on.
 *
 * The double-double code needs each double operation rounded once, as
 * written: the Makefile builds with -ffp-contract=off, so that no compiler
 * fuses a multiply and an add here behind its back.
 */
typedef fx80 fpv;
typedef struct { double h, l; } fs_dd;               /* a double-double: h + l, |l| <= ulp(h)/2 */
#include "fpu_soft_tables.h"

static int fv_isnan(fpv v) { int c = fx_class(v); return c == FX_QNAN || c == FX_SNAN; }
static int fv_isinf(fpv v) { return fx_class(v) == FX_INF; }
static int fv_iszero(fpv v) { return fx_class(v) == FX_ZERO; }
static int fv_neg(fpv v) { return v.se >> 15; }
static fpv fv_nan(void) { return fx_bits(0xc000000000000000ull, 0x7fff); }   /* C's NAN, as the x87 holds it */
static fpv fv_one(int neg) { return fx_bits(1ull << 63, (uint16_t)(neg << 15 | 0x3fff)); }
static fpv fv_chs(fpv v) { v.se ^= 0x8000; return v; }
/* a pseudo-denormal (exponent 0, integer bit set) as the x87's arithmetic
 * hands it back: its canonical encoding, exponent 1 */
static fpv fv_canon(fpv v) { if (!(v.se & 0x7fff) && v.m >> 63) v.se |= 1; return v; }

/* 12 bytes, big-endian, as the x87 build takes them: an infinity or NaN
 * gets its integer bit, an unnormal is normalised by value, anything else
 * (a denormal, a pseudo-denormal) is kept as it is. */
static fpv from_ext(const uint8_t *p) {
    uint16_t se = (uint16_t)(p[0] << 8 | p[1]);
    uint64_t mant = 0;
    for (int i = 4; i < 12; i++) mant = mant << 8 | p[i];
    int e = se & 0x7fff;
    if (e == 0x7fff) return fx_bits(mant | 1ull << 63, se);
    if (e && !(mant >> 63)) {                       /* unnormal: mant x 2^(e - 16446) */
        if (!mant) return fx_zero_b(se >> 15);
        int z = __builtin_clzll(mant);
        if (e - z >= 1) return fx_bits(mant << z, (uint16_t)((se & 0x8000) | (e - z)));
        return fx_bits(mant << (e - 1), (uint16_t)(se & 0x8000));   /* a denormal, exactly */
    }
    return fx_bits(mant, se);
}
static void to_ext(fpv v, uint8_t *p) {
    p[0] = (uint8_t)(v.se >> 8); p[1] = (uint8_t)v.se; p[2] = p[3] = 0;
    for (int i = 0; i < 8; i++) p[4 + i] = (uint8_t)(v.m >> (56 - 8 * i));
}

static void fpu_cc(ac090 *c, fpv v) { c->fpsr = (c->fpsr & 0x00ffffffu) | fx_cc(v); }

/* Round a result to the precision FPCR asks for (or the instruction's own):
 * as (long double)(float) and (long double)(double) do on the x87. */
static fpv fpu_round(ac090 *c, fpv v, int prec) {
    if (prec == 0) prec = c->fpcr >> 6 & 3;
    return prec == 1 || prec == 2 ? fx_format(v, prec) : v;
}
/* FINT: to an integer by FPCR's mode; a NaN made quiet */
static fpv round_mode(ac090 *c, fpv v) {
    if (fv_isnan(v)) return fx_quiet_b(v);
    return fx_integer(v, (int)(c->fpcr >> 4 & 3));
}
/* to an integer in lo to hi (a long, word or byte), by FPCR's mode,
 * saturating; a NaN gives hi */
static int32_t to_int(ac090 *c, fpv v, int64_t lo, int64_t hi) {
    if (fv_isnan(v)) return (int32_t)hi;
    fpv r = fx_integer(v, (int)(c->fpcr >> 4 & 3));
    int neg = fv_neg(r), e = r.se & 0x7fff;
    int64_t n;
    if (fv_iszero(r)) n = 0;
    else if (e >= 0x3fff + 62) n = neg ? INT64_MIN : INT64_MAX;   /* infinity too */
    else { n = (int64_t)(r.m >> (0x3fff + 63 - e)); if (neg) n = -n; }
    return (int32_t)(n < lo ? lo : n > hi ? hi : n);
}

/* The other formats in and out, as the x87's loads and stores convert
 * them: a signalling NaN comes in quiet; out, rounded to nearest. */
static fpv fv_int(int32_t n) {
    if (!n) return fx_zero_b(0);
    uint32_t u = (uint32_t)(n < 0 ? -(int64_t)n : n);
    int bit = 31 - __builtin_clz(u);
    return fx_bits((uint64_t)u << (63 - bit), (uint16_t)((n < 0 ? 0x8000 : 0) | (0x3fff + bit)));
}
static fpv fv_ieee(uint64_t u, int fbits, int ebits) {          /* binary32 or binary64 bits */
    int bias = (1 << (ebits - 1)) - 1, emax = (1 << ebits) - 1;
    uint16_t sign = (uint16_t)((u >> (fbits + ebits)) & 1) << 15;
    int e = (int)(u >> fbits) & emax;
    uint64_t f = u & ((1ull << fbits) - 1);
    if (e == emax) return f ? fx_bits(3ull << 62 | f << (63 - fbits), sign | 0x7fff) : fx_inf_b(sign != 0);
    if (!e) {
        if (!f) return fx_zero_b(sign != 0);
        int z = __builtin_clzll(f);                 /* f x 2^(1 - bias - fbits) */
        return fx_bits(f << z, (uint16_t)(sign | (0x3fff + 63 - z + 1 - bias - fbits)));
    }
    return fx_bits((1ull << 63) | f << (63 - fbits), (uint16_t)(sign | (0x3fff + e - bias)));
}
static fpv fv_f32(uint32_t u) { return fv_ieee(u, 23, 8); }
static fpv fv_f64(uint64_t u) { return fv_ieee(u, 52, 11); }
static uint64_t fv_to_ieee(fpv v, int prec) {
    int fbits = prec == 1 ? 23 : 52, ebits = prec == 1 ? 8 : 11;
    int bias = (1 << (ebits - 1)) - 1, emax = (1 << ebits) - 1;
    fpv x = fx_format(v, prec);                     /* the value the format holds, as extended */
    uint64_t sign = (uint64_t)(x.se >> 15) << (fbits + ebits);
    int c = fx_class(x), e = x.se & 0x7fff;
    if (c == FX_ZERO) return sign;
    if (c == FX_INF) return sign | (uint64_t)emax << fbits;
    if (c == FX_QNAN || c == FX_SNAN) return sign | (uint64_t)emax << fbits | (x.m << 1 >> (64 - fbits));
    uint64_t m = x.m;
    if (!e) { int z = __builtin_clzll(m); m <<= z; e = 1 - z; }
    int ue = e - 0x3fff;
    if (ue >= 1 - bias) return sign | (uint64_t)(ue + bias) << fbits | (m << 1 >> (64 - fbits));
    return sign | m >> (63 - fbits + (1 - bias - ue));   /* a denormal of the format */
}
static uint32_t fv_to_f32(fpv v) { return (uint32_t)fv_to_ieee(v, 1); }
static uint64_t fv_to_f64(fpv v) { return fv_to_ieee(v, 2); }

static fpv fmovecr(int off) {
    for (size_t i = 0; i < sizeof fs_fmovecr / sizeof fs_fmovecr[0]; i++)
        if (fs_fmovecr[i].off == off) return fx_bits(fs_fmovecr[i].m, fs_fmovecr[i].se);
    return fx_zero_b(0);
}
static fpv fpu_default_nan(void) { return fx_defnan_b(); }
static fpv from_packed(const uint8_t *p) { return fs_from_packed(p); }
static void to_packed(fpv v, int k, uint8_t *p) { fs_to_packed(v, k, p); }

/* --- Arithmetic as the x87 does it --- */

/* FADD (0x22), FSUB (0x28, a - b), FMUL (0x23) and FDIV (0x20, a / b) of
 * extended values, rounded once to nearest at 64 bits, the x87's NaNs and
 * infinities; an invalid operation gives the 68040's default NaN, or with
 * x87_nan the x87's own indefinite. */
static fpv fs_op(int op, fpv a, fpv b, int x87_nan) {
    fx80 r;
    int k = fx_special_bits(op, a, b, &r);
    if (k) return k == 2 && x87_nan ? fx_indefinite_b() : r;
    if (fx_arith_fast(op, a, b, &r)) return r;
    int ca = fx_class(a), cb = fx_class(b), sa = a.se >> 15, sb = b.se >> 15;   /* a zero operand */
    switch (op) {
    case 0x28: sb ^= 1; b.se ^= 0x8000;
        /* fall through */
    case 0x22:
        if (ca == FX_ZERO && cb == FX_ZERO) return fx_zero_b(sa && sb);
        return fv_canon(ca == FX_ZERO ? b : a);
    case 0x23: return fx_zero_b(sa ^ sb);
    }
    if (cb == FX_ZERO) return ca == FX_ZERO ? (x87_nan ? fx_indefinite_b() : fx_defnan_b()) : fx_inf_b(sa ^ sb);
    return fx_zero_b(sa ^ sb);
}
static fpv fs_sqrt(fpv b) {
    int cb = fx_class(b);
    fx80 r;
    if (cb == FX_QNAN || cb == FX_SNAN) return fx_quiet_b(b);
    if (cb == FX_ZERO) return b;
    if (b.se & 0x8000) return fx_defnan_b();
    if (cb == FX_INF) return b;
    fx_sqrt_fast(b, &r);
    return r;
}
/* the x87's conversion of an integral value to int64 (fistp): out of range
 * or a NaN gives the integer indefinite, 0x8000000000000000 */
static uint64_t fs_int64(fpv q) {
    int c = fx_class(q), e = q.se & 0x7fff;
    if (c == FX_ZERO) return 0;
    if (c != FX_FINITE || e >= 0x3fff + 63) return 1ull << 63;
    if (e < 0x3fff) return 0;
    uint64_t n = q.m >> (0x3fff + 63 - e);
    return fv_neg(q) ? (uint64_t)-(int64_t)n : n;
}
/* (int)x of an integral value, as the x87 converts it: out of range or a
 * NaN, the integer indefinite, INT_MIN */
static int32_t fs_int32(fpv q) {
    int c = fx_class(q), e = q.se & 0x7fff;
    if (c == FX_ZERO) return 0;
    if (c != FX_FINITE || e >= 0x3fff + 31) return INT32_MIN;
    if (e < 0x3fff) return 0;
    int32_t n = (int32_t)(q.m >> (0x3fff + 63 - e));
    return fv_neg(q) ? -n : n;
}

/* --- Double-double, for the transcendentals --- */
#define FS_INLINE static inline __attribute__((always_inline))
FS_INLINE fs_dd dd_d(double a) { return (fs_dd){ a, 0.0 }; }
FS_INLINE fs_dd dd_fast2(double a, double b) { double s = a + b; return (fs_dd){ s, b - (s - a) }; }
FS_INLINE fs_dd dd_two(double a, double b) { double s = a + b, v = s - a; return (fs_dd){ s, (a - (s - v)) + (b - v) }; }
FS_INLINE fs_dd dd_prod(double a, double b) { double p = a * b; return (fs_dd){ p, fma(a, b, -p) }; }
FS_INLINE fs_dd dd_neg(fs_dd a) { return (fs_dd){ -a.h, -a.l }; }
FS_INLINE fs_dd dd_add(fs_dd a, fs_dd b) {
    fs_dd s = dd_two(a.h, b.h), t = dd_two(a.l, b.l);
    s.l += t.h;
    s = dd_fast2(s.h, s.l);
    s.l += t.l;
    return dd_fast2(s.h, s.l);
}
FS_INLINE fs_dd dd_sub(fs_dd a, fs_dd b) { return dd_add(a, dd_neg(b)); }
FS_INLINE fs_dd dd_mul(fs_dd a, fs_dd b) {
    double p = a.h * b.h;
    return dd_fast2(p, fma(a.h, b.h, -p) + (a.h * b.l + a.l * b.h));
}
FS_INLINE fs_dd dd_mul_d(fs_dd a, double b) { double p = a.h * b; return dd_fast2(p, fma(a.h, b, -p) + a.l * b); }
static fs_dd dd_div(fs_dd a, fs_dd b) {
    double q1 = a.h / b.h;
    fs_dd r = dd_sub(a, dd_mul_d(b, q1));
    double q2 = r.h / b.h;
    r = dd_sub(r, dd_mul_d(b, q2));
    double q3 = r.h / b.h;
    return dd_add(dd_fast2(q1, q2), dd_d(q3));
}
static fs_dd dd_sqrt(fs_dd a) {                      /* a > 0 */
    double s = sqrt(a.h);
    fs_dd r = dd_sub(a, dd_prod(s, s));
    return dd_fast2(s, r.h / (2 * s));
}
FS_INLINE double dd_pow2(int k) { uint64_t b = (uint64_t)(k + 1023) << 52; double d; memcpy(&d, &b, 8); return d; }   /* -1022 <= k <= 1023 */
FS_INLINE fs_dd dd_scale(fs_dd a, int k) { double s = dd_pow2(k); return (fs_dd){ a.h * s, a.l * s }; }
static const fs_dd FS_PIO2 = { FS_PIO2_1, FS_PIO2_2 }, FS_PIDD = { FS_PI_1, FS_PI_2 };
static const fs_dd FS_LN2DD = { 0x1.62e42fefa39efp-1, 0x1.abc9e3b39803fp-56 };

/* |x| = m 2^e, m in [1, 2) as a double-double, for a finite nonzero x */
typedef struct { fs_dd m; int32_t e; int neg; } fs_x;
static fs_x fs_split(fpv x) {
    fs_x r;
    int32_t e = 0;
    r.neg = x.se >> 15;
    fx_operand(&x, &e);
    r.e = e - 0x3fff;
    r.m = (fs_dd){ (double)(x.m & ~0x7ffull) * 0x1p-63, (double)(x.m & 0x7ffull) * 0x1p-63 };
    return r;
}
/* x as a double-double (its exponent within -900 to 1000), sign and all */
static fs_dd fs_dd_of(fpv x) {
    fs_x s = fs_split(x);
    fs_dd v = dd_scale(s.m, s.e);
    return s.neg ? dd_neg(v) : v;
}
/* v 2^scale, rounded once to extended (nearest/even), to a denormal, to
 * zero or to infinity as it falls */
static fpv fs_pack(fs_dd v, int32_t scale) {
    if (v.h == 0) return fx_zero_b(signbit(v.h) != 0);
    int sign = v.h < 0;
    uint64_t hb, lb;
    memcpy(&hb, &v.h, 8); memcpy(&lb, &v.l, 8);
    int eh = (int)(hb >> 52 & 0x7ff);
    uint64_t mh = hb & ((1ull << 52) - 1);
    if (eh) mh |= 1ull << 52; else eh = 1;
    int z0 = __builtin_clzll(mh) - 11;               /* 0 unless h is a binary64 denormal */
    fx_u128 A = (fx_u128)mh << (75 + z0);
    int32_t EA = eh - 1075 - 75 - z0;                /* |h| = A 2^EA */
    int sticky = 0;
    if (v.l != 0) {
        int el = (int)(lb >> 52 & 0x7ff);
        uint64_t ml = lb & ((1ull << 52) - 1);
        if (el) ml |= 1ull << 52; else el = 1;
        int32_t d = el - 1075 - EA;                  /* |l| = ml 2^(EA + d) */
        fx_u128 B;
        if (d >= 0) B = (fx_u128)ml << d;            /* |l| < 2^-52 |h|: below A's bit 76 */
        else if (d <= -64) { B = 0; sticky = 1; }
        else { B = ml >> -d; sticky = (ml & ((1ull << -d) - 1)) != 0; }
        if ((v.l < 0) == sign) A += B;
        else A = A - B - (fx_u128)sticky;
    }
    int z = fx_clz128(A);
    A = fx_shl(A, z); EA -= z;
    fx80 r;
    fx_pack(sign, EA + 127 + 0x3fff + scale, A | (fx_u128)sticky, &r);
    return r;
}

/* Taylor sums by Horner: sum c[i0 + i*step] y^i, i < n, with a double-double y */
static fs_dd fs_poly(fs_dd y, const fs_dd *c, int n, int step, int alt) {
    fs_dd s = c[(n - 1) * step];
    if (alt && ((n - 1) & 1)) s = dd_neg(s);
    for (int i = n - 2; i >= 0; i--) {
        fs_dd ci = c[i * step];
        if (alt && (i & 1)) ci = dd_neg(ci);
        s = dd_add(dd_mul(s, y), ci);
    }
    return s;
}
/* e^r - 1 for |r| <= 0.35: r (1 + r/2 + r^2/6 + ...), to r^26/27! */
static fs_dd fs_expm1_small(fs_dd r) { return dd_mul(r, fs_poly(r, fs_inv_fact + 1, 26, 1, 0)); }
/* e^x = y 2^k, |x| < 2^15 */
static fs_dd fs_exp_core(fs_dd x, int32_t *k) {
    double n = nearbyint(x.h * FS_INVLN2_1);
    fs_dd r = dd_add(x, dd_d(-n * FS_LN2_1));        /* n x LN2_1 and n x LN2_2 exact: 36 bits each */
    r = dd_add(r, dd_d(-n * FS_LN2_2));
    r = dd_sub(r, dd_prod(n, FS_LN2_3));
    r = dd_add(r, dd_d(-n * FS_LN2_4));
    *k = (int32_t)n;
    return dd_add(dd_d(1.0), fs_expm1_small(r));
}
/* e^x - 1 and e^x as double-doubles, |x| < 2^9 */
static fs_dd fs_exp_dd(fs_dd x) {
    int32_t k;
    fs_dd y = fs_exp_core(x, &k);
    return dd_scale(y, k);
}
static fs_dd fs_expm1_dd(fs_dd x) { return dd_sub(fs_exp_dd(x), dd_d(1.0)); }
/* 2 atanh(s) = ln((1 + s)/(1 - s)), |s| <= 0.18 */
static fs_dd fs_atanh2_small(fs_dd s) {
    fs_dd s2 = dd_mul(s, s);
    fs_dd t = dd_mul(s, fs_poly(s2, fs_inv_odd, 24, 1, 0));
    return (fs_dd){ 2 * t.h, 2 * t.l };
}
/* ln(m 2^e), m in [1/2, 2) a double-double */
static fs_dd fs_log_core(fs_dd m, int32_t e) {
    if (m.h > 0x1.6a09e667f3bcdp0) { m.h *= 0.5; m.l *= 0.5; e++; }        /* sqrt 2 */
    else if (m.h < 0x1.6a09e667f3bcdp-1) { m.h *= 2; m.l *= 2; e--; }
    fs_dd r = fs_atanh2_small(dd_div(dd_sub(m, dd_d(1.0)), dd_add(m, dd_d(1.0))));
    if (e) {
        double en = e;                                /* |e| < 2^15: the first two products exact */
        fs_dd l = dd_add(dd_d(en * FS_LN2_1), dd_d(en * FS_LN2_2));
        l = dd_add(l, dd_prod(en, FS_LN2_3));
        l = dd_add(l, dd_d(en * FS_LN2_4));
        r = dd_add(l, r);
    }
    return r;
}
/* ln of a double-double y > 0 within binary64's range */
static fs_dd fs_log_dd(fs_dd y) {
    int e;
    frexp(y.h, &e);
    return fs_log_core(dd_scale(y, 1 - e), e - 1);
}
/* atan(y) for a double-double y; |y| < 2^1000 */
static fs_dd fs_atan_dd(fs_dd y) {
    int neg = y.h < 0, inv = 0;
    if (neg) y = dd_neg(y);
    if (y.h > 1.0) { y = dd_div(dd_d(1.0), y); inv = 1; }
    int halvings = 0;
    while (y.h > 0.1) {                              /* atan y = 2 atan(y / (1 + sqrt(1 + y^2))) */
        y = dd_div(y, dd_add(dd_d(1.0), dd_sqrt(dd_add(dd_d(1.0), dd_mul(y, y)))));
        halvings++;
    }
    fs_dd r = dd_mul(y, fs_poly(dd_mul(y, y), fs_inv_odd, 20, 1, 1));
    r = dd_scale(r, halvings);
    if (inv) r = dd_add(dd_add(FS_PIO2, dd_d(FS_PIO2_3)), dd_neg(r));
    return neg ? dd_neg(r) : r;
}
/* sin and cos of |r| <= pi/4 */
static void fs_sincos_small(fs_dd r, fs_dd *s, fs_dd *c) {
    fs_dd z = dd_mul(r, r);
    *s = dd_mul(r, fs_poly(z, fs_inv_fact + 1, 16, 2, 1));
    *c = fs_poly(z, fs_inv_fact, 17, 2, 1);
}
/* x mod pi/2 for a finite x with |x| >= pi/4: r, |r| <= pi/4, and the
 * quadrant, x = r + q pi/2 (Payne and Hanek, on 2/pi's bits) */
static fs_dd fs_reduce(fpv x, int *quadrant) {
    int neg_x = x.se >> 15;
    int32_t e = 0;
    fx_operand(&x, &e);
    uint64_t m = x.m;                                /* |x| = m 2^(ue - 63) */
    int32_t ue = e - 0x3fff;
    int a = ue > 65 ? ue - 65 : 0;                   /* 2/pi's bits from a on (from 0): those before give multiples of 4 */
    uint64_t w[5];                                   /* bits a to a + 319, most significant first */
    for (int i = 0; i < 5; i++) {
        int bit = a + 64 * i, j = bit / 64, d = bit % 64;
        uint64_t hi = j < FS_TWO_OVER_PI_WORDS ? fs_two_over_pi[j] : 0, lo = j + 1 < FS_TWO_OVER_PI_WORDS ? fs_two_over_pi[j + 1] : 0;
        w[i] = d ? hi << d | lo >> (64 - d) : hi;
    }
    uint64_t P[6];                                   /* m x W, least significant first */
    uint64_t carry = 0;
    for (int i = 4; i >= 0; i--) {
        fx_u128 t = (fx_u128)w[i] * m + carry;
        P[4 - i] = (uint64_t)t; carry = (uint64_t)(t >> 64);
    }
    P[5] = carry;
    int pt = a + 320 + 63 - ue;                      /* the binary point, from P's bit 0: 318 to 384 */
#define PBIT(i) ((i) < 0 || (i) >= 384 ? 0 : (int)(P[(i) / 64] >> ((i) % 64) & 1))
    int q = PBIT(pt) | PBIT(pt + 1) << 1;
    fx_u128 F = 0; uint64_t F2 = 0;                  /* the fraction's 192 bits: F, then F2 */
    for (int i = 1; i <= 128; i++) F = F << 1 | (fx_u128)PBIT(pt - i);
    for (int i = 129; i <= 192; i++) F2 = F2 << 1 | (uint64_t)PBIT(pt - i);
#undef PBIT
    int neg = 0;
    if ((uint64_t)(F >> 127)) {                      /* f >= 1/2: f - 1, and a quadrant on */
        F2 = -F2; F = ~F + (fx_u128)(F2 == 0);
        q++; neg = 1;
    }
    if (!F && !F2) { *quadrant = (neg_x ? -q : q) & 3; return dd_d(0.0); }
    int z = F ? fx_clz128(F) : 128 + __builtin_clzll(F2);
    fx_u128 N;                                       /* the fraction's first 128 significant bits */
    if (z == 0) N = F;
    else if (z < 64) N = F << z | (fx_u128)(F2 >> (64 - z));
    else if (z < 128) N = F << z | (fx_u128)F2 << (z - 64);
    else N = (fx_u128)(F2 << (z - 128)) << 64;
    double h = (double)(uint64_t)(N >> 75);                               /* 53 bits */
    double l = (double)(uint64_t)((N & (((fx_u128)1 << 75) - 1)) >> 11);   /* the 64 below them, rounded */
    fs_dd f = dd_fast2(h * 0x1p-53, l * 0x1p-117);   /* N 2^-128, in [1/2, 1) */
    f = dd_scale(f, -z);                             /* the fraction of a quarter turn */
    fs_dd r = dd_mul(f, FS_PIO2);
    r = dd_add(r, dd_d(f.h * FS_PIO2_3));
    if (neg) r = dd_neg(r);
    if (neg_x) { r = dd_neg(r); q = -q; }
    *quadrant = q & 3;
    return r;
}
static void fs_sincos(fpv x, fs_dd *sn, fs_dd *cs) {
    fs_x s = fs_split(x);
    fs_dd r;
    int q = 0;
    if (s.e < -1 || (s.e == -1 && dd_scale(s.m, -1).h < 0.785398163397448)) r = fs_dd_of(x);
    else r = fs_reduce(x, &q);
    fs_dd a, b;
    fs_sincos_small(r, &a, &b);
    switch (q) {
    case 0: *sn = a; *cs = b; break;
    case 1: *sn = b; *cs = dd_neg(a); break;
    case 2: *sn = dd_neg(a); *cs = dd_neg(b); break;
    default: *sn = dd_neg(b); *cs = a; break;
    }
}

/* The transcendentals, on any operand: the x87 build's answers for NaNs,
 * infinities, zeros and out of range (a NaN where it gives one, made the
 * default NaN by fpsp_operr), and the value rounded once otherwise. */
static fpv fs_trans(int base, fpv x) {
    int c = fx_class(x), neg = fv_neg(x);
    if (c == FX_QNAN || c == FX_SNAN) {
        x = fx_quiet_b(x);
        if (base == 0x1c) x.se &= 0x7fff;            /* glibc's acosl clears a NaN's sign */
        return x;
    }
    /* glibc's log1pl, tanhl and atanl hand back a tiny pseudo-denormal
     * canonical, the others (sinhl, expm1l, asinl, atanhl, sinl, tanl) as
     * it came */
    if (base == 0x06 || base == 0x09 || base == 0x0a) x = fv_canon(x);
    fs_x s = { { 0, 0 }, 0, neg };
    int tiny = 0;
    if (c == FX_FINITE) { s = fs_split(x); tiny = s.e < -900; }
    fs_dd r;
    int32_t k = 0;
    switch (base) {
    case 0x02: case 0x19: case 0x09:                 /* FSINH, FCOSH, FTANH */
        if (c == FX_INF) return base == 0x19 ? fx_inf_b(0) : base == 0x09 ? fv_one(neg) : x;
        if (c == FX_ZERO || tiny) return base == 0x19 ? fv_one(0) : x;
        if (base == 0x09) {
            if (s.e >= 6) return fv_one(neg);            /* |x| >= 64: 1 - 2e^-2|x| rounds to 1 */
            fs_dd m = dd_scale(s.m, s.e + 1);             /* 2|x| */
            fs_dd E = s.e < -2 ? fs_expm1_small(m) : fs_expm1_dd(m);
            r = dd_div(E, dd_add(E, dd_d(2.0)));
            return fs_pack(neg ? dd_neg(r) : r, 0);
        }
        if (s.e >= 14) return base == 0x19 ? fx_inf_b(0) : fx_inf_b(neg);
        {
            fs_dd a = dd_scale(s.m, s.e);
            if (s.e >= 6) {                               /* |x| >= 64: e^|x| / 2 */
                r = fs_exp_core(a, &k);
                return fs_pack(base == 0x02 && neg ? dd_neg(r) : r, k - 1);
            }
            if (base == 0x02 && s.e < 0) {                /* sinh: (E + E/(E + 1)) / 2, E = e^|x| - 1 */
                fs_dd E = s.e < -2 ? fs_expm1_small(a) : fs_expm1_dd(a);
                r = dd_add(E, dd_div(E, dd_add(E, dd_d(1.0))));
            } else {
                fs_dd y = fs_exp_dd(a), iy = dd_div(dd_d(1.0), y);
                r = base == 0x02 ? dd_sub(y, iy) : dd_add(y, iy);
            }
            r = dd_scale(r, -1);
            return fs_pack(base == 0x02 && neg ? dd_neg(r) : r, 0);
        }
    case 0x06:                                       /* FLOGNP1 */
        if (c == FX_ZERO || tiny) return x;
        if (c == FX_INF) return neg ? fv_nan() : x;
        if (neg && (s.e > 0 || (s.e == 0 && (s.m.h > 1.0 || s.m.l > 0)))) return fv_nan();
        if (neg && s.e == 0) return fx_inf_b(1);       /* -1 */
        if (s.e < -2) {                                   /* |x| < 1/4: 2 atanh(x / (2 + x)) */
            fs_dd a = fs_dd_of(x);
            return fs_pack(fs_atanh2_small(dd_div(a, dd_add(dd_d(2.0), a))), 0);
        }
        if (s.e >= 100) return fs_pack(fs_log_core(s.m, s.e), 0);   /* ln(1 + x) = ln x + 1/x and less */
        return fs_pack(fs_log_dd(dd_add(dd_d(1.0), fs_dd_of(x))), 0);
    case 0x08:                                       /* FETOXM1 */
        if (c == FX_ZERO || tiny) return x;
        if (c == FX_INF) return neg ? fv_one(1) : x;
        if (neg && s.e >= 6) return fv_one(1);         /* e^x < 2^-92 */
        if (!neg && s.e >= 14) return fx_inf_b(0);
        if (s.e < -2) return fs_pack(fs_expm1_small(fs_dd_of(x)), 0);
        r = fs_exp_core(fs_dd_of(x), &k);
        if (k > 120) return fs_pack(r, k);
        return fs_pack(dd_sub(dd_scale(r, k), dd_d(1.0)), 0);   /* k from -93 */
    case 0x0a:                                       /* FATAN */
        if (c == FX_ZERO || tiny) return x;
        if (c == FX_INF || s.e > 1000) { r = dd_add(FS_PIO2, dd_d(FS_PIO2_3)); return fs_pack(neg ? dd_neg(r) : r, 0); }
        return fs_pack(fs_atan_dd(fs_dd_of(x)), 0);
    case 0x0c: case 0x1c: {                          /* FASIN, FACOS */
        if (c == FX_INF || (c == FX_FINITE && (s.e > 0 || (s.e == 0 && (s.m.h > 1.0 || s.m.l > 0))))) return fv_nan();
        if (base == 0x0c && (c == FX_ZERO || tiny)) return x;
        if (base == 0x1c && (c == FX_ZERO || tiny)) return fs_pack(dd_add(FS_PIO2, dd_d(FS_PIO2_3)), 0);
        fs_dd a = fs_dd_of(x), one = dd_d(1.0);
        if (base == 0x0c) {
            if (s.e == 0) { r = dd_add(FS_PIO2, dd_d(FS_PIO2_3)); return fs_pack(neg ? dd_neg(r) : r, 0); }   /* +-1 */
            fs_dd ax = neg ? dd_neg(a) : a;
            fs_dd w = dd_mul(dd_sub(one, ax), dd_add(one, ax));
            r = fs_atan_dd(dd_div(ax, dd_sqrt(w)));
            return fs_pack(neg ? dd_neg(r) : r, 0);
        }
        if (s.e == 0) return neg ? fs_pack(dd_add(FS_PIDD, dd_d(FS_PI_3)), 0) : fx_zero_b(0);   /* acos -1, acos 1 */
        r = fs_atan_dd(dd_sqrt(dd_div(dd_sub(one, a), dd_add(one, a))));
        return fs_pack(dd_scale(r, 1), 0);
    }
    case 0x0d:                                       /* FATANH */
        if (c == FX_ZERO || tiny) return x;
        if (c == FX_INF || s.e > 0 || (s.e == 0 && (s.m.h > 1.0 || s.m.l > 0))) return fv_nan();
        if (s.e == 0) return fx_inf_b(neg);
        {
            fs_dd a = fs_dd_of(x);                        /* atanh x = 2 atanh(x) / 2, by ln((1 + x)/(1 - x)) / 2 */
            if (s.e < -3) r = fs_atanh2_small(a);
            else r = fs_log_dd(dd_div(dd_add(dd_d(1.0), a), dd_sub(dd_d(1.0), a)));
            return fs_pack(dd_scale(r, -1), 0);
        }
    case 0x0e: case 0x0f: case 0x1d: {               /* FSIN, FTAN, FCOS */
        if (c == FX_INF) return fv_nan();
        if (c == FX_ZERO || tiny) return base == 0x1d ? fv_one(0) : x;
        fs_dd sn, cs;
        fs_sincos(x, &sn, &cs);
        if (base == 0x0e) return fs_pack(sn, 0);
        if (base == 0x1d) return fs_pack(cs, 0);
        return fs_pack(dd_div(sn, cs), 0);
    }
    case 0x10: case 0x11: case 0x12: {               /* FETOX, FTWOTOX, FTENTOX */
        if (c == FX_INF) return neg ? fx_zero_b(0) : x;
        if (c == FX_ZERO || tiny) return fv_one(0);
        if (s.e >= 15) return neg ? fx_zero_b(0) : fx_inf_b(0);
        fs_dd a = fs_dd_of(x);
        if (base == 0x10) { r = fs_exp_core(a, &k); return fs_pack(r, k); }
        double n;
        fs_dd f;
        if (base == 0x11) {                           /* 2^x: the whole part exact */
            n = nearbyint(a.h);
            f = dd_add(dd_d(a.h - n), dd_d(a.l));
        } else {                                      /* 10^x: x log2 10 to about 2^-106 */
            fs_dd p = dd_prod(a.h, FS_LOG2_10_1);
            n = nearbyint(p.h);
            f = dd_add(dd_d(p.h - n), dd_d(p.l));
            f = dd_add(f, dd_prod(a.h, FS_LOG2_10_2));
            f = dd_add(f, dd_prod(a.l, FS_LOG2_10_1));
            f = dd_add(f, dd_d(a.h * FS_LOG2_10_3 + a.l * FS_LOG2_10_2));
        }
        r = dd_add(dd_d(1.0), fs_expm1_small(dd_mul(f, FS_LN2DD)));
        return fs_pack(r, (int32_t)n);
    }
    case 0x14: case 0x15: case 0x16: {               /* FLOGN, FLOG10, FLOG2 */
        if (c == FX_ZERO) return fx_inf_b(1);
        if (neg) return fv_nan();
        if (c == FX_INF) return x;
        fs_dd m = s.m;
        int32_t e = s.e;
        if (m.h > 0x1.6a09e667f3bcdp0) { m.h *= 0.5; m.l *= 0.5; e++; }
        r = fs_log_core(m, base == 0x16 ? 0 : e);
        if (base == 0x15) r = dd_mul(r, (fs_dd){ FS_INVLN10_1, FS_INVLN10_2 });
        if (base == 0x16) r = dd_add(dd_d((double)e), dd_mul(r, (fs_dd){ FS_INVLN2_1, FS_INVLN2_2 }));
        return fs_pack(r, 0);
    }
    }
    return fv_nan();
}

/* The FPU's default NaN out of an operand error: fpsp_operr and fpu_invalid
 * (shared with the x87 build) say when. */
static int fpsp_operr(int base, fpv r, fpv d, fpv src);
static int fpu_invalid(int base, fpv r, fpv d, fpv src);

/* One arithmetic operation: dst op src into FPn. The x87 build's
 * fpu_arith, case for case. */
static void fpu_arith(ac090 *c, int opmode, fpv src, int dn) {
    fpv *d = &c->fp[dn], r = fx_zero_b(0);
    int prec = 0, base = opmode;
    if (opmode & 0x40) {                                         /* 040 FSxxx (single), FDxxx (double) */
        prec = (opmode & 4) ? 2 : 1;
        base = opmode & ~0x44;
        if (base == 0x01) base = 0x04;                           /* FSSQRT/FDSQRT */
    }
    switch (base) {
    case 0x00: r = src; break;                                   /* FMOVE */
    case 0x01: r = round_mode(c, src); break;                    /* FINT */
    case 0x03: r = fv_isnan(src) ? fx_quiet_b(src) : fx_integer(src, 1); break;   /* FINTRZ */
    case 0x04: r = fs_sqrt(src); break;                          /* FSQRT */
    case 0x02: case 0x06: case 0x08: case 0x09: case 0x0a: case 0x0c: case 0x0d: case 0x0e: case 0x0f:
    case 0x10: case 0x11: case 0x12: case 0x14: case 0x15: case 0x16: case 0x19: case 0x1c: case 0x1d:
        r = fs_trans(base, src); break;
    case 0x18: r = src; r.se &= 0x7fff; break;                   /* FABS */
    case 0x1a: r = fv_chs(src); break;                           /* FNEG */
    case 0x1e:                                                   /* FGETEXP */
        if (fv_iszero(src) || fv_isnan(src)) r = src;
        else if (fv_isinf(src)) r = fv_nan();
        else { fpv t = src; int32_t e = 0; fx_operand(&t, &e); r = fv_int(e - 0x3fff); }
        break;
    case 0x1f:                                                   /* FGETMAN */
        if (fv_iszero(src) || fv_isnan(src)) r = src;
        else if (fv_isinf(src)) r = fv_nan();
        else { fpv t = src; int32_t e = 0; fx_operand(&t, &e); r = fx_bits(t.m, (uint16_t)((src.se & 0x8000) | 0x3fff)); }
        break;
    case 0x20: r = fs_op(0x20, *d, src, 0); break;               /* FDIV */
    case 0x21: case 0x25: {                                      /* FMOD (truncated), FREM (nearest) */
        fpv q = fs_op(0x20, *d, src, 1);                         /* the quotient as the x87 build has it */
        q = fv_isnan(q) ? fx_quiet_b(q) : fx_integer(q, opmode == 0x21 ? 1 : 0);
        int ca = fx_class(*d), cb = fx_class(src);
        uint8_t qbyte;
        if (ca == FX_QNAN || ca == FX_SNAN || cb == FX_QNAN || cb == FX_SNAN) r = fx_nan2(*d, ca, src, cb);
        else if (ca == FX_INF || cb == FX_ZERO) r = fx_indefinite_b();
        else if (ca == FX_ZERO || cb == FX_INF) r = fv_canon(*d);
        else fx_modrem(*d, src, opmode == 0x25, &r, &qbyte);
        uint64_t qa = fs_int64(fx_bits(q.m, q.se & 0x7fff));      /* (int64_t)fabsl(q) */
        uint32_t qb = (uint32_t)(qa & 0x7f) | (fv_neg(q) ? 0x80 : 0);
        c->fpsr = (c->fpsr & ~0x00ff0000u) | qb << 16;
        break;
    }
    case 0x22: r = fs_op(0x22, *d, src, 0); break;               /* FADD */
    case 0x23: r = fs_op(0x23, *d, src, 0); break;               /* FMUL */
    case 0x24: r = fx_format(fs_op(0x20, *d, src, 1), 1); break; /* FSGLDIV */
    case 0x26: {                                                 /* FSCALE */
        int32_t n = fv_isnan(src) || fv_isinf(src) ? INT32_MIN : fs_int32(fx_integer(src, 1));
        int cd = fx_class(*d);
        if (cd == FX_QNAN || cd == FX_SNAN) r = fx_quiet_b(*d);
        else if (cd != FX_FINITE) r = *d;
        else {
            if (n < -100000) n = -100000;
            if (n > 100000) n = 100000;
            fx_scale_fast(*d, n, &r);
        }
        break;
    }
    case 0x27: {                                                 /* FSGLMUL */
        /* of two NaNs, the destination's (as the x87 build) */
        fpv a = fx_format(*d, 1), b = fx_format(src, 1);
        r = fv_isnan(a) ? fx_quiet_b(a) : fx_format(fs_op(0x23, a, b, 1), 1);
        break;
    }
    case 0x28: r = fs_op(0x28, *d, src, 0); break;               /* FSUB */
    case 0x38:                                                   /* FCMP: the FPU compares, two infinities of one sign are equal */
        if (fv_isinf(*d) && fv_isinf(src) && fv_neg(*d) == fv_neg(src)) fpu_cc(c, fx_zero_b(fv_neg(*d)));
        else fpu_cc(c, fs_op(0x28, *d, src, 0));
        return;
    case 0x3a: fpu_cc(c, src); return;                           /* FTST */
    default:
        if ((base & 0x78) == 0x30) {                           /* FSINCOS: sin to FPn, cos to FPc */
            fpv co = fs_trans(0x1d, src);
            c->fp[opmode & 7] = fpsp_operr(base, co, fx_zero_b(0), src) ? fpu_default_nan() : fpu_round(c, co, prec);
            r = fs_trans(0x0e, src);
            break;
        }
        illegal(c); longjmp(c->abort, 2);
    }
    r = fpsp_operr(base, r, *d, src) || fpu_invalid(base, r, *d, src) ? fpu_default_nan() : fpu_round(c, r, prec);
    *d = r;
    fpu_cc(c, r);
}
