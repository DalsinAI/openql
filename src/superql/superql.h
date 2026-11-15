/*
 * The Super QL: a Q40/Q60-compatible board in C, for the C AC090 core on
 * its 68040 model (Dale, 20 Oct 2026: "lets do a super ql, as much memory
 * as any of the machines hobbyists ever built").
 *
 * Peter Graf's Q60 took 128 MB, the most any QL-family machine has had; one
 * Q60 board was fitted with a 68040 (the "Q40i"). This is that machine: the
 * Q40/Q60 map SMSQ/E's own Q40 build expects (keys/q40 and keys/q40_multiIO
 * in the SMSQ/E sources), a 68040 and up to 128 MB. The Firmware/ROM is
 * SMSQ/E, fetched on request by openql-fetch-roms and never shipped.
 *
 * Map:
 *   $00000000-      RAM (128 MB at most). While "low ROM" is set (at reset,
 *                   and after a write to $FF010000) reads below $18000 come
 *                   from the ROM and writes go to RAM; after a write to
 *                   $FF018000 they come from RAM, which is write protected
 *                   below $18000 (the QL's ROM area).
 *   $00020000       the QL screen in QL modes 4 and 8 (512 x 256)
 *   $FE000000       ROM (SMSQ/E), up to 8 MB here
 *   $FE800000       video RAM: 16-bit modes, 512 x 256 or 1024 x 512
 *   $FF000000       the master chip: interrupts, keyboard, display mode
 *   $FF008000       the two sound DACs
 *   $FF020000       the clock's NVRAM, one byte every 4; the clock at $FF021FE0
 *   $FF400000       the ISA I/O of the multi-I/O card, port << 2 (+1 for bytes):
 *                   COM1-COM4, LPT1, the floppy controller, IDE
 */
#ifndef SUPERQL_H
#define SUPERQL_H

#include <stdint.h>
#include <stdio.h>
#include <time.h>
#ifdef _WIN32
#define localtime_r(t, tm) localtime_s(tm, t)   /* Windows has the same with its arguments the other way round */
#endif
#include "../cpu/ac090.h"
#include "fdc.h"

#define SQL_CLOCK_HZ      40000000u              /* the Q40's 68040 at 40 MHz */
#define SQL_FRAME_CLOCKS  (SQL_CLOCK_HZ / 50)    /* the master chip's 50 Hz interrupt */
#define SQL_MAX_RAM_MB    128u
#define SQL_ROM_BASE      0xFE000000u
#define SQL_ROM_BYTES     0x800000u
#define SQL_VRAM_BASE     0xFE800000u
#define SQL_VRAM_BYTES    0x100000u
#define SQL_LOW_ROM_END   0x18000u
#define SQL_QL_SCREEN     0x20000u
#define SQL_OUT_W         1024                   /* the picture the runtime shows: QL modes doubled */
#define SQL_OUT_H         512
#define SQL_KEYQ          64
#define SQL_SECTOR        512

/* master chip interrupt register bits (keys/q40) */
enum { SQL_IR_KBD = 1 << 1, SQL_IR_SER = 1 << 2, SQL_IR_FRAME = 1 << 3, SQL_IR_EINT = 1 << 4 };
/* display modes (q40_dmode) */
enum { SQL_D_QL8 = 0, SQL_D_QL4 = 1, SQL_D_SMALL = 2, SQL_D_LARGE = 3 };

#define SQL_RXQ           64
#define SQL_AUDIOQ        8192                   /* stereo samples the host has not taken yet */

typedef struct sql_uart {             /* a 16550, as far as SMSQ/E's serial driver looks */
    uint8_t ier, lcr, mcr, scr, dll, dlm, fcr;
    uint8_t rxq[SQL_RXQ];             /* bytes coming in, not yet read */
    int rx_head, rx_tail;
} sql_uart;

typedef struct sql_ide {              /* one ATA disk, PIO, on the primary channel */
    FILE *f;                          /* the image: a QXL.WIN file is a whole QL hard disk */
    uint32_t sectors;
    uint16_t cyls, heads, spt;
    uint8_t err, scnt, sect, cyll, cylh, head, status, ctl;
    uint8_t buf[SQL_SECTOR];
    int pos, left, writing;           /* the sector in buf: bytes moved, sectors still to go */
    uint32_t lba;
    int read_only;
    int geometry;                     /* the QLWA header names no geometry: the disk's is shown in sector 0 */
} sql_ide;

