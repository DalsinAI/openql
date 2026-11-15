/*
 * Packed decimal in and out of the 68881's extended format, worked out in
 * big integers: the value of 17 BCD digits and a 3-digit exponent rounded
 * once (as glibc's strtold rounds the string AC090 makes of them), and the
 * first k digits of an extended value rounded to nearest/even (as glibc's
 * printf %.*Le gives them). The x87 build took both from the C library,
 * so a Mac or Windows build could read and write other digits than Linux;
 * both builds use this now, and give the bits glibc gave.
 * Included by ac090_fpu.c.
 */

/* Big integers: 32-bit limbs, least significant first */
#define BN_MAX 1400                                 /* 44,800 bits: 64 x 5^16445 needs 38,262 */
typedef struct { int n; uint32_t w[BN_MAX]; } bn;
static void bn_set(bn *a, uint64_t v) { a->n = 0; while (v) { a->w[a->n++] = (uint32_t)v; v >>= 32; } }
static void bn_mul_small(bn *a, uint32_t k) {
    uint64_t carry = 0;
    for (int i = 0; i < a->n; i++) { uint64_t t = (uint64_t)a->w[i] * k + carry; a->w[i] = (uint32_t)t; carry = t >> 32; }
    if (carry) a->w[a->n++] = (uint32_t)carry;
}
static void bn_shl(bn *a, int s) {
    if (!a->n) return;
    int words = s / 32, bits = s % 32;
    a->w[a->n] = 0;
    if (bits) for (int i = a->n; i > 0; i--) a->w[i] = a->w[i] << bits | a->w[i - 1] >> (32 - bits);
    else a->w[a->n] = 0;
    if (bits) a->w[0] <<= bits;
    int n = a->n + 1;
    for (int i = n - 1; i >= 0; i--) a->w[i + words] = a->w[i];
    for (int i = 0; i < words; i++) a->w[i] = 0;
    a->n = n + words;
    while (a->n && !a->w[a->n - 1]) a->n--;
}
static int bn_bits(const bn *a) { return a->n ? 32 * (a->n - 1) + 32 - __builtin_clz(a->w[a->n - 1]) : 0; }
static int bn_bit(const bn *a, int i) { return i >= 0 && i / 32 < a->n ? (int)(a->w[i / 32] >> (i % 32) & 1) : 0; }
static uint32_t bn_div_small(bn *a, uint32_t k) {   /* a /= k, the remainder back */
    uint64_t r = 0;
    for (int i = a->n - 1; i >= 0; i--) { uint64_t t = r << 32 | a->w[i]; a->w[i] = (uint32_t)(t / k); r = t % k; }
    while (a->n && !a->w[a->n - 1]) a->n--;
    return (uint32_t)r;
}
static void bn_pow10(bn *a, int k) { bn_set(a, 1); for (; k >= 9; k -= 9) bn_mul_small(a, 1000000000u); while (k--) bn_mul_small(a, 10); }

/* D x 10^k rounded once to extended, nearest/even, as strtold gives it. */
static fx80 fs_decimal(int neg, uint64_t D, int k) {
    if (!D) return fx_zero_b(neg);
    static _Thread_local bn N, M, R;
    fx_u128 q = 0;
    int sticky = 0, e;
    if (k >= 0) {
        bn_set(&N, D);                               /* N = D x 10^k */
        for (; k >= 9; k -= 9) bn_mul_small(&N, 1000000000u);
        while (k--) bn_mul_small(&N, 10);
        int b = bn_bits(&N);
        for (int i = 0; i < 128; i++) q = q << 1 | (fx_u128)bn_bit(&N, b - 1 - i);
        for (int i = 0; i < b - 128 && !sticky; i++) sticky = bn_bit(&N, i);
        e = 0x3fff + b - 1;
    } else {
        /* q = D x 2^s / 10^-k, 127 or 128 bits, a bit at a time */
        bn_pow10(&M, -k);
        int bD = 64 - __builtin_clzll(D), s = 127 + bn_bits(&M) - 1 - bD;
        bn_set(&R, 0);
        for (int i = bD + s - 1; i >= 0; i--) {
            bn_shl(&R, 1);
            if (i >= s && (D >> (i - s) & 1)) { if (R.n) R.w[0] |= 1; else bn_set(&R, 1); }
            /* R >= M ? */
            int ge = R.n != M.n ? R.n > M.n : 1;
            if (R.n == M.n) for (int j = R.n - 1; j >= 0; j--) if (R.w[j] != M.w[j]) { ge = R.w[j] > M.w[j]; break; }
            q <<= 1;
            if (ge) {
                int64_t borrow = 0;
                for (int j = 0; j < R.n; j++) {
                    int64_t t = (int64_t)R.w[j] - (j < M.n ? M.w[j] : 0) - borrow;
                    borrow = t < 0; R.w[j] = (uint32_t)t;
                }
                while (R.n && !R.w[R.n - 1]) R.n--;
                q |= 1;
            }
        }
        sticky = R.n != 0;
        int z = fx_clz128(q);
        q <<= z;
        e = 0x3fff + 127 - z - s;
    }
    fx80 r;
    fx_pack(neg, e, q | (fx_u128)sticky, &r);
    return r;
}

/* strtold on the string from_packed makes: [-]d.dddddddddddddddde[-]ddd,
 * whose "digits" may be any of 0-9:;<=>? (a nibble of A-F). As strtold:
 * the number ends at the first character that cannot go on it. */
