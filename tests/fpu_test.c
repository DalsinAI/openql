/*
 * The 68040 FPU's results, bit for bit, on every host.
 *
 * The x86 build does the FPU's arithmetic on the x87; ARM64 builds (Apple
 * Silicon, Linux on a Raspberry Pi) do it in integers (src/cpu/fpu_soft.c).
 * This runs the same instructions on both and checks they give the same
 * bits: random operands (ordinary numbers, the ends of the range,
 * denormals, zeros, infinities, quiet and signalling NaNs, unnormals and
 * pseudo-denormals), every FPCR rounding mode and precision, register and
 * memory operands in every format, stores in every format, FMOVECR.
 *
 *   ./fpu_test             the checks, as make test runs them
 *   ./fpu_test dump F [N]  N random cases' results to F
 *   ./fpu_test cmp F [N]   this build's results against F's (another build's), with a summary
 *
 * Two hashes: of the operations whose result is exact by definition
 * (arithmetic, conversions, FMOVECR, packed decimal), which every build
 * must give; and of the transcendentals, which the integer builds must
 * all give alike (the x87 build's glibc rounds a few of them otherwise:
 * cmp shows how many, and by how much).
 */
#include "cpu/ac090.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint8_t ram[1 << 20];
static ac090 cpu;
static ac090 *c = &cpu;
#define CODE 0x1000u
#define DATA 0x2000u

static void setup(void) {
    ac090_init(c, AC090_68040);
    for (int pg = 0; pg < 16; pg++) { c->bus.rpage[pg] = c->bus.wpage[pg] = ram + ((uint32_t)pg << 16); c->bus.wait[pg] = 0; }
    ram[0] = 0; ram[1] = 0x08; ram[2] = 0; ram[3] = 0;          /* SSP $80000 */
    ram[4] = 0; ram[5] = 0; ram[6] = 0x10; ram[7] = 0;          /* PC $1000 */
    ac090_reset(c);
}

static void set_fp(int n, const uint8_t *x) {
    uint32_t w[3];
    for (int k = 0; k < 3; k++) w[k] = (uint32_t)x[4 * k] << 24 | (uint32_t)x[4 * k + 1] << 16 | (uint32_t)x[4 * k + 2] << 8 | x[4 * k + 3];
    memcpy(&c->fp_tmp, w, 12);
    ac090_fpu_x_in(c, (uint32_t)n);
}
static void get_fp(int n, uint8_t *x) {
    uint32_t w[3];
    ac090_fpu_x_out(c, (uint32_t)n);
    memcpy(w, &c->fp_tmp, 12);
    for (int k = 0; k < 3; k++) { x[4 * k] = (uint8_t)(w[k] >> 24); x[4 * k + 1] = (uint8_t)(w[k] >> 16); x[4 * k + 2] = (uint8_t)(w[k] >> 8); x[4 * k + 3] = (uint8_t)w[k]; }
}
/* run op, ext at CODE (A0 = DATA) */
static void run(uint16_t op, uint16_t ext) {
    ram[CODE] = (uint8_t)(op >> 8); ram[CODE + 1] = (uint8_t)op;
    ram[CODE + 2] = (uint8_t)(ext >> 8); ram[CODE + 3] = (uint8_t)ext;
    ram[CODE + 4] = 0x4e; ram[CODE + 5] = 0x71;                 /* NOP */
    c->pc = CODE; c->a[0] = DATA;
    ac090_set_sr(c, 0x2700);
    ac090_step(c);
}

