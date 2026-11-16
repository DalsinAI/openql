/*
 * AC090's 68040 MMU, included by ac090.c. From the MC68040 User's Manual,
 * section 3 (memory management); no emulator code is used.
 *
 * - TC: E (bit 15) turns translation on, P (bit 14) picks 8K pages over 4K.
 * - Transparent translation: ITT0/1 for program space, DTT0/1 for data. A
 *   match (address bits 31-24 against the base, under the mask, with the S
 *   field) passes the address through; W makes it write protected.
 * - Otherwise a three-level table walk from URP (user) or SRP (supervisor):
 *   root index A31-A25, pointer index A24-A18, page index A17-A12 (4K) or
 *   A17-A13 (8K). Root and pointer descriptors are resident when UDT bit 1
 *   is set; page descriptors when PDT is 01 or 11, and PDT 10 is indirect.
 *   The walk sets U in each descriptor it passes and M in the page
 *   descriptor on a write. W anywhere on the path protects the page; S in
 *   the page descriptor makes it supervisor only.
 * - The ATC keeps 64 pages each for data and program, direct mapped. PFLUSH
 *   clears entries; PTEST walks and fills MMUSR.
 * - A failed translation is an access error: vector 2, a format 7 frame
 *   (EA, SSW with ATC set, FA). The instruction restarts from its first
 *   word after RTE, so the data and address registers go back to how the
 *   instruction found them. The write-back slots are empty: the core has no
 *   store buffer, every write completes before the next instruction.
 * - Cache modes (CM) and U0/U1 are kept in descriptors and MMUSR but have
 *   no effect: the core models no data cache.
 */

enum { MMU_OK = 0, MMU_INVALID, MMU_WP, MMU_SUPER };
typedef struct { int status; uint32_t pa; uint32_t desc; int wp, super, mod, global, transparent; } mmu_walk_t;

static inline int mmu_shift(const ac090 *c) { return (c->tc & 0x4000) ? 13 : 12; }

/* A transparent translation register match for this access. */
static int mmu_tt(const ac090 *c, uint32_t a, int super, int prog, int *wp) {
    const uint32_t *tt = prog ? &c->itt0 : &c->dtt0;
    for (int i = 0; i < 2; i++) {
        uint32_t t = tt[i];
        if (!(t & 0x8000)) continue;
        uint32_t base = t >> 24, msk = t >> 16 & 0xff;
        if ((((a >> 24) ^ base) & ~msk & 0xff) != 0) continue;
        int sf = t >> 13 & 3;
        if (!(sf & 2) && sf != super) continue;
        *wp = (t & 4) != 0;
        return 1;
    }
    return 0;
}

/* The table walk. update: set U and M as the 68040 does (not for PTEST's
 * look or the identity check). */
static mmu_walk_t mmu_walk(ac090 *c, uint32_t a, int write, int super, int update) {
    mmu_walk_t w = { MMU_INVALID, 0, 0, 0, 0, 0, 0, 0 };
    c->mmu_walks++;
    uint32_t root = (super ? c->srp : c->urp) & ~0x1ffu;
    uint32_t ra = root + (a >> 25) * 4, rd = prd32(c, ra);
    if (!(rd & 2)) return w;
    if (update && !(rd & 8)) pwr32(c, ra, rd | 8);
    w.wp |= (rd & 4) != 0;
    uint32_t pa_ = (rd & ~0x1ffu) + (a >> 18 & 0x7f) * 4, pd = prd32(c, pa_);
    if (!(pd & 2)) return w;
    if (update && !(pd & 8)) pwr32(c, pa_, pd | 8);
    w.wp |= (pd & 4) != 0;
    uint32_t da;
    if (mmu_shift(c) == 12) da = (pd & ~0xffu) + (a >> 12 & 0x3f) * 4;
    else da = (pd & ~0x7fu) + (a >> 13 & 0x1f) * 4;
    uint32_t d = prd32(c, da);
    if ((d & 3) == 2) { da = d & ~3u; d = prd32(c, da); }   /* indirect */
    if ((d & 3) == 0 || (d & 3) == 2) return w;
    w.desc = d;
    w.wp |= (d & 4) != 0;
    w.super = (d & 0x80) != 0;
    w.global = (d & 0x400) != 0;
    if (update) {
        uint32_t nd = d | 8;
        if (write && !w.wp && !(w.super && !super)) nd |= 0x10;
        if (nd != d) { pwr32(c, da, nd); d = nd; }
    }
    w.mod = (d & 0x10) != 0;
    w.pa = d & ~((1u << mmu_shift(c)) - 1);
    if (w.super && !super) w.status = MMU_SUPER;
    else if (write && w.wp) w.status = MMU_WP;
    else w.status = MMU_OK;
    return w;
}

/* The debugger's look (ac090_mmu_peek): a long word of the tables from a
 * page of memory, else 0 (an invalid descriptor): no bus cycle, no wait. */
static int peek32(const ac090 *c, uint32_t a, uint32_t *v) {
    const uint8_t *p = c->bus.rpage[a >> 16];
    if (!p || (a & 0xffff) > 0xfffc || (a & 3)) return 0;
    p += a & 0xffff;
    *v = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
    return 1;
}

