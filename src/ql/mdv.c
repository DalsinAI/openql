/*
 * The QL's microdrives, as the ZX8302 sees them (ql.h has the state).
 *
 * The tape is a loop of cells, one byte time each (40 us: 300 clocks of the
 * 68008's 7.5 MHz). A cell holds the byte recorded there, or a gap (no
 * signal). The tape moves under the head only while its drive is selected,
 * so QDOS's own microdrive code runs unchanged: it waits for a gap and for
 * its end, resets the PLL, and reads each byte from pc_trak1/2 when
 * pc..rxrd says one is ready; it writes each byte to pc_tdata when pc..txfl
 * says the last one has gone. FORMAT writes the whole loop, wraps over its
 * own first sectors and verifies, as on a real cartridge.
 *
 * A real loop is joined with a splice, and QDOS's FORMAT counts on it: a
 * cartridge on which every sector verifies has not been all the way round,
 * so FORMAT fails. The last SPLICE cells of the loop record nothing.
 *
 * The PLL: after a reset it hunts for a preamble (zeros, then two $FFs) and
 * passes on each byte after it, until a gap. A gap's start raises the gap
 * interrupt while it is enabled; enabling it in a gap raises it too.
 *
 * Images are QLAY's and the MiSTer QL core's: 255 sectors of 686 bytes, each
 * a 28-byte header block (10 zeros, 2 $FFs, $FF, the sector number, the
 * medium name, a random word, a checksum) and a 658-byte data block (10
 * zeros, 2 $FFs, file and block numbers and their checksum, 6 zeros and 2
 * $FFs, 512 bytes and their checksum, 120 more). On the tape a gap comes
 * before each block. Written from Minerva's microdrive sources (its md
 * folder and inc/pc) and the format's description.
 */
#include "ql.h"
#include <stdlib.h>
#include <string.h>

#define CELL        300u            /* clocks a byte takes to pass the head */
#define TAPE_CELLS  180000u         /* a blank loop: 7.2 s, room for about 225 formatted sectors */
#define GAP_CELLS   71              /* 2.84 ms, the gap QDOS leaves when it formats */
#define HDR_BYTES   28
#define BLK_BYTES   (QL_MDV_SECTOR - HDR_BYTES)
#define SYNC        12              /* 10 zeros and 2 $FFs */
#define SPLICE      200             /* the tape's join, at the loop's end: nothing records there */

/* ---- images and tapes ------------------------------------------------- */

static int formatted(const uint8_t *rec) {
    return rec[10] == 0xff && rec[11] == 0xff && rec[12] == 0xff;
}

uint16_t *ql_mdv_from_image(const uint8_t *img, long n, uint32_t *len) {
    long recs = n / QL_MDV_SECTOR, used = 0;
    for (long r = 0; r < recs; r++) used += formatted(img + r * QL_MDV_SECTOR);
    uint32_t need = (uint32_t)used * (2 * GAP_CELLS + QL_MDV_SECTOR) + GAP_CELLS + SPLICE;
    *len = need > TAPE_CELLS ? need : TAPE_CELLS;
    uint16_t *t = malloc(*len * sizeof *t);
    if (!t) return NULL;
    for (uint32_t i = 0; i < *len; i++) t[i] = QL_MDV_GAP;
    uint32_t p = 0;
    for (long r = 0; r < recs; r++) {
        const uint8_t *rec = img + r * QL_MDV_SECTOR;
        if (!formatted(rec)) continue;
        p += GAP_CELLS;
        for (int i = 0; i < HDR_BYTES; i++) t[p++] = rec[i];
        p += GAP_CELLS;
        for (int i = 0; i < BLK_BYTES; i++) t[p++] = rec[HDR_BYTES + i];
    }
    return t;
}

/* The bytes after a block's preamble: where they start in the run of cells
 * from s (length n), or -1 if the run has no preamble. */
static int sync_at(const uint16_t *tape, uint32_t len, uint32_t s, uint32_t n) {
    int zeros = 0, ffs = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint16_t v = tape[(s + i) % len];
        if (v == 0) { zeros = 1; ffs = 0; }
        else if (v == 0xff && zeros) { if (++ffs == 2) return (int)i + 1; }
        else zeros = ffs = 0;
    }
    return -1;
}