/* --- random operands --- */
static uint64_t rs = 0x9e3779b97f4a7c15ull;
static uint64_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
static void put_ext(uint8_t *x, uint16_t se, uint64_t m) {
    x[0] = (uint8_t)(se >> 8); x[1] = (uint8_t)se; x[2] = x[3] = 0;
    for (int i = 0; i < 8; i++) x[4 + i] = (uint8_t)(m >> (56 - 8 * i));
}
static void rand_ext(uint8_t *x) {
    uint64_t r = rnd(), m = rnd() | 1ull << 63;
    uint16_t s = (uint16_t)(r & 1) << 15;
    int e;
    switch (r >> 1 & 31) {
    case 0: put_ext(x, s, 0); return;                                   /* zero */
    case 1: put_ext(x, s | 0x7fff, 1ull << 63); return;                 /* infinity */
    case 2: put_ext(x, s | 0x7fff, m | 1ull << 62); return;             /* quiet NaN */
    case 3: put_ext(x, s | 0x7fff, (m & ~(1ull << 62)) | 1); return;    /* signalling NaN */
    case 4: put_ext(x, s, m >> (r >> 8 & 63)); return;                  /* denormal */
    case 5: put_ext(x, s, m); return;                                   /* pseudo-denormal */
    case 6: put_ext(x, (uint16_t)(s | (rnd() % 0x7ffe + 1)), m >> (1 + (r >> 8 & 63))); return;   /* unnormal */
    case 7: e = (int)(rnd() % 0x7ffe) + 1; break;                       /* anywhere */
    case 8: e = 0x7ffe - (int)(rnd() % 64); break;                      /* near the top */
    case 9: e = 1 + (int)(rnd() % 64); break;                           /* near the bottom */
    case 10: e = 0x3fff + (int)(rnd() % 64); m &= ~0ull << (rnd() % 64); break;   /* an integer, or near one */
    case 11: e = 0x3fff + (int)(rnd() % 16) - 4; m = (m & ~0ull << (rnd() % 64)) | (rnd() & 1); break;
    case 12: e = 0x3fff - 1 + (int)(rnd() % 3); m = 1ull << 63 | (rnd() & 0xff); break;   /* about 1 */
    case 13: e = 0x3fff + (int)(rnd() % 30) - 10; m = (m & 0xffffffffff000000ull); break; /* singles */
    case 14: e = 0x3fff - 70 + (int)(rnd() % 140); break;
    case 15: { static const uint64_t pi = 0xc90fdaa22168c235ull; e = 0x3fff + (int)(rnd() % 40) - 1; m = pi ^ (rnd() & 0xffff); break; }
    default: e = 0x3fff + (int)(rnd() % 40) - 20; break;               /* ordinary numbers */
    }
    put_ext(x, (uint16_t)(s | e), m);
}

/* the opmodes FPm,FPn takes (the 040's FS and FD forms with them) */
static const uint8_t opmodes[] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x06, 0x08, 0x09, 0x0a, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x14, 0x15,
    0x16, 0x18, 0x19, 0x1a, 0x1c, 0x1d, 0x1e, 0x1f, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28,
    0x30, 0x32, 0x37, 0x38, 0x3a,
    0x40, 0x41, 0x44, 0x45, 0x58, 0x5a, 0x5c, 0x5e, 0x60, 0x62, 0x63, 0x64, 0x66, 0x67, 0x68, 0x6c,
};
static int transcendental(int opmode) {
    switch (opmode) {
    case 0x02: case 0x06: case 0x08: case 0x09: case 0x0a: case 0x0c: case 0x0d: case 0x0e: case 0x0f:
    case 0x10: case 0x11: case 0x12: case 0x14: case 0x15: case 0x16: case 0x19: case 0x1c: case 0x1d:
        return 1;
    }
    return (opmode & 0x78) == 0x30;
}

/* One random case: what it does, and its results. */
typedef struct {
    uint8_t kind, opmode, fmt, pad;                 /* kind: 0 FPm,FPn; 1 <ea>,FPn; 2 FPn,<ea>; 3 FMOVECR */
    uint16_t fpcr; int16_t k;
    uint8_t dst[12], src[12];
    uint8_t fp0[12], fp2[12], mem[12];
    uint32_t fpsr;
} rec;
static const int fsize[8] = { 4, 4, 12, 12, 2, 8, 1, 12 };