int ac090_mmu_peek(const ac090 *c, uint32_t a, int prog, uint32_t *pa) {
    int wp, super = c->s != 0;
    if (!c->mmu_on || mmu_tt(c, a, super, prog, &wp)) { *pa = a; return 0; }
    uint32_t rd, pd, d, root = (super ? c->srp : c->urp) & ~0x1ffu;
    if (!peek32(c, root + (a >> 25) * 4, &rd) || !(rd & 2)) return -1;
    if (!peek32(c, (rd & ~0x1ffu) + (a >> 18 & 0x7f) * 4, &pd) || !(pd & 2)) return -1;
    uint32_t da = mmu_shift(c) == 12 ? (pd & ~0xffu) + (a >> 12 & 0x3f) * 4 : (pd & ~0x7fu) + (a >> 13 & 0x1f) * 4;
    if (!peek32(c, da, &d)) return -1;
    if ((d & 3) == 2 && !peek32(c, d & ~3u, &d)) return -1;   /* indirect */
    if ((d & 3) == 0 || (d & 3) == 2 || ((d & 0x80) && !super)) return -1;
    uint32_t off = (1u << mmu_shift(c)) - 1;
    *pa = (d & ~off) | (a & off);
    return 0;
}

/* The JIT's cache of looks that would hit (mmu_xc in ac090.h) forgets them
 * all: the ATC, a DTT or a device page's candidacy has changed. */
static void mmu_xc_forget(ac090 *c) {
    if (++c->mmu_xgen == 0) { memset(c->mmu_xc, 0, sizeof c->mmu_xc); c->mmu_xgen = 1; }
}
static void mmu_atc_flush(ac090 *c) { memset(c->atc_key, 0, sizeof c->atc_key); mmu_xc_forget(c); }

/* An access error, as the 68040 stacks it: format 7, vector 2. atc: the
 * tables refused it (a bus error from the board is not one). */
static void access_fault(ac090 *c, uint32_t a, int write, int fc, int size, int atc);
static void mmu_access_fault(ac090 *c, uint32_t a, int write, int fc, int size) {
    c->mmu_faults++;
    access_fault(c, a, write, fc, size, 1);
}
void ac090_bus_error(ac090 *c, uint32_t a, int write, int size) {
    access_fault(c, a, write, fc_data(c), size, 0);
}
static void access_fault(ac090 *c, uint32_t a, int write, int fc, int size, int atc) {
    c->mmu_fc = 0;
    if (c->mmu_in_fault) { c->fault = 1; c->halted = 1; longjmp(c->abort, 1); }   /* a fault while stacking one: halt */
    c->mmu_in_fault = 1;
    c->excepted = 1;
    /* The 68040 restarts the instruction: registers it changed ((An)+,
     * -(An), a partly done MOVEM) go back to how it found them. */
    if (c->mmu_regs_valid) { memcpy(c->d, c->mmu_regs, sizeof c->mmu_regs); c->mmu_regs_valid = 0; }
    if (c->fault_hook) c->fault_hook(c, V_BUS, c->op_pc);
    uint16_t old = ac090_get_sr(c);
    save_sp(c);
    c->s = 1; c->t1 = 0; c->t0 = 0;
    load_sp(c);
    c->a[7] -= 60;
    uint32_t f = c->a[7];
    for (int i = 0; i < 60; i += 4) wr32(c, f + (uint32_t)i, 0);
    uint16_t ssw = (uint16_t)((atc ? 0x0400 : 0) |                    /* ATC: a translation fault */
                              (write ? 0 : 0x0100) |                  /* RW: 1 read */
                              (size == 1 ? 1 : size == 2 ? 2 : 0) << 5 |
                              (fc & 7));                              /* TM: the function code */
    wr16(c, f + 0, old);
    wr32(c, f + 2, c->op_pc);                                         /* restart the instruction */
    wr16(c, f + 6, 0x7000 | V_BUS * 4);
    wr32(c, f + 8, a);                                                /* EA */
    wr16(c, f + 12, ssw);
    wr32(c, f + 20, a);                                               /* FA */
    c->mmu_last_fa = a; c->mmu_last_pc = c->op_pc; c->mmu_last_ssw = ssw;
    c->pc = rd32(c, c->vbr + V_BUS * 4);
    c->stopped = 0;
    c->clock += 50;
    c->mmu_in_fault = 0;
    longjmp(c->abort, 2);
}

/* Transparent translation, then the ATC, then the walk: MMU_OK and the
 * physical address, or why not. */