int ql_mdv_to_image(const uint16_t *tape, uint32_t len, uint8_t *out) {
    memset(out, 0, QL_MDV_IMAGE);
    uint32_t start = 0;
    while (start < len && tape[start] != QL_MDV_GAP) start++;
    if (start == len) return 0;                     /* no gap anywhere: nothing a QL wrote */
    /* the runs of signal, in tape order from the first gap */
    int sectors = 0, have_hdr = 0;
    uint8_t *rec = NULL;
    uint32_t i = 0;
    while (i < len && sectors <= QL_MDV_SECTORS) {
        uint32_t p = (start + i) % len;
        if (tape[p] == QL_MDV_GAP) { i++; continue; }
        uint32_t n = 0;
        while (i + n < len && tape[(p + n) % len] != QL_MDV_GAP) n++;
        int k = sync_at(tape, len, p, n);
        if (k >= 0) {
            uint32_t body = n - (uint32_t)k;
            int is_hdr = body >= 16 && body < 100 && tape[(p + (uint32_t)k) % len] == 0xff;
            if (is_hdr) {
                if (sectors == QL_MDV_SECTORS) break;
                rec = out + sectors++ * QL_MDV_SECTOR;
                memset(rec, 0, SYNC);
                rec[10] = rec[11] = 0xff;
                for (int b = 0; b < 16; b++) rec[SYNC + b] = (uint8_t)tape[(p + (uint32_t)k + (uint32_t)b) % len];
                have_hdr = 1;
            } else if (have_hdr) {                  /* the data block after a header */
                uint8_t *blk = rec + HDR_BYTES;
                memset(blk, 0, SYNC);
                blk[10] = blk[11] = 0xff;
                for (uint32_t b = 0; b < body && b < BLK_BYTES - SYNC; b++) blk[SYNC + b] = (uint8_t)tape[(p + (uint32_t)k + b) % len];
                have_hdr = 0;
            }
        }
        i += n;
    }
    return sectors;
}

/* ---- cartridges ------------------------------------------------------- */

int ql_mdv_insert(ql *q, int drive, const char *path, int read_only) {
    if (drive < 0 || drive >= QL_MDV_DRIVES) return -1;
    uint8_t *img = NULL;
    long n = 0;
    FILE *f = fopen(path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n < 0 || n > 4 * QL_MDV_IMAGE || n % QL_MDV_SECTOR) {
            fclose(f);
            fprintf(stderr, "ql: %s is %ld bytes, not a microdrive image (sectors of %d bytes)\n", path, n, QL_MDV_SECTOR);
            return -1;
        }
        img = malloc((size_t)n + 1);
        if (!img || (long)fread(img, 1, (size_t)n, f) != n) { fclose(f); free(img); fprintf(stderr, "ql: cannot read %s\n", path); return -1; }
        fclose(f);
        if (!read_only && !(f = fopen(path, "r+b"))) read_only = 1;   /* a read-only file: the tab is out */
        else if (!read_only) fclose(f);
    }
    uint32_t len;
    uint16_t *tape = ql_mdv_from_image(img ? img : (const uint8_t *)"", n, &len);
    free(img);
    if (!tape) return -1;
    ql_mdv_eject(q, drive);
    ql_mdv *d = &q->mdv[drive];
    d->tape = tape;
    d->len = len;
    d->pos = 0;
    d->at = q->cpu.clock;
    d->was_gap = 1;
    d->read_only = read_only;
    d->dirty = 0;
    d->path = malloc(strlen(path) + 1);
    if (d->path) strcpy(d->path, path);
    return 0;
}

int ql_mdv_save(ql *q, int drive) {
    if (drive < 0 || drive >= QL_MDV_DRIVES) return -1;
    ql_mdv *d = &q->mdv[drive];
    if (!d->tape || !d->dirty || d->read_only || !d->path) return 0;
    uint8_t *img = malloc(QL_MDV_IMAGE);
    if (!img) return -1;
    ql_mdv_to_image(d->tape, d->len, img);
    FILE *f = fopen(d->path, "wb");
    int ok = f && fwrite(img, 1, QL_MDV_IMAGE, f) == QL_MDV_IMAGE;
    if (f && fclose(f)) ok = 0;
    free(img);
    if (!ok) { fprintf(stderr, "ql: cannot write %s\n", d->path); return -1; }
    d->dirty = 0;
    return 0;
}

