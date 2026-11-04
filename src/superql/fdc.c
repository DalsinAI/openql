/*
 * The multi-I/O card's floppy controller (fdc.h).
 */
#include "fdc.h"
#include <string.h>

#define TRACKS 80
#define SECTOR 512

enum { MSR_RQM = 0x80, MSR_DIO = 0x40, MSR_NDM = 0x20, MSR_CB = 0x10 };
enum { P_CMD, P_READ, P_WRITE, P_RESULT };

#ifdef _WIN32
#define img_seek(f, o) _fseeki64(f, (__int64)(o), SEEK_SET)
#define img_size(f) (_fseeki64(f, 0, SEEK_END) ? -1 : (long long)_ftelli64(f))
#else
#define img_seek(f, o) fseeko(f, (off_t)(o), SEEK_SET)
#define img_size(f) (fseeko(f, 0, SEEK_END) ? -1 : (long long)ftello(f))
#endif

static int cmd_len(uint8_t c) {
    switch (c & 0x1f) {
    case 0x02: case 0x05: case 0x06: return 9;   /* READ TRACK, WRITE DATA, READ DATA */
    case 0x03: return 3;                         /* SPECIFY */
    case 0x04: case 0x07: case 0x0a: case 0x12: return 2;   /* SENSE DRIVE STATUS, RECALIBRATE, READ ID, PERPENDICULAR */
    case 0x0d: return 6;                         /* FORMAT TRACK */
    case 0x0f: return 3;                         /* SEEK */
    case 0x13: return 4;                         /* CONFIGURE */
    default: return 1;                           /* SENSE INTERRUPT, VERSION, DUMPREG, LOCK, and the invalid ones */
    }
}

void fdc_reset(sql_fdc *f) {
    f->phase = P_CMD;
    f->ncmd = f->nres = f->rpos = 0;
    f->int_pending = 0;
    f->int_n = 4;                                /* polling mode: one status for each of the four drive numbers */
    for (int i = 0; i < 4; i++) f->int_st0[i] = (uint8_t)(0xc0 | i);
    if (!f->locked) { f->config[1] = 0; f->config[2] = 0; }
}

int fdc_insert(sql_fdc *f, int drive, const char *path, int read_only) {
    if (drive < 0 || drive >= FDC_DRIVES) return -1;
    FILE *fp = fopen(path, read_only ? "rb" : "r+b");
    if (!fp && !read_only && (fp = fopen(path, "rb"))) read_only = 1;   /* a read-only file: the disk is write protected */
    if (!fp) { fprintf(stderr, "fdc: cannot open %s\n", path); return -1; }
    long long n = img_size(fp);
    int spt = n > 0 && n % (TRACKS * 2 * SECTOR) == 0 ? (int)(n / (TRACKS * 2 * SECTOR)) : 0;
    if (n != 0 && (spt < 1 || spt > 40)) {
        fclose(fp);
        fprintf(stderr, "fdc: %s is %lld bytes, not a floppy image of 80 tracks, 2 sides, 512-byte sectors\n", path, n);
        return -1;
    }
    fdc_eject(f, drive);
    fdc_drive *d = &f->d[drive];
    d->f = fp;
    d->read_only = read_only;
    d->spt = spt;                                /* 0: an empty file, a disk not yet formatted */
    d->changed = 1;
    return 0;
}

void fdc_eject(sql_fdc *f, int drive) {
    if (drive < 0 || drive >= FDC_DRIVES) return;
    fdc_drive *d = &f->d[drive];
    if (d->f) { fclose(d->f); d->f = NULL; d->changed = 1; }
    d->spt = 0;
}

static fdc_drive *drive_of(sql_fdc *f, int n) { return n < FDC_DRIVES ? &f->d[n] : NULL; }

static void result(sql_fdc *f, const uint8_t *r, int n) {
    memcpy(f->res, r, (size_t)n);
    f->nres = n;
    f->rpos = 0;
    f->phase = P_RESULT;
}