static int mmu_lookup(ac090 *c, uint32_t a, int write, int super, int prog, uint32_t *pa) {
    int wp = 0;
    if (mmu_tt(c, a, super, prog, &wp)) {
        *pa = a;
        return write && wp ? MMU_WP : MMU_OK;
    }
    int sh = mmu_shift(c);
    uint32_t pg = a >> sh, key = (pg << 1 | (uint32_t)super) + 1, i = pg & 63;
    if (c->atc_key[prog][i] == key) {
        uint8_t b = c->atc_bits[prog][i];
        if (!write || (!(b & 1) && (b & 4))) {                        /* a write to an unmodified page walks, to set M */
            if ((b & 2) && !super) return MMU_SUPER;
            *pa = c->atc_pa[prog][i] | (a & ((1u << sh) - 1));
            return MMU_OK;
        }
    }
    mmu_walk_t w = mmu_walk(c, a, write, super, 1);
    if (w.status == MMU_INVALID) return MMU_INVALID;
    mmu_xc_forget(c);                                                 /* an entry replaced */
    c->atc_key[prog][i] = key;
    c->atc_pa[prog][i] = w.pa;
    c->atc_bits[prog][i] = (uint8_t)((w.wp ? 1 : 0) | (w.super ? 2 : 0) | (w.mod ? 4 : 0) | (w.global ? 8 : 0));
    *pa = w.pa | (a & ((1u << sh) - 1));
    return w.status;
}

static uint32_t mmu_xlate(ac090 *c, uint32_t a, int write, int fc, int size) {
    uint32_t pa = 0;
    if (mmu_lookup(c, a, write, fc >> 2 & 1, (fc & 3) == 2, &pa) != MMU_OK) mmu_access_fault(c, a, write, fc, size);
    return pa;
}

/* After a look that let the access through: the same look again would be
 * a DTT match or an ATC hit (a write's only once M is set and the page is
 * not protected) with nothing to change, and the page is no device page
 * still to be walked (ac090_mmu_io_touch), so the JIT may take its answer
 * from mmu_xc until something changes (mmu_xc_forget). */
static int io_cand(const ac090 *c, uint32_t pg, int super, int write);
static void mmu_xc_fill(ac090 *c, uint32_t a, int write, uint32_t pa) {
    uint32_t pg = a >> 12, super = c->s != 0;
    if (io_cand(c, pg, (int)super, write)) return;
    int wp = 0;
    if (!mmu_tt(c, a, (int)super, 0, &wp)) {
        uint32_t lp = a >> mmu_shift(c), i = lp & 63;
        if (c->atc_key[0][i] != (lp << 1 | super) + 1) return;
        if (write && ((c->atc_bits[0][i] & 1) || !(c->atc_bits[0][i] & 4))) return;
    }
    c->mmu_xc[write][pg & 127].tag = (pg << 1 | super) + 1;
    c->mmu_xc[write][pg & 127].gen = c->mmu_xgen;
    c->mmu_xc[write][pg & 127].pa = pa & ~0xfffu;
}
int ac090_mmu_probe(ac090 *c, uint32_t a, int write, uint32_t *pa) {
    if (!c->mmu_on) { *pa = a; return MMU_OK; }
    if (!c->mmu_xc_on) return mmu_lookup(c, a, write, c->s, 0, pa);
    int r = mmu_lookup(c, a, write, c->s, 0, pa);
    if (r == MMU_OK) mmu_xc_fill(c, a, write != 0, *pa);
    return r;
}

/* Can translated code, which uses guest addresses as they are, run under
 * this tree? It reaches host memory directly (page_backed says where), so
 * every page with host memory behind it must be one the tables leave alone:
 * valid, mapped to itself, not supervisor only, and write protected only if
 * the host keeps it read-only too (ROM), so that writes still fault into the
 * slow path. A page with no host memory behind it may be anything: translated
 * code that reaches it faults into the JIT's slow path, which translates.
 * AmigaOS 3.2's mmu.library maps the address space nothing answers to onto
 * one blank page, so most of the 4 GB is remapped that way; those pages are
 * counted. While it tests the MMU it makes a chip RAM page invalid, then
 * remaps it: the interpreter runs that, so the access faults it expects come. */
static int mmu_backed(ac090 *c, uint32_t la, uint32_t size) {
    if (!c->page_backed) return 1;
    if (c->range_backed) return c->range_backed(la, size);
    int kind = 0;
    for (uint32_t off = 0; off < size; off += 0x1000) {
        int k = c->page_backed(la + off);
        if (k > kind) kind = k;
        if (kind == 1) break;                    /* RAM: the strictest case */
        if (off + 0x1000 < off) break;
    }
    return kind;
}
/* The snapshot: a table's bytes as the walk read them, through the host
 * memory behind them (a table with none, or an indirect descriptor, makes
 * the snapshot useless and every PFLUSH looks again). */
static const uint8_t *table_host(ac090 *c, uint32_t a, uint32_t len) {
    uint8_t *p = c->bus.rpage[(a & c->addr_mask) >> 16];
    if (!p || (a & 0xffff) + len > 0x10000) return NULL;
    return p + (a & 0xffff);
}
/* The tables one look has met, by address: MMULib points every pointer
 * descriptor of unused space at one page table, so a 4 GB tree is a few
 * tables met thousands of times. Each goes into the snapshot once, and a
 * page table whose descriptors all send their pages to one place is read
 * once (uniform, pa). */