static void one(rec *r) {
    memset(r, 0, sizeof *r);
    uint64_t pick = rnd();
    r->fpcr = (uint16_t)(pick & 0xf0);
    rand_ext(r->dst); rand_ext(r->src);
    uint8_t three[12]; put_ext(three, 0x4000, 0xc000000000000000ull);
    int kind = (int)(pick >> 8) % 16;
    r->kind = kind < 9 ? 0 : kind < 13 ? 1 : kind < 15 ? 2 : 3;
    r->opmode = opmodes[(pick >> 16) % sizeof opmodes];
    r->fmt = (uint8_t)((pick >> 24) % 7);
    r->k = (int16_t)((int)(pick >> 32 & 127) - 64);
    set_fp(0, r->dst); set_fp(1, r->src); set_fp(2, three);
    c->fpcr = r->fpcr; c->fpsr = 0;
    memset(ram + DATA, 0x5a, 16);
    switch (r->kind) {
    case 0: run(0xf200, (uint16_t)(0x0400 | r->opmode)); break;
    case 1: {                                                /* the source in memory, in format fmt */
        int sz = fsize[r->fmt];
        if (r->fmt == 2 || r->fmt == 3) {
            if (r->fmt == 3 && (rnd() & 1)) {                /* packed decimal: digits, not always valid ones */
                uint64_t a = rnd(), b = rnd();
                memcpy(r->src, &a, 8); memcpy(r->src + 8, &b, 4);
                if (rnd() & 3) for (int i = 0; i < 12; i++) {
                    if ((r->src[i] & 0x0f) > 9) r->src[i] = (uint8_t)(r->src[i] & 0xf0) | 9;
                    if ((r->src[i] >> 4) > 9) r->src[i] = (uint8_t)(r->src[i] & 0x0f) | 0x90;
                }
                if (rnd() & 1) r->src[0] = (uint8_t)((r->src[0] & 0xc0) | (rnd() & 1));   /* a usual exponent */
                r->src[2] = 0; r->src[3] &= 0x0f;
            }
            memcpy(ram + DATA, r->src, 12);
        } else for (int i = 0; i < sz; i++) ram[DATA + i] = r->src[(i + 4) % 12];
        run(0xf210, (uint16_t)(0x4000 | r->fmt << 10 | r->opmode));
        break;
    }
    case 2: run(0xf210, (uint16_t)(0x6000 | r->fmt << 10 | ((unsigned)r->k & 127))); break;
    case 3: r->opmode = (uint8_t)((pick >> 40) % 0x40); run(0xf200, (uint16_t)(0x5c00 | r->opmode)); break;
    }
    get_fp(0, r->fp0); get_fp(2, r->fp2);
    memcpy(r->mem, ram + DATA, 12);
    r->fpsr = c->fpsr;
}
static int rec_trans(const rec *r) { return (r->kind == 0 || r->kind == 1) && transcendental(r->opmode); }

static uint64_t fnv(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001b3ull; }
    return h;
}
static uint64_t rec_hash(uint64_t h, const rec *r) {
    h = fnv(h, r->fp0, 12); h = fnv(h, r->fp2, 12); h = fnv(h, r->mem, 12);
    return fnv(h, &r->fpsr, 4);
}

/* ulps between two extended results of one sign, or -1 */
static int64_t ulps(const uint8_t *a, const uint8_t *b) {
    if ((a[0] ^ b[0]) & 0x80) return -1;
    unsigned __int128 x = 0, y = 0;
    for (int i = 0; i < 2; i++) { x = x << 8 | (a[i] & (i ? 0xff : 0x7f)); y = y << 8 | (b[i] & (i ? 0xff : 0x7f)); }
    for (int i = 4; i < 12; i++) { x = x << 8 | a[i]; y = y << 8 | b[i]; }
    unsigned __int128 d = x > y ? x - y : y - x;
    return d > 1000000 ? -1 : (int64_t)d;
}