static void rw_result(sql_fdc *f, uint8_t st0, uint8_t st1, uint8_t st2) {
    uint8_t r[7] = { (uint8_t)(st0 | f->head << 2 | f->drv), st1, st2, (uint8_t)f->c, (uint8_t)f->h, (uint8_t)f->r, (uint8_t)f->n };
    result(f, r, 7);
    f->int_pending = 1;                          /* the result phase raises INT */
    f->int_st0[0] = r[0];
}

static long long offset(const fdc_drive *d, int c, int head, int r) {
    return (((long long)c * 2 + head) * d->spt + (r - 1)) * SECTOR;
}

/* The next sector of a READ, or the end of the transfer. */
static void read_next(sql_fdc *f) {
    fdc_drive *d = drive_of(f, f->drv);
    if (f->r > f->eot) {
        if ((f->cmd[0] & 0x80) && f->head == 0) { f->head = 1; f->h = 1; f->r = 1; }   /* multi-track: side 1 next */
        else { f->c++; f->r = 1; rw_result(f, 0x40, 0x80, 0); return; }               /* end of cylinder, as without TC */
    }
    if (!d || !d->f || !d->spt || f->c >= TRACKS || f->r < 1 || f->r > d->spt || f->n != 2) {
        rw_result(f, 0x40, 0x04, 0);             /* no data: the sector is not on the disk */
        return;
    }
    if (img_seek(d->f, offset(d, f->c, f->head, f->r)) || fread(f->buf, 1, SECTOR, d->f) != SECTOR) {
        rw_result(f, 0x40, 0x20, 0x20);          /* data error */
        return;
    }
    f->pos = 0;
    f->len = SECTOR;
    f->phase = P_READ;
}

static void begin_rw(sql_fdc *f, int write) {
    f->drv = f->cmd[1] & 3;
    f->head = f->cmd[1] >> 2 & 1;
    f->c = f->cmd[2]; f->h = f->cmd[3]; f->r = f->cmd[4]; f->n = f->cmd[5]; f->eot = f->cmd[6];
    fdc_drive *d = drive_of(f, f->drv);
    if (!d || !d->f) { rw_result(f, 0x48, 0, 0); return; }               /* abnormal end, not ready: no disk */
    d->track = (uint8_t)f->c;
    if (write && d->read_only) { rw_result(f, 0x40, 0x02, 0); return; }  /* not writable */
    if (!write) { if ((f->cmd[0] & 0x1f) == 0x02) f->r = 1; read_next(f); return; }
    if (!d->spt || f->c >= TRACKS || f->r < 1 || f->r > d->spt || f->n != 2) { rw_result(f, 0x40, 0x04, 0); return; }
    f->pos = 0;
    f->len = SECTOR;
    f->phase = P_WRITE;
}

static void write_sector(sql_fdc *f) {
    fdc_drive *d = drive_of(f, f->drv);
    if (img_seek(d->f, offset(d, f->c, f->head, f->r)) || fwrite(f->buf, 1, SECTOR, d->f) != SECTOR) {
        rw_result(f, 0x40, 0x20, 0x20);
        return;
    }
    fflush(d->f);
    f->r++;
    if (f->r > f->eot) {
        if ((f->cmd[0] & 0x80) && f->head == 0) { f->head = 1; f->h = 1; f->r = 1; }
        else { f->c++; f->r = 1; rw_result(f, 0x40, 0x80, 0); return; }
    }
    if (f->r > d->spt) { rw_result(f, 0x40, 0x04, 0); return; }
    f->pos = 0;
    f->phase = P_WRITE;
}

/* FORMAT TRACK: each sector's ID (C H R N) comes in; the sector is filled. */
static void format_sector(sql_fdc *f) {
    fdc_drive *d = drive_of(f, f->drv);
    int c = f->buf[0], r = f->buf[2];
    if (!d->spt) {                               /* the first format of an empty image sets its sectors a track */
        d->spt = f->cmd[3];
        if (d->spt < 1 || d->spt > 40) d->spt = 9;
    }
    if (c < TRACKS && r >= 1 && r <= d->spt) {
        uint8_t fill[SECTOR];
        memset(fill, f->fmt_fill, sizeof fill);
        if (!img_seek(d->f, offset(d, c, f->head, r))) fwrite(fill, 1, SECTOR, d->f);
    }
    f->c = c; f->h = f->buf[1]; f->r = r; f->n = f->buf[3];
    if (--f->fmt_left > 0) { f->pos = 0; f->len = 4; return; }
    fflush(d->f);
    rw_result(f, 0, 0, 0);
}