typedef struct { uint32_t a, len, pa; uint8_t used, uniform; } mmu_table_t;
#define MMU_TABLES (1u << 16)
static mmu_table_t *mmu_table(ac090 *c, uint32_t a) {
    mmu_table_t *t = c->mmu_tables;
    if (!t) {
        if (!(t = c->mmu_tables = calloc(MMU_TABLES, sizeof *t))) return NULL;
        c->mmu_tables_n = 0;
    }
    for (uint32_t h = (a >> 4) * 2654435761u >> 16, i = 0; i < MMU_TABLES; i++, h = (h + 1) & (MMU_TABLES - 1)) {
        if (t[h].used && t[h].a == a) return &t[h];
        if (!t[h].used) {
            if (c->mmu_tables_n >= MMU_TABLES / 4 * 3) return NULL;   /* full enough: no help */
            c->mmu_tables_n++;
            t[h] = (mmu_table_t){ .a = a, .used = 1 };
            return &t[h];
        }
    }
    return NULL;
}
static void snap_add(ac090 *c, uint32_t a, uint32_t len) {
    if (!c->mmu_snap_ok) return;
    mmu_table_t *t = mmu_table(c, a);
    if (t && t->len >= len) return;              /* in the snapshot already */
    if (t) t->len = len;
    const uint8_t *h = table_host(c, a, len);
    if (!h) { c->mmu_snap_ok = 0; return; }
    if (c->mmu_snap_len + 8 + len > c->mmu_snap_cap) {
        uint32_t cap = c->mmu_snap_cap ? c->mmu_snap_cap * 2 : 1u << 20;
        while (cap < c->mmu_snap_len + 8 + len) cap *= 2;
        uint8_t *n = realloc(c->mmu_snap, cap);
        if (!n) { c->mmu_snap_ok = 0; return; }
        c->mmu_snap = n; c->mmu_snap_cap = cap;
    }
    uint8_t *d = c->mmu_snap + c->mmu_snap_len;
    memcpy(d, &a, 4); memcpy(d + 4, &len, 4); memcpy(d + 8, h, len);
    c->mmu_snap_len += 8 + len;
}
/* Descriptors equal but for U and M (bits 3 and 4 of each long, big
 * endian: the low byte), which the walks themselves set and which do not
 * change where a page maps. len is a multiple of 8. */
static int same_but_um(const uint8_t *x, const uint8_t *y, uint32_t len) {
    static const uint8_t keep_b[8] = { 0xff, 0xff, 0xff, 0xe7, 0xff, 0xff, 0xff, 0xe7 };   /* two longs, U and M cleared */
    uint64_t keep;
    memcpy(&keep, keep_b, 8);                             /* the same bytes in memory order, whatever the host's */
    for (uint32_t i = 0; i < len; i += 8) {
        uint64_t p, q;
        memcpy(&p, x + i, 8); memcpy(&q, y + i, 8);
        if ((p ^ q) & keep) return 0;
    }
    return 1;
}
/* The tables are as the snapshot holds them. */
static int snap_same(ac090 *c) {
    if (!c->mmu_snap_ok || c->tc != c->mmu_snap_tc || c->urp != c->mmu_snap_urp || c->srp != c->mmu_snap_srp) return 0;
    if ((c->backed_gen ? *c->backed_gen : 0) != c->mmu_snap_gen) return 0;
    for (uint32_t o = 0; o < c->mmu_snap_len;) {
        uint32_t a, len;
        memcpy(&a, c->mmu_snap + o, 4); memcpy(&len, c->mmu_snap + o + 4, 4);
        const uint8_t *h = table_host(c, a, len);
        if (!h || !same_but_um(h, c->mmu_snap + o + 8, len)) return 0;
        o += 8 + len;
    }
    return 1;
}

/* A page the view moves (SPD-19): la shown from pa, read-only if wp. */
static int remap_add(ac090 *c, uint32_t la, uint32_t pa, int wp, uint32_t size) {
    for (uint32_t o = 0; o < size; o += 4096) {
        if (c->mmu_remap_n + 3 > c->mmu_remap_cap) {
            uint32_t cap = c->mmu_remap_cap ? c->mmu_remap_cap * 2 : 3 * 256;
            uint32_t *n = realloc(c->mmu_remap, cap * sizeof *n);
            if (!n) return 0;
            c->mmu_remap = n; c->mmu_remap_cap = cap;
        }
        c->mmu_remap[c->mmu_remap_n++] = la + o;
        c->mmu_remap[c->mmu_remap_n++] = pa + o;
        c->mmu_remap[c->mmu_remap_n++] = (uint32_t)wp;
    }
    return 1;
}

/* Device pages that map to themselves (mmu_io_ok in ac090.h). */
#define IO_WORDS AC090_IO_WORDS
static void io_mark(uint32_t *bm, uint32_t la, int sh) {
    for (uint32_t o = 0; o < 1u << sh; o += 4096) { uint32_t pg = (la + o) >> 12; bm[pg >> 5] |= 1u << (pg & 31); }
}
/* mmu_io_ok's bitmaps, IO_WORDS each: what the JIT reads (reads, writes);
 * the candidates the trees and DTTs pass (reads, writes); what the DTTs alone
 * pass (reads, writes); each mode's readiness, set by its first walk (user
 * reads, writes; supervisor reads, writes); and each mode's pages whose
 * descriptor is global (user, supervisor). */