typedef struct superql {
    ac090 cpu;
    uint8_t *ram;                     /* SQL_MAX_RAM_MB MB; ram_bytes of it fitted */
    uint32_t ram_bytes;
    uint8_t *rom;                     /* SQL_ROM_BYTES, $FF past the image */
    uint32_t rom_len;
    uint8_t *vram;
    uint8_t open[0x10000];            /* reads where nothing is */
    int low_rom;                      /* reads below $18000 from the ROM */
    /* master chip */
    uint8_t ir, eir, kie, sie, eie, uie, dmode, srate, led;
    uint8_t keyq[SQL_KEYQ];           /* PC set 2 scan codes not yet read */
    int kq_head, kq_tail;
    uint64_t next_frame, frames;
    /* the clock's NVRAM (MK48T08 style); the time is the host's plus an offset */
    uint8_t nvram[0x800];
    int64_t clock_offset;
    uint8_t rtc_ctl;
    /* multi-I/O */
    sql_uart com[4];
    uint8_t lpt_data, lpt_ctl;
    sql_ide ide;
    sql_fdc fdc;                      /* FLP1_ and FLP2_ */
    /* sound: two 8-bit DACs, fed by SMSQ/E at each sample interrupt (level 6,
     * 20 or 40 kHz, on while the sample enable is set) */
    uint8_t dac[2];
    int sample_pending;
    uint64_t next_sample;
    int16_t audioq[SQL_AUDIOQ][2];
    int aq_head, aq_tail;
    /* the mouse: a Microsoft serial mouse on a COM port (SMSQ/E's driver finds it
     * by the 'M' it sends when DTR and RTS come on), -1 for none */
    int mouse_port;
    int mouse_dx, mouse_dy;           /* movement not yet sent */
    uint8_t mouse_buttons, mouse_sent;   /* bit 0 left, bit 1 right; what the last packet said */
    void (*ser_out)(void *opaque, int port, uint8_t ch);
    void (*par_out)(void *opaque, uint8_t ch);
    void *opaque;
} superql;

int sql_init(superql *s, const char *rom_path, uint32_t ram_mb);
int sql_attach_disk(superql *s, const char *path, int read_only);   /* WIN1: an IDE disk on the primary channel */
void sql_free(superql *s);
void sql_reset(superql *s, int clear_ram);
uint64_t sql_run(superql *s, uint64_t until);

/* The floppy drives (0 FLP1_, 1 FLP2_): a raw image of a QL floppy, 80
 * tracks, two sides, 9, 18 or 36 sectors of 512 a track. */
int sql_insert_floppy(superql *s, int drive, const char *path, int read_only);
void sql_eject_floppy(superql *s, int drive);

/* Sound: the DACs' output at the sample rate (sql_audio_rate: 20000 or
 * 40000 a second), stereo, as signed 16-bit. Returns the samples taken. */
unsigned sql_audio_rate(const superql *s);
int sql_audio_read(superql *s, int16_t *out, int max);

/* The keyboard: PC set 2 scan codes, as the Q40's keyboard port gives them
 * (SMSQ/E's ibm_at102cvt reads them). sql_key_raw puts one byte in. */
void sql_key_raw(superql *s, uint8_t code);
/* An Amiga raw key (bit 7: released) as the set 2 bytes a PC keyboard sends. */
void sql_key_amiga(superql *s, uint8_t raw);

/* The mouse: movement in mouse counts (y down) and the buttons (bit 0 left,
 * bit 1 right). Sent as Microsoft mouse packets on mouse_port, one a frame. */
void sql_mouse_move(superql *s, int dx, int dy);
void sql_mouse_buttons(superql *s, uint8_t buttons);
/* A byte coming in on a COM port (0-3). */
void sql_serial_in(superql *s, int port, uint8_t ch);

/* The picture now, SQL_OUT_W x SQL_OUT_H, 0x00RRGGBB: QL modes are doubled,
 * the small 16-bit mode is doubled, the large one is as it is. */
void sql_render(superql *s, uint32_t *rgb);

#endif