static void execute(sql_fdc *f) {
    uint8_t *c = f->cmd;
    int drv = c[1] & 3;
    fdc_drive *d = drive_of(f, drv);
    switch (c[0] & 0x1f) {
    case 0x03:                                   /* SPECIFY */
        f->specify[0] = c[1]; f->specify[1] = c[2];
        f->phase = P_CMD;
        break;
    case 0x04: {                                 /* SENSE DRIVE STATUS: ST3 */
        uint8_t st3 = (uint8_t)(0x20 | 0x08 | (c[1] & 7));
        if (!d || !d->f || d->read_only) st3 |= 0x40;
        if (!d || d->track == 0) st3 |= 0x10;
        result(f, &st3, 1);
        break;
    }
    case 0x07:                                   /* RECALIBRATE */
    case 0x0f:                                   /* SEEK */
        if (d) {
            uint8_t to = (c[0] & 0x1f) == 0x07 ? 0 : c[2];
            if (d->f && to != d->track) d->changed = 0;   /* a step with a disk in clears the change line */
            if (d->f && (c[0] & 0x1f) == 0x07) d->changed = 0;
            d->track = to;
            f->int_st0[0] = (uint8_t)(0x20 | drv);      /* seek end */
        } else f->int_st0[0] = (uint8_t)(0x70 | drv);  /* seek end, equipment check: no such drive */
        f->int_pending = 1;
        f->phase = P_CMD;
        break;
    case 0x08: {                                 /* SENSE INTERRUPT */
        uint8_t r[2];
        if (f->int_n > 0) {                      /* after a reset, one for each drive number */
            int i = 4 - f->int_n--;
            r[0] = f->int_st0[i];
            fdc_drive *e = drive_of(f, i);
            r[1] = e ? e->track : 0;
            result(f, r, 2);
        } else if (f->int_pending) {
            f->int_pending = 0;
            r[0] = f->int_st0[0];
            fdc_drive *e = drive_of(f, r[0] & 3);
            r[1] = e ? e->track : 0;
            result(f, r, 2);
        } else { r[0] = 0x80; result(f, r, 1); }
        break;
    }
    case 0x02: case 0x06: begin_rw(f, 0); break; /* READ TRACK, READ DATA */
    case 0x05: begin_rw(f, 1); break;            /* WRITE DATA */
    case 0x0a: {                                 /* READ ID */
        f->drv = drv; f->head = c[1] >> 2 & 1;
        if (!d || !d->f) { f->c = f->h = f->r = f->n = 0; rw_result(f, 0x48, 0, 0); break; }
        if (!d->spt) { f->c = d->track; f->h = f->head; f->r = 1; f->n = 2; rw_result(f, 0x40, 0x01, 0); break; }   /* no address mark */
        f->c = d->track; f->h = f->head; f->r = 1; f->n = 2;
        rw_result(f, 0, 0, 0);
        break;
    }
    case 0x0d:                                   /* FORMAT TRACK */
        f->drv = drv; f->head = c[1] >> 2 & 1;
        f->c = d ? d->track : 0; f->h = f->head; f->r = 1; f->n = c[2];
        if (!d || !d->f) { rw_result(f, 0x48, 0, 0); break; }
        if (d->read_only) { rw_result(f, 0x40, 0x02, 0); break; }
        if (c[2] != 2 || (d->spt && c[3] != d->spt)) { rw_result(f, 0x40, 0x04, 0); break; }   /* the image keeps its density */
        f->fmt_left = c[3];
        f->fmt_fill = c[5];
        if (f->fmt_left <= 0) { rw_result(f, 0, 0, 0); break; }
        f->pos = 0; f->len = 4;
        f->phase = P_WRITE;
        break;
    case 0x10: { uint8_t v = 0x90; result(f, &v, 1); break; }   /* VERSION: an 82077 */
    case 0x12: f->perp = c[1]; f->phase = P_CMD; break;
    case 0x13: f->config[0] = c[1]; f->config[1] = c[2]; f->config[2] = c[3]; f->phase = P_CMD; break;
    case 0x14: {                                 /* LOCK (bit 7 of the command: lock or unlock) */
        f->locked = c[0] >> 7 & 1;
        uint8_t v = (uint8_t)(f->locked << 4);
        result(f, &v, 1);
        break;
    }
    case 0x0e: {                                 /* DUMPREG */
        uint8_t r[10] = { f->d[0].track, f->d[1].track, 0, 0, f->specify[0], f->specify[1], (uint8_t)f->eot,
                          (uint8_t)(f->locked << 7 | (f->perp & 0x7f)), f->config[1], f->config[2] };
        result(f, r, 10);
        break;
    }
    default: { uint8_t v = 0x80; result(f, &v, 1); break; }    /* invalid command */
    }
}