enum { IO_OK_R, IO_OK_W, IO_CAND_R, IO_CAND_W, IO_TT_R, IO_TT_W,
       IO_RDY_U_R, IO_RDY_U_W, IO_RDY_S_R, IO_RDY_S_W, IO_GLOB_U, IO_GLOB_S, IO_MAPS };
#define IO_MAP(c, k) ((c)->mmu_io_ok + (size_t)(k) * IO_WORDS)
/* 4K page pg: that mode's walk has made it ready for the access. */
static int io_ready(const ac090 *c, uint32_t pg, int super, int write) {
    return c->mmu_io_ok[((super ? IO_RDY_S_R : IO_RDY_U_R) + (write ? 1 : 0)) * IO_WORDS + (pg >> 5)] >> (pg & 31) & 1;
}
/* A candidate this mode's next access would walk (ac090_mmu_io_touch). */
static int io_cand(const ac090 *c, uint32_t pg, int super, int write) {
    return c->mmu_io_ok && (c->mmu_io_ok[(write ? IO_CAND_W : IO_CAND_R) * IO_WORDS + (pg >> 5)] >> (pg & 31) & 1) &&
           !io_ready(c, pg, super, write);
}
/* What the JIT may use for bitmap word i: a candidate that each mode's DTT
 * passes or that mode's own walk has made ready. */
static void io_settle(ac090 *c, uint32_t i, int tu, int ts) {
    uint32_t *ok = c->mmu_io_ok;
    uint32_t ur = tu ? ~0u : ok[IO_RDY_U_R * IO_WORDS + i], uw = tu ? ~0u : ok[IO_RDY_U_W * IO_WORDS + i];
    uint32_t sr = ts ? ~0u : ok[IO_RDY_S_R * IO_WORDS + i], sw = ts ? ~0u : ok[IO_RDY_S_W * IO_WORDS + i];
    ok[IO_OK_R * IO_WORDS + i] = ok[IO_CAND_R * IO_WORDS + i] & ur & sr;
    ok[IO_OK_W * IO_WORDS + i] = ok[IO_CAND_W * IO_WORDS + i] & ur & sr & uw & sw;
}
static void io_settle_all(ac090 *c) {
    for (uint32_t r = 0; r < 256; r++) {
        int wp = 0, tu = mmu_tt(c, r << 24, 0, 0, &wp), ts = mmu_tt(c, r << 24, 1, 0, &wp);
        for (uint32_t i = r * 128; i < r * 128 + 128; i++) io_settle(c, i, tu, ts);
    }
}
/* The two trees' answers and the DTTs, which come first on every access: a
 * DTT that matches passes the address through (writes refused with its W),
 * else the tree for that mode decides. A 16 MB region at a time (a TT's
 * size). What a tree passes is a candidate until that mode's access has
 * walked it (U and M, ac090_mmu_io_touch); what the DTTs pass is ready. */
static void mmu_io_combine(ac090 *c) {
    mmu_xc_forget(c);                            /* a DTT, or the candidates, changed */
    if (!c->mmu_io_ok) return;
    const uint32_t *t = c->mmu_io_tree;
    uint32_t *cand = IO_MAP(c, IO_CAND_R), *tt = IO_MAP(c, IO_TT_R);
    memset(IO_MAP(c, IO_RDY_U_R), 0, (IO_MAPS - IO_RDY_U_R) * (size_t)IO_WORDS * sizeof *cand);   /* the tables may be new */
    for (uint32_t r = 0; r < 256; r++) {
        int wpu = 0, wps = 0;
        int tu = mmu_tt(c, r << 24, 0, 0, &wpu), ts = mmu_tt(c, r << 24, 1, 0, &wps);
        for (uint32_t i = r * 128; i < r * 128 + 128; i++) {
            uint32_t ru = tu ? ~0u : t[i], wu = tu ? (wpu ? 0 : ~0u) : t[IO_WORDS + i];
            uint32_t rs = ts ? ~0u : t[2 * IO_WORDS + i], ws = ts ? (wps ? 0 : ~0u) : t[3 * IO_WORDS + i];
            cand[i] = ru & rs;
            cand[IO_WORDS + i] = ru & rs & wu & ws;
            tt[i] = tu && ts ? ~0u : 0;
            tt[IO_WORDS + i] = tu && ts && !wpu && !wps ? ~0u : 0;
            io_settle(c, i, tu, ts);
        }
    }
}
/* A candidate device page's access in the JIT's slow path, after its own
 * look: the walk the 68040 makes in the current mode's tree, setting U in
 * the root, pointer and page descriptors and M for a write. Ready for that
 * mode only if the walk still maps the page to itself (the tables may have
 * changed since they were read); one tree for both modes makes both ready.
 * The page goes fast once every mode is ready or passed by a DTT, so no
 * later access leaves its descriptors looking unused or clean. */