/* --- fixed cases --- */
static void ext_of(uint8_t *x, const char *hex) {   /* "SEEE MMMMMMMMMMMMMMMM" */
    uint16_t se = (uint16_t)strtoul(hex, NULL, 16);
    uint64_t m = strtoull(hex + 5, NULL, 16);
    put_ext(x, se, m);
}
static void fixed(void) {
    uint8_t a[12], b[12], r[12];
    char got[32];
#define SHOW(x) (snprintf(got, sizeof got, "%02x%02x %02x%02x%02x%02x%02x%02x%02x%02x", x[0], x[1], x[4], x[5], x[6], x[7], x[8], x[9], x[10], x[11]), got)
#define RR(opm, d, s, want) do { ext_of(a, d); ext_of(b, s); set_fp(0, a); set_fp(1, b); c->fpsr = 0; \
        run(0xf200, (uint16_t)(0x0400 | (opm))); get_fp(0, r); \
        CHECK(!strcmp(SHOW(r), want), "opmode %02x %s, %s: %s, want %s", opm, d, s, got, want); } while (0)
    c->fpcr = 0;
    RR(0x22, "3fff 8000000000000000", "3fbf 8000000000000000", "3fff 8000000000000000");   /* 1 + 2^-64: ties to even */
    RR(0x22, "3fff 8000000000000001", "3fbf 8000000000000000", "3fff 8000000000000002");
    RR(0x28, "3fff 8000000000000000", "3fff 8000000000000000", "0000 0000000000000000");   /* x - x = +0 */
    RR(0x22, "8000 0000000000000000", "8000 0000000000000000", "8000 0000000000000000");   /* -0 + -0 */
    RR(0x20, "4000 c000000000000000", "4000 8000000000000000", "3fff c000000000000000");   /* 6/4 */
    RR(0x20, "3fff 8000000000000000", "4000 c000000000000000", "3ffd aaaaaaaaaaaaaaab");   /* 1/3 */
    RR(0x04, "0000 0000000000000000", "4000 8000000000000000", "3fff b504f333f9de6484");   /* sqrt 2 */
    RR(0x23, "0001 8000000000000000", "3ffe 8000000000000000", "0000 4000000000000000");   /* to a denormal */
    RR(0x23, "7ffe ffffffffffffffff", "4000 8000000000000000", "7fff 8000000000000000");   /* overflow */
    RR(0x22, "7fff 8000000000000000", "ffff 8000000000000000", "7fff ffffffffffffffff");   /* inf - inf: default NaN */
    RR(0x23, "0000 0000000000000000", "7fff 8000000000000000", "7fff ffffffffffffffff");
    RR(0x20, "0000 0000000000000000", "8000 0000000000000000", "7fff ffffffffffffffff");
    RR(0x04, "0000 0000000000000000", "bfff 8000000000000000", "7fff ffffffffffffffff");
    RR(0x22, "7fff a000000000000000", "3fff 8000000000000000", "7fff e000000000000000");   /* a signalling NaN comes out quiet */
    RR(0x22, "7fff c000000000000001", "ffff c000000000000002", "ffff c000000000000002");   /* of two NaNs, the larger significand */
    RR(0x00, "0000 0000000000000000", "7fff a000000000000000", "7fff a000000000000000");   /* FMOVE keeps it signalling */
    RR(0x40, "0000 0000000000000000", "3fff aaaaaaaaaaaaaaab", "3fff aaaaab0000000000");   /* FSMOVE */
    RR(0x44, "0000 0000000000000000", "3fff aaaaaaaaaaaaaaab", "3fff aaaaaaaaaaaaa800");   /* FDMOVE */
    RR(0x01, "0000 0000000000000000", "4000 a000000000000000", "4000 8000000000000000");   /* FINT 2.5: 2 */
    RR(0x03, "0000 0000000000000000", "c000 e000000000000000", "c000 c000000000000000");   /* FINTRZ -3.5: -3 */
    RR(0x1e, "0000 0000000000000000", "0000 0000000000000001", "c00d 807a000000000000");   /* FGETEXP of the least denormal: -16445 */
    RR(0x1f, "0000 0000000000000000", "8000 0000000000000003", "bfff c000000000000000");   /* FGETMAN */
    RR(0x26, "3fff 8000000000000000", "400c c01e000000000000", "7006 8000000000000000");   /* FSCALE by 12295.5: 2^12295 */
    RR(0x21, "4002 b000000000000000", "4000 c000000000000000", "4000 8000000000000000");   /* FMOD 11, 3: 2 */
    RR(0x25, "4002 b000000000000000", "4000 c000000000000000", "bfff 8000000000000000");   /* FREM 11, 3: -1 */
    RR(0x10, "0000 0000000000000000", "3fff 8000000000000000", "4000 adf85458a2bb4a9b");   /* e^1 */
    RR(0x12, "0000 0000000000000000", "4003 a000000000000000", "4041 ad78ebc5ac620000");   /* 10^20 */
    RR(0x15, "0000 0000000000000000", "4041 ad78ebc5ac620000", "4003 a000000000000000");   /* log10 of it */
    RR(0x16, "0000 0000000000000000", "0000 0000000000000001", "c00d 807a000000000000");   /* log2 of the least denormal */
    RR(0x0e, "0000 0000000000000000", "3fff 8000000000000000", "3ffe d76aa47848677021");   /* sin 1 */
    RR(0x0e, "0000 0000000000000000", "7ffe ffffffffffffffff", "3ffe fdfd9d4b6d0e5f7c");   /* sin of the largest number */
    RR(0x0a, "0000 0000000000000000", "7fff 8000000000000000", "3fff c90fdaa22168c235");   /* atan inf: pi/2 */
    RR(0x14, "0000 0000000000000000", "0000 0000000000000000", "ffff 8000000000000000");   /* ln 0: -inf */
    RR(0x14, "0000 0000000000000000", "bfff 8000000000000000", "7fff ffffffffffffffff");   /* ln -1: the default NaN */
    RR(0x0e, "0000 0000000000000000", "7fff 8000000000000000", "7fff ffffffffffffffff");
    /* FCMP: equal infinities */
    ext_of(a, "7fff 8000000000000000"); set_fp(0, a); set_fp(1, a); c->fpsr = 0; run(0xf200, 0x0438);
    CHECK((c->fpsr >> 24) == 0x04, "FCMP inf, inf: FPSR %08x", (unsigned)c->fpsr);
    ext_of(a, "8000 0000000000000000"); ext_of(b, "0000 0000000000000000"); set_fp(0, a); set_fp(1, b); c->fpsr = 0; run(0xf200, 0x0438);
    CHECK((c->fpsr >> 24) == 0x0c, "FCMP -0, +0: FPSR %08x", (unsigned)c->fpsr);
    /* FMOVECR pi, and 10^4096 */
    run(0xf200, 0x5c00); get_fp(0, r); CHECK(!strcmp(SHOW(r), "4000 c90fdaa22168c235"), "FMOVECR pi: %s", got);
    run(0xf200, 0x5c3f); get_fp(0, r); CHECK(!strcmp(SHOW(r), "7525 c46052028a20979b"), "FMOVECR 1e4096: %s", got);
    /* packed decimal both ways */
    static const uint8_t pk[12] = { 0x00, 0x01, 0, 0x03, 0x14, 0x15, 0x92, 0x65, 0x35, 0x89, 0x79, 0x32 };   /* 3.1415926535897932e1 */
    memcpy(ram + DATA, pk, 12); run(0xf210, 0x4c00); get_fp(0, r);
    CHECK(!strcmp(SHOW(r), "4003 fb53d14aa9c2f1e4"), "FMOVE.P in: %s", got);
    run(0xf210, 0x6c11);                                     /* FMOVE.P FP0,(A0){#17} */
    CHECK(!memcmp(ram + DATA, pk, 12), "FMOVE.P out: %02x%02x %02x %02x%02x%02x%02x", ram[DATA], ram[DATA + 1], ram[DATA + 3], ram[DATA + 4], ram[DATA + 5], ram[DATA + 6], ram[DATA + 7]);
    /* single and double stores, round to nearest */
    ext_of(a, "3fff aaaaaaaaaaaaaaab"); set_fp(0, a);
    run(0xf210, 0x6400);                                     /* FMOVE.S FP0,(A0) */
    CHECK(ram[DATA] == 0x3f && ram[DATA + 1] == 0xaa && ram[DATA + 2] == 0xaa && ram[DATA + 3] == 0xab, "FMOVE.S 4/3");
    run(0xf210, 0x7400);                                     /* FMOVE.D FP0,(A0) */
    CHECK(ram[DATA] == 0x3f && ram[DATA + 1] == 0xf5 && ram[DATA + 7] == 0x55, "FMOVE.D 4/3");
    c->fpcr = 0x20; run(0xf210, 0x6000);                    /* FMOVE.L, toward minus infinity */
    CHECK(ram[DATA + 3] == 1, "FMOVE.L 4/3 down: %d", ram[DATA + 3]);
    c->fpcr = 0;
}