uint8_t fdc_read(sql_fdc *f, unsigned port) {
    switch (port & 7) {
    case 2: return f->dor;
    case 4:
        if (!(f->dor & 4)) return 0;             /* held in reset */
        switch (f->phase) {
        case P_READ: return MSR_RQM | MSR_DIO | MSR_NDM | MSR_CB;
        case P_WRITE: return MSR_RQM | MSR_NDM | MSR_CB;
        case P_RESULT: return MSR_RQM | MSR_DIO | MSR_CB;
        default: return (uint8_t)(MSR_RQM | (f->ncmd ? MSR_CB : 0));
        }
    case 5:
        if (f->phase == P_READ) {
            uint8_t v = f->buf[f->pos++];
            if (f->pos >= f->len) { f->r++; read_next(f); }
            return v;
        }
        if (f->phase == P_RESULT) {
            uint8_t v = f->res[f->rpos++];
            if (f->rpos >= f->nres) { f->phase = P_CMD; if ((f->res[0] & 0xc0) != 0xc0 && f->nres == 7) f->int_pending = 0; }
            return v;
        }
        return 0xff;
    case 7: {                                    /* DIR: bit 7 the selected drive's disk change line */
        fdc_drive *d = drive_of(f, f->dor & 3);
        return (uint8_t)(d && (d->changed || !d->f) ? 0x80 : 0x00);
    }
    default: return 0xff;
    }
}

void fdc_write(sql_fdc *f, unsigned port, uint8_t v) {
    switch (port & 7) {
    case 2: {
        int was_reset = !(f->dor & 4);
        f->dor = v;
        if (!(v & 4)) { f->phase = P_CMD; f->ncmd = f->nres = 0; }
        else if (was_reset) fdc_reset(f);        /* leaving reset */
        break;
    }
    case 4:                                      /* DSR: bit 7 a software reset */
        f->dsr = v & 0x7f;
        if (v & 0x80) fdc_reset(f);
        break;
    case 5:
        if (!(f->dor & 4)) return;
        if (f->phase == P_WRITE) {
            f->buf[f->pos++] = v;
            if (f->pos < f->len) return;
            if ((f->cmd[0] & 0x1f) == 0x0d) format_sector(f);
            else write_sector(f);
            return;
        }
        if (f->phase != P_CMD) return;           /* not the time for a byte: the chip ignores it */
        if (f->ncmd == 0) f->want = cmd_len(v);
        f->cmd[f->ncmd++] = v;
        if (f->ncmd >= f->want) { f->ncmd = 0; execute(f); }
        break;
    case 7: f->ccr = v & 3; break;               /* the data rate: images take any */
    default: break;
    }
}

int fdc_irq(const sql_fdc *f) {
    return (f->dor & 8) && (f->int_pending || f->int_n > 0 || f->phase == P_READ || f->phase == P_WRITE);
}