void ac090_mmu_io_touch(ac090 *c, uint32_t a, int write) {
    uint32_t *ok = c->mmu_io_ok;
    if (!ok || !c->mmu_on) return;
    uint32_t pg = a >> 12, i = pg >> 5, bit = 1u << (pg & 31);
    if (!(ok[(write ? IO_CAND_W : IO_CAND_R) * IO_WORDS + i] & bit)) return;
    int super = c->s, wp = 0;
    if (io_ready(c, pg, super, write)) return;      /* this mode has walked it: the other one has yet to (two trees) */
    if (!mmu_tt(c, a, super, 0, &wp)) {
        mmu_walk_t w = mmu_walk(c, a, write, super, 1);
        if (w.status != MMU_OK || w.pa != (a & ~((1u << mmu_shift(c)) - 1))) return;
        for (int m = 0; m < 2; m++) {
            if (m != super && c->srp != c->urp) continue;
            ok[(m ? IO_RDY_S_R : IO_RDY_U_R) * IO_WORDS + i] |= bit;
            if (write) ok[(m ? IO_RDY_S_W : IO_RDY_U_W) * IO_WORDS + i] |= bit;
            if (w.global) ok[(m ? IO_GLOB_S : IO_GLOB_U) * IO_WORDS + i] |= bit;
            else ok[(m ? IO_GLOB_S : IO_GLOB_U) * IO_WORDS + i] &= ~bit;
        }
    }
    int tu = mmu_tt(c, a, 0, 0, &wp), ts = mmu_tt(c, a, 1, 0, &wp);
    io_settle(c, i, tu, ts);
}
/* PFLUSH: what the ATC forgets is walked again on its next access, so those
 * pages go back to candidates for that mode; with keep_global (PFLUSHN,
 * PFLUSHAN) pages whose descriptor is global stay, as their ATC entries do.
 * all: every page, both modes; else a's page in DFC's mode (super). */
static void mmu_io_forget(ac090 *c, int all, int keep_global, int super, uint32_t a) {
    uint32_t *ok = c->mmu_io_ok;
    if (!ok) return;
    if (all) {
        for (int m = 0; m < 2; m++) {
            uint32_t *g = IO_MAP(c, m ? IO_GLOB_S : IO_GLOB_U);
            for (int k = 0; k < 2; k++) {
                uint32_t *rdy = IO_MAP(c, (m ? IO_RDY_S_R : IO_RDY_U_R) + k);
                for (uint32_t i = 0; i < IO_WORDS; i++) rdy[i] &= keep_global ? g[i] : 0;
            }
        }
        io_settle_all(c);
        return;
    }
    for (uint32_t o = 0; o < 1u << mmu_shift(c); o += 4096) {
        uint32_t pg = ((a & ~((1u << mmu_shift(c)) - 1)) + o) >> 12, i = pg >> 5, bit = 1u << (pg & 31);
        if (keep_global && (ok[(super ? IO_GLOB_S : IO_GLOB_U) * IO_WORDS + i] & bit)) continue;
        ok[(super ? IO_RDY_S_R : IO_RDY_U_R) * IO_WORDS + i] &= ~bit;
        ok[(super ? IO_RDY_S_W : IO_RDY_U_W) * IO_WORDS + i] &= ~bit;
        int wp = 0, tu = mmu_tt(c, pg << 12, 0, 0, &wp), ts = mmu_tt(c, pg << 12, 1, 0, &wp);
        io_settle(c, i, tu, ts);
    }
}
/* Why a look left the tables to the interpreter, and where: for the
 * runtime's mmu line. */
static int why(ac090 *c, const char *what, uint32_t la) {
    snprintf(c->mmu_why_buf, sizeof c->mmu_why_buf, "%s at %08x", what, la);
    c->mmu_why = c->mmu_why_buf;
    return 0;
}
/* io: this tree's read and write bitmaps (NULL: none kept); super: the
 * supervisor's tree, where supervisor-only pages are fine. */