/* The hashes every build gives, from the x86 build's x87, and from the
 * integer builds (x86 with -DAC090_FPU_SOFT=1, ARM64). */
#define CASES 60000
#define HASH_EXACT 0xd61a6eeecb3d9d31ull
#define HASH_TRANS_SOFT 0x5ef4252d97dcd1d6ull

int main(int argc, char **argv) {
    setup();
    if (getenv("FPU_SEED")) rs = strtoull(getenv("FPU_SEED"), NULL, 0) * 0x9e3779b97f4a7c15ull | 1;
    if (argc >= 3 && (!strcmp(argv[1], "dump") || !strcmp(argv[1], "cmp"))) {
        long n = argc > 3 ? atol(argv[3]) : 300000;
        int dump = !strcmp(argv[1], "dump");
        FILE *f = fopen(argv[2], dump ? "wb" : "rb");
        if (!f) { perror(argv[2]); return 2; }
        long diff_exact = 0, diff_trans = 0, trans = 0, hist[4] = { 0 };
        int shown = 0;
        for (long i = 0; i < n; i++) {
            rec r, o;
            one(&r);
            if (dump) { fwrite(&r, sizeof r, 1, f); continue; }
            if (fread(&o, sizeof o, 1, f) != 1) { printf("short file\n"); return 2; }
            int t = rec_trans(&r);
            trans += t;
            if (!memcmp(r.fp0, o.fp0, 12) && !memcmp(r.fp2, o.fp2, 12) && !memcmp(r.mem, o.mem, 12) && r.fpsr == o.fpsr) continue;
            if (t) {
                diff_trans++;
                int64_t u = memcmp(r.fp0, o.fp0, 12) ? ulps(r.fp0, o.fp0) : ulps(r.fp2, o.fp2);
                hist[u == 1 ? 0 : u == 2 ? 1 : u > 2 ? 2 : 3]++;
                if (u == 1 || u == 2) continue;
            } else diff_exact++;
            if ((!t || !getenv("FPU_CMP_EXACT")) && shown++ < (getenv("FPU_CMP_SHOW") ? atoi(getenv("FPU_CMP_SHOW")) : 40)) {
                printf("case %ld kind %d op %02x fmt %d fpcr %02x k %d\n  dst %02x%02x %016llx src %02x%02x %016llx\n", i, r.kind, r.opmode, r.fmt, r.fpcr, r.k,
                       r.dst[0], r.dst[1], (unsigned long long)__builtin_bswap64(*(uint64_t *)(r.dst + 4)), r.src[0], r.src[1], (unsigned long long)__builtin_bswap64(*(uint64_t *)(r.src + 4)));
                printf("  here  fp0 %02x%02x %016llx fp2 %02x%02x %016llx mem %02x%02x%02x%02x%02x%02x%02x%02x fpsr %08x\n", r.fp0[0], r.fp0[1], (unsigned long long)__builtin_bswap64(*(uint64_t *)(r.fp0 + 4)),
                       r.fp2[0], r.fp2[1], (unsigned long long)__builtin_bswap64(*(uint64_t *)(r.fp2 + 4)), r.mem[0], r.mem[1], r.mem[2], r.mem[3], r.mem[4], r.mem[5], r.mem[6], r.mem[7], (unsigned)r.fpsr);
                printf("  there fp0 %02x%02x %016llx fp2 %02x%02x %016llx mem %02x%02x%02x%02x%02x%02x%02x%02x fpsr %08x\n", o.fp0[0], o.fp0[1], (unsigned long long)__builtin_bswap64(*(uint64_t *)(o.fp0 + 4)),
                       o.fp2[0], o.fp2[1], (unsigned long long)__builtin_bswap64(*(uint64_t *)(o.fp2 + 4)), o.mem[0], o.mem[1], o.mem[2], o.mem[3], o.mem[4], o.mem[5], o.mem[6], o.mem[7], (unsigned)o.fpsr);
            }
        }
        if (!dump) printf("%ld cases: %ld exact ones differ; of %ld transcendental ones %ld differ (1 ulp %ld, 2 ulps %ld, more %ld, other %ld)\n",
                          n, diff_exact, trans, diff_trans, hist[0], hist[1], hist[2], hist[3]);
        fclose(f);
        return dump ? 0 : diff_exact != 0;
    }
    fixed();
    uint64_t he = 0xcbf29ce484222325ull, ht = he;
    for (long i = 0; i < CASES; i++) {
        rec r;
        one(&r);
        if (rec_trans(&r)) ht = rec_hash(ht, &r); else he = rec_hash(he, &r);
    }
    printf("fpu_test: %s FPU, exact hash %016llx, transcendental hash %016llx\n", AC090_FPU_SOFT ? "integer" : "x87",
           (unsigned long long)he, (unsigned long long)ht);
    CHECK(he == HASH_EXACT, "the exact operations' hash: %016llx, want %016llx", (unsigned long long)he, HASH_EXACT);
#if AC090_FPU_SOFT
    CHECK(ht == HASH_TRANS_SOFT, "the transcendentals' hash: %016llx, want %016llx", (unsigned long long)ht, HASH_TRANS_SOFT);
#endif
    printf(fails ? "fpu_test: %d failed\n" : "fpu_test: all passed\n", fails);
    return fails != 0;
}