void ql_mdv_eject(ql *q, int drive) {
    if (drive < 0 || drive >= QL_MDV_DRIVES) return;
    ql_mdv *d = &q->mdv[drive];
    if (!d->tape) return;
    ql_mdv_save(q, drive);
    free(d->tape);
    free(d->path);
    memset(d, 0, sizeof *d);
}

/* ---- the tape moving -------------------------------------------------- */

/* The next cell comes under the head: the head writes or erases it, or the
 * PLL reads it. */
static void step(ql *q, ql_mdv *d) {
    d->pos = (d->pos + 1) % d->len;
    uint16_t *c = &d->tape[d->pos];
    int writing = q->mctrl & 0x04, erasing = q->mctrl & 0x08;
    if (writing || erasing) {
        uint16_t v = writing ? (q->mdv_txfl ? q->mdv_tx : 0x00) : QL_MDV_GAP;   /* the buffer empty: zeros go out */
        if (writing) q->mdv_txfl = 0;
        if (!d->read_only && d->pos < d->len - SPLICE) { *c = v; d->dirty = 1; }
        d->was_gap = *c == QL_MDV_GAP;
        return;
    }
    int gap = *c == QL_MDV_GAP;
    if (gap && !d->was_gap && (q->intr_mask & 0x20)) q->intr_pending |= 0x01;   /* a gap starts */
    d->was_gap = gap;
    if (gap) { q->mdv_sync = q->mdv_zeros = q->mdv_ffs = 0; return; }
    uint8_t v = (uint8_t)*c;
    if (q->mdv_sync) { q->mdv_rx = v; q->mdv_rxrd = 1; return; }
    if (v == 0) { q->mdv_zeros = 1; q->mdv_ffs = 0; }
    else if (v == 0xff && q->mdv_zeros) { if (++q->mdv_ffs == 2) q->mdv_sync = 1; }
    else q->mdv_zeros = q->mdv_ffs = 0;
}

void ql_mdv_advance(ql *q) {
    if (q->mdv_run < 0) return;
    ql_mdv *d = &q->mdv[q->mdv_run];
    if (!d->tape) return;
    while (q->cpu.clock >= d->at + CELL) {
        d->at += CELL;
        step(q, d);
    }
}

uint64_t ql_mdv_next(ql *q) {
    if (q->mdv_run < 0 || !q->mdv[q->mdv_run].tape) return UINT64_MAX;
    return q->mdv[q->mdv_run].at + 4 * CELL;        /* a few cells at a time, so a gap's interrupt comes on time */
}

/* The select chain changed: the drive selected runs, the others stop. A
 * drive that stops after a write saves its image. */
void ql_mdv_select(ql *q) {
    int run = -1;
    for (int i = 0; i < QL_MDV_DRIVES; i++) if (q->mdv_sel >> i & 1) { run = i; break; }
    if (run == q->mdv_run) return;
    if (q->mdv_run >= 0) ql_mdv_save(q, q->mdv_run);
    q->mdv_run = run;
    if (run >= 0) { q->mdv[run].at = q->cpu.clock; q->mdv[run].was_gap = 1; }
}

/* A write to pc_mctrl in read mode resets the PLL and the read buffer. */
void ql_mdv_control(ql *q, uint8_t v) {
    if (!(v & 0x04)) { q->mdv_sync = q->mdv_zeros = q->mdv_ffs = 0; q->mdv_rxrd = 0; }
    if (!(v & 0x0c)) q->mdv_txfl = 0;
}

int ql_mdv_gap(ql *q) {
    if (q->mdv_run < 0 || !q->mdv[q->mdv_run].tape) return 1;   /* set normally */
    if (q->mctrl & 0x0c) return 1;
    return q->mdv[q->mdv_run].tape[q->mdv[q->mdv_run].pos] == QL_MDV_GAP;
}