static int mmu_identity_tree(ac090 *c, uint32_t root, uint32_t *io, int super) {
    int sh = mmu_shift(c), n = sh == 12 ? 64 : 32;
    root &= ~0x1ffu;
    snap_add(c, root, 0x200);
    for (uint32_t ri = 0; ri < 128; ri++) {
        uint32_t rd = prd32(c, root + ri * 4);
        if (!(rd & 2)) { if (c->page_backed && mmu_backed(c, ri << 25, 1u << 25)) return why(c, "memory left invalid", ri << 25); continue; }
        snap_add(c, rd & ~0x1ffu, 0x200);
        for (uint32_t pi = 0; pi < 128; pi++) {
            uint32_t pd = prd32(c, (rd & ~0x1ffu) + pi * 4);
            if (!(pd & 2)) { if (c->page_backed && mmu_backed(c, ri << 25 | pi << 18, 1u << 18)) return why(c, "memory left invalid", ri << 25 | pi << 18); continue; }
            uint32_t pt = sh == 12 ? (pd & ~0xffu) : (pd & ~0x7fu);
            snap_add(c, pt, (uint32_t)n * 4);
            uint32_t la0 = ri << 25 | pi << 18;
            mmu_table_t *mt = c->page_backed ? mmu_table(c, pt) : NULL;
            if (mt && !mt->uniform) {                    /* first meeting: one place for all its pages? */
                uint32_t d0 = prd32(c, pt), pa0 = d0 & ~((1u << sh) - 1);
                mt->uniform = 2;
                if ((d0 & 3) == 1 || (d0 & 3) == 3) {
                    uint32_t k = 1;
                    for (; k < (uint32_t)n; k++) { uint32_t d = prd32(c, pt + k * 4); if (!((d & 3) == 1 || (d & 3) == 3) || (d & ~((1u << sh) - 1)) != pa0) break; }
                    if (k == (uint32_t)n) { mt->uniform = 1; mt->pa = pa0; }
                }
            }
            if (mt && mt->uniform == 1 && (mt->pa < la0 || mt->pa - la0 >= 1u << 18) && !mmu_backed(c, la0, 1u << 18)) {
                c->mmu_blank_pages += (uint32_t)n;   /* no memory here, all sent elsewhere: remapped blank pages */
                continue;
            }
            for (uint32_t k = 0; k < (uint32_t)n; k++) {
                uint32_t d = prd32(c, pt + k * 4);
                if ((d & 3) == 2) { c->mmu_snap_ok = 0; d = prd32(c, d & ~3u); }   /* indirect: not in the snapshot */
                uint32_t la = ri << 25 | pi << 18 | k << sh;
                int backed = mmu_backed(c, la, 1u << sh);
                int valid = (d & 3) == 1 || (d & 3) == 3;
                if (!backed) {
                    if (valid && (d & ~((1u << sh) - 1)) != la) c->mmu_blank_pages++;
                    else if (valid && io && (super || !(d & 0x80))) {   /* a device page left where it is */
                        io_mark(io, la, sh);
                        if (!((d | rd | pd) & 4)) io_mark(io + IO_WORDS, la, sh);
                    }
                    continue;
                }
                if (!valid) { if (c->page_backed) return why(c, "memory left invalid", la); continue; }   /* no JIT: as before */
                if ((d & 0x80) && !super) return why(c, "supervisor-only page", la);   /* in the supervisor's own tree, fine */
                int wp = ((d | rd | pd) & 4) != 0;
                uint32_t pa = d & ~((1u << sh) - 1);
                if (wp && backed != 2 && pa == la) {                     /* write-protected RAM (MuFastRom's PROTECT): read-only in the view */
                    if (!c->mmu_can_view) return why(c, "write-protected RAM", la);
                    if (!remap_add(c, la, la, 1, 1u << sh)) return why(c, "write-protected RAM the view cannot show", la);
                } else if (pa != la) {                                   /* moved: through the view, when the JIT has one (SPD-19) */
                    if (!c->mmu_can_view) return why(c, "page moved, no view", la);
                    if (!mmu_backed(c, pa, 1u << sh) || !remap_add(c, la, pa, wp, 1u << sh)) return why(c, "page moved where the view cannot show it", la);
                }
            }
        }
    }
    return 1;
}
static int mmu_jit_ok(ac090 *c) {
    c->mmu_blank_pages = 0;
    c->mmu_snap_len = 0; c->mmu_snap_ok = 1;
    if (c->mmu_tables) { memset(c->mmu_tables, 0, MMU_TABLES * sizeof(mmu_table_t)); c->mmu_tables_n = 0; }
    c->mmu_snap_tc = c->tc; c->mmu_snap_urp = c->urp; c->mmu_snap_srp = c->srp;
    c->mmu_snap_gen = c->backed_gen ? *c->backed_gen : 0;
    c->mmu_jit_walks++;
    c->mmu_remap_n = 0;
    if (c->mmu_on && !c->mmu_io_ok) {
        c->mmu_io_ok = calloc((size_t)IO_MAPS * IO_WORDS, sizeof *c->mmu_io_ok);
        c->mmu_io_tree = calloc(4 * IO_WORDS, sizeof *c->mmu_io_tree);
        if (!c->mmu_io_tree) { free(c->mmu_io_ok); c->mmu_io_ok = NULL; }
    }
    uint32_t *iou = c->mmu_io_tree, *ios = iou ? iou + 2 * IO_WORDS : NULL;
    if (iou) memset(iou, 0, 4 * IO_WORDS * sizeof *iou);
    c->mmu_why = NULL;
    int ok = !c->mmu_on || mmu_identity_tree(c, c->urp, iou, 0);
    uint32_t user_moved = c->mmu_remap_n;
    if (ok && c->srp != c->urp) ok = mmu_identity_tree(c, c->srp, ios, 1);
    if (iou) {
        if (!ok || !c->mmu_on) memset(iou, 0, 4 * IO_WORDS * sizeof *iou);   /* a walk cut short: none */
        else if (c->srp == c->urp) memcpy(ios, iou, 2 * IO_WORDS * sizeof *iou);   /* one tree, its user answer for both */
        mmu_io_combine(c);
    }
    /* two trees: one view serves both when they move the same pages the
     * same way (as MMULib's user and supervisor contexts do); else phase 3 */
    if (ok && c->mmu_on && c->srp != c->urp && c->mmu_remap_n) {
        uint32_t n = c->mmu_remap_n - user_moved;
        if (n != user_moved || memcmp(c->mmu_remap, c->mmu_remap + user_moved, n * sizeof *c->mmu_remap))
            ok = why(c, "user and supervisor trees move pages differently", 0);
        else c->mmu_remap_n = user_moved;
    }
    if (!c->mmu_on) c->mmu_snap_ok = 0;
    /* a look cut short still covers what it read, which decided it: the
     * same tables give the same answer, so PFLUSH need not look again */
    int view = ok && c->mmu_on && c->mmu_remap_n;
    size_t bytes = (size_t)c->mmu_remap_n * sizeof *c->mmu_remap;
    int same = view && c->mmu_view && c->mmu_remap_shown && c->mmu_remap_shown_n == c->mmu_remap_n &&
               !memcmp(c->mmu_remap_shown, c->mmu_remap, bytes);
    if ((view || c->mmu_view) && !same) {             /* the JIT builds the view again, or drops it */
        c->mmu_view_gen++;
        c->mmu_remap_shown_n = 0;
        uint32_t *n = view ? realloc(c->mmu_remap_shown, bytes) : NULL;
        if (n) { memcpy(n, c->mmu_remap, bytes); c->mmu_remap_shown = n; c->mmu_remap_shown_n = c->mmu_remap_n; }
    }
    c->mmu_view = view;
    return ok;
}
static void mmu_changed(ac090 *c) {
    int was_on = c->mmu_on;
    c->mmu_on = c->model == AC090_68040 && (c->tc & 0x8000);
    mmu_atc_flush(c);
    /* the same TC and roots loaded again (MMULib does, often): the same
     * tables, the same view, so translations stand. Until 20 Oct 2026 each
     * such load checked every translation again as a cache clear */
    if (was_on && c->mmu_on && snap_same(c)) {
        mmu_io_forget(c, 1, 0, 0, 0);            /* the ATC forgot: walked again */
        c->mmu_jit_walks_saved++;
        c->clr_n[AC090_CLR_MMU_SAME]++;
        return;
    }
    if (was_on || c->mmu_on) { c->cache_epoch++; c->clr_n[AC090_CLR_MMU]++; }   /* translations are checked again */
    else c->clr_n[AC090_CLR_MMU_SAME]++;         /* off and still off: every address as it was */
    c->mmu_identity = mmu_jit_ok(c);
}