static fx80 fs_strtold(const char *s) {
    int neg = 0, nd = 0, frac = 0, ex = 0;
    uint64_t D = 0;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') { D = D * 10 + (uint64_t)(*s++ - '0'); nd++; }
    if (*s == '.') {
        s++;
        while (*s >= '0' && *s <= '9') { D = D * 10 + (uint64_t)(*s++ - '0'); nd++; frac++; }
    }
    if (!nd) return fx_zero_b(0);                    /* no number at all: strtold's 0 */
    if (*s == 'e') {
        const char *t = s + 1;
        int eneg = 0;
        if (*t == '-' || *t == '+') eneg = *t++ == '-';
        if (*t >= '0' && *t <= '9') {
            while (*t >= '0' && *t <= '9') ex = ex * 10 + (*t++ - '0');
            if (eneg) ex = -ex;
        }
    }
    return fs_decimal(neg, D, ex - frac);
}

/* Packed decimal: 12 bytes, read as the x87 build reads them. */
static fx80 fs_from_packed(const uint8_t *p) {
    char buf[64]; int n = 0;
    if (p[0] & 0x80) buf[n++] = '-';
    buf[n++] = (char)('0' + (p[3] & 15));
    buf[n++] = '.';
    for (int i = 4; i < 12; i++) { buf[n++] = (char)('0' + (p[i] >> 4)); buf[n++] = (char)('0' + (p[i] & 15)); }
    buf[n++] = 'e';
    if (p[0] & 0x40) buf[n++] = '-';
    buf[n++] = (char)('0' + (p[0] & 15)); buf[n++] = (char)('0' + (p[1] >> 4)); buf[n++] = (char)('0' + (p[1] & 15));
    buf[n] = 0;
    if ((p[0] & 0x7f) == 0x7f && (p[1] & 0xf0) == 0xf0) {    /* infinity or NaN */
        int nz = 0; for (int i = 4; i < 12; i++) nz |= p[i];
        return nz ? fx_bits(0xc000000000000000ull, 0x7fff) : fx_inf_b(p[0] >> 7);   /* C's NAN */
    }
    return fs_strtold(buf);
}

/* The first nd significant decimal digits of |v| (finite), rounded to
 * nearest/even, and the decimal exponent: what printf's %.*Le gives. */
static int fs_digits(fx80 v, int nd, int *dig) {
    static _Thread_local bn N;
    static _Thread_local char s[12000];
    int e = v.se & 0x7fff;
    uint64_t m = v.m;
    if (!e) m &= ~(1ull << 63);                     /* glibc's printf takes a pseudo-denormal's integer bit as 0 */
    if (!m) { for (int i = 0; i < nd; i++) dig[i] = 0; return 0; }
    int E = (e ? e : 1) - 0x3fff - 63;               /* |v| = m x 2^E */
    int shift = 0;
    bn_set(&N, m);
    if (E >= 0) bn_shl(&N, E);
    else {                                           /* |v| = m 5^-E x 10^E */
        int k = -E;
        for (; k >= 13; k -= 13) bn_mul_small(&N, 1220703125u);
        while (k--) bn_mul_small(&N, 5);
        shift = E;
    }
    int len = 0;                                     /* N's digits, least significant first */
    while (N.n) {
        uint32_t r = bn_div_small(&N, 1000000000u);
        for (int i = 0; i < 9; i++) { s[len++] = (char)(r % 10); r /= 10; }
    }
    while (len > 1 && !s[len - 1]) len--;
    int dexp = len - 1 + shift;
    for (int i = 0; i < nd; i++) dig[i] = i < len ? s[len - 1 - i] : 0;
    if (len > nd) {                                  /* round on the digits dropped */
        int d0 = s[len - 1 - nd], more = 0;
        for (int i = len - 2 - nd; i >= 0 && !more; i--) more = s[i] != 0;
        if (d0 > 5 || (d0 == 5 && (more || (dig[nd - 1] & 1)))) {
            int i = nd - 1;
            while (i >= 0 && dig[i] == 9) dig[i--] = 0;
            if (i >= 0) dig[i]++;
            else { dig[0] = 1; dexp++; }
        }
    }
    return dexp;
}
static void fs_to_packed(fx80 v, int k, uint8_t *p) {
    memset(p, 0, 12);
    int cl = fx_class(v);
    if (cl == FX_QNAN || cl == FX_SNAN) { p[0] = 0x7f; p[1] = 0xff; p[4] = 0xff; return; }
    if (cl == FX_INF) { p[0] = (uint8_t)(v.se & 0x8000 ? 0xff : 0x7f); p[1] = 0xff; return; }
    int digits = k > 0 ? (k > 17 ? 17 : k) : 17;
    int dig[17];
    int ex = fs_digits(v, digits, dig);
    if (v.se & 0x8000) p[0] |= 0x80;
    int mant[17] = { 0 };
    for (int i = 0; i < digits; i++) mant[i] = dig[i];
    if (ex < 0) { p[0] |= 0x40; ex = -ex; }
    p[0] |= (uint8_t)(ex / 100 % 10); p[1] = (uint8_t)((ex / 10 % 10) << 4 | ex % 10);
    p[3] = (uint8_t)mant[0];
    for (int i = 0; i < 8; i++) p[4 + i] = (uint8_t)(mant[1 + 2 * i] << 4 | mant[2 + 2 * i]);
}

