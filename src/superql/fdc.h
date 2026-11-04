/*
 * The multi-I/O card's floppy controller: an 82077-style FDC at ISA $3F0,
 * worked without DMA (the Q40 has none), so the CPU moves each byte through
 * the FIFO at $3F5 while the main status register says so. Two drives,
 * FLP1_ and FLP2_, each a raw image of a QL floppy: DD (720 KB, 9 sectors a
 * track), HD (1.44 MB, 18) or ED (2.88 MB, 36), 80 tracks, two sides,
 * 512-byte sectors, as QL floppy images are kept.
 *
 * Commands: SPECIFY, SENSE DRIVE STATUS, RECALIBRATE, SENSE INTERRUPT, SEEK,
 * READ DATA, WRITE DATA, READ ID, FORMAT TRACK, VERSION, CONFIGURE, LOCK,
 * DUMPREG, PERPENDICULAR MODE. A transfer runs to the end of the track (EOT)
 * and ends as the chip does without TC: ST0 abnormal end, ST1 end of
 * cylinder. Written from Intel's 82077AA data sheet.
 */
#ifndef SQL_FDC_H
#define SQL_FDC_H

#include <stdint.h>
#include <stdio.h>

#define FDC_DRIVES 2

typedef struct fdc_drive {
    FILE *f;
    int read_only;
    int spt;                 /* 9, 18 or 36; 0: no disk */
    uint8_t track;           /* where the head is */
    int changed;             /* DIR bit 7: a disk went in or out since the last step */
} fdc_drive;

typedef struct sql_fdc {
    fdc_drive d[FDC_DRIVES];
    uint8_t dor, dsr, ccr, specify[2], config[3], locked, perp;
    uint8_t cmd[16];
    int ncmd, want;          /* command bytes in, and how many the command takes */
    uint8_t res[16];
    int nres, rpos;          /* the result phase's bytes */
    int phase;               /* 0 command, 1 execution (read), 2 execution (write), 3 result */
    uint8_t buf[512];
    int pos, len;            /* the sector being moved */
    int drv, head, c, h, r, n, eot;  /* the transfer */
    int fmt_left;            /* FORMAT: sectors still to be described */
    uint8_t fmt_fill;
    int int_pending;         /* SENSE INTERRUPT has a status to give */
    uint8_t int_st0[4];
    int int_n;
} sql_fdc;

void fdc_reset(sql_fdc *f);
int fdc_insert(sql_fdc *f, int drive, const char *path, int read_only);   /* 0, or -1 with a message */
void fdc_eject(sql_fdc *f, int drive);
uint8_t fdc_read(sql_fdc *f, unsigned port);     /* port & 7 */
void fdc_write(sql_fdc *f, unsigned port, uint8_t v);
int fdc_irq(const sql_fdc *f);                   /* the IRQ 6 line (DOR's DMA/IRQ gate on) */

#endif