void ac090_mmu_off(ac090 *c) {
    if (!(c->tc & 0x8000)) return;
    c->tc &= ~0x8000u;
    mmu_changed(c);
}

/* PFLUSH (An), PFLUSHN (An), PFLUSHA, PFLUSHAN: clear ATC entries, and look
 * at the tables again. The (An) forms use DFC for user or supervisor. */
static void mmu_pflush(ac090 *c, int opmode, uint32_t a) {
    int all = opmode >= 2, keep_global = !(opmode & 1);
    int sh = mmu_shift(c), super = c->dfc >> 2 & 1;
    uint32_t pg = a >> sh, key = (pg << 1 | (uint32_t)super) + 1;
    mmu_xc_forget(c);
    for (int t = 0; t < 2; t++)
        for (int i = 0; i < 64; i++) {
            if (!c->atc_key[t][i]) continue;
            if (!all && c->atc_key[t][i] != key) continue;
            if (keep_global && (c->atc_bits[t][i] & 8)) continue;
            c->atc_key[t][i] = 0;
        }
    mmu_io_forget(c, all, keep_global, super, a);
    if (snap_same(c)) { c->mmu_jit_walks_saved++; return; }   /* the tables as last seen: the same answer */
    int was = c->mmu_identity;
    c->mmu_identity = mmu_jit_ok(c);
    if (was != c->mmu_identity) { c->cache_epoch++; c->clr_n[AC090_CLR_MMU]++; }
}

/* PTESTR/PTESTW (An): the translation for DFC's space, into MMUSR: physical
 * address, G, U1/U0, S, CM, M, W, T (transparent), R (resident). The ATC is
 * loaded as by a normal access. */
static void mmu_ptest(ac090 *c, int write, uint32_t a) {
    int super = c->dfc >> 2 & 1, prog = (c->dfc & 3) == 2, wp = 0;
    if (mmu_tt(c, a, super, prog, &wp)) { c->mmusr = (a & ~0xfffu) | 0x2 | (wp ? 4 : 0); return; }
    mmu_walk_t w = mmu_walk(c, a, write, super, 1);
    if (w.status == MMU_INVALID) { c->mmusr = 0; return; }
    c->mmusr = w.pa | (w.desc & 0x7e0) | (w.wp ? 4 : 0) | (w.mod ? 0x10 : 0) | 1;
    mmu_xc_forget(c);
    int sh = mmu_shift(c);
    uint32_t pg = a >> sh, i = pg & 63;
    c->atc_key[prog][i] = (pg << 1 | (uint32_t)super) + 1;
    c->atc_pa[prog][i] = w.pa;
    c->atc_bits[prog][i] = (uint8_t)((w.wp ? 1 : 0) | (w.super ? 2 : 0) | (w.mod ? 4 : 0) | (w.global ? 8 : 0));
}
