/*
 * Sinclair QL board in C, for the C AC090 core on its 68000 model.
 *
 * A PAL QL: a 68008 at 7.5 MHz, the ZX8301 display chip, the ZX8302
 * peripheral chip and the 8049 IPC behind it (keyboard, sound, the serial
 * ports' input). The Firmware/ROM is the user's or one openql-fetch-roms fetches on
 * request (Minerva, SMSQ/E); nothing of it is in this repository.
 *
 * Map (the 68008 has 20 address lines, so everything repeats every 1 MB):
 *   $00000-$0BFFF  ROM, 48 KB
 *   $0C000-$0FFFF  ROM port (a cartridge, or empty)
 *   $18000-$1BFFF  ZX8302 and the ZX8301's control register (mc_stat)
 *   $20000-$3FFFF  128 KB RAM; the screen is $20000 (scr0) or $28000 (scr1)
 *   $40000-        expansion RAM, as an expansion card adds it (up to $FFFFF)
 */
#ifndef QL_H
#define QL_H

#include <stdint.h>
#include <stdio.h>
#include "../cpu/ac090.h"

#define QL_CLOCK_HZ     7500000u        /* the 68008's crystal */
#define QL_FRAME_CLOCKS (QL_CLOCK_HZ / 50)   /* PAL: one frame interrupt each 150,000 clocks */
#define QL_ROM_BYTES    0x10000u        /* ROM and the ROM port: page 0 */
#define QL_RAM_BASE     0x20000u
#define QL_MAX_RAM      (0x100000u - QL_RAM_BASE)   /* 896 KB: the whole map above the I/O */
#define QL_SCREEN_W     512
#define QL_SCREEN_H     256
#define QL_KEYQ         16
#define QL_SERQ         4096            /* bytes on their way in to a serial port, not yet in the IPC */
#define QL_IPC_SERBUF   24              /* the IPC's own buffer for each port (QDOS reads it whole) */
#define QL_MDV_DRIVES   8               /* mdv1_ to mdv8_: two in the QL, six more on its expansion port */
#define QL_MDV_GAP      0x100           /* a tape cell with no signal */

/* A microdrive cartridge: the tape loop as cells, one byte time (40 us,
 * 300 clocks) each, holding the byte recorded there or QL_MDV_GAP. The tape
 * moves only while its drive is selected. mdv.c has the model. */
typedef struct ql_mdv {
    uint16_t *tape;             /* NULL: no cartridge */
    uint32_t len, pos;          /* cells in the loop; the cell under the head */
    uint64_t at;                /* the clock at which the cell at pos came under the head */
    int was_gap;                /* the cell before pos was a gap: a gap's start is an edge */
    int read_only;              /* the write protect tab is out */
    int dirty;                  /* written since the image was last saved */
    char *path;                 /* the image file (QLAY / MiSTer format: 255 sectors of 686 bytes) */
} ql_mdv;

typedef struct ql_ipc {
    /* The link: commands and data come in a bit at a time, msb first, and
     * the reply goes back a bit at a time as the CPU clocks it out. */
    int phase;                  /* 0 command nibble, 1 parameters, 2 reply */
    int cmd;
    uint32_t in_bits, in_n, in_want;
    uint8_t param[8];           /* the parameters as bytes, as they arrived */
    uint8_t out[8 + 8 * QL_IPC_SERBUF];   /* the reply, one bit per byte */
    int out_n, out_pos;
    int out_bit;                /* the bit pc_ipcrd shows in bit 7 */
    /* Keyboard: what is held now (64 keys by keyrow code, the IPC's
     * col << 3 | row), and the key presses not yet read, each with the
     * SHIFT/CTRL/ALT it was pressed with. */
    uint64_t held;
    int shift, ctrl, alt;
    uint8_t q_key[QL_KEYQ], q_sca[QL_KEYQ];
    int q_head, q_tail;
    int lost;                   /* the queue overflowed: the next read says so */
    /* Serial input: ser1 and ser2. Bytes from the host wait in serq and
     * come into the IPC's buffer at the baud rate while the port is open;
     * QDOS reads them with commands 6 and 7. */
    uint8_t serq[2][QL_SERQ];
    int sq_head[2], sq_tail[2];
    uint8_t serbuf[2][QL_IPC_SERBUF];
    int sb_n[2];
    int ser_open[2];
    uint8_t baud;               /* command 13's rate, 0 = 19200 .. 7 = 75 */
    uint64_t ser_next;          /* the clock at which the next byte may come in */
    /* Sound (BEEP): the last request, kept for the runtime to play. */
    int sound_on;
    uint64_t sound_until;
    uint8_t pitch1, pitch2;
    uint16_t interval, duration;
} ql_ipc;

typedef struct ql {
    ac090 cpu;
    uint8_t rom[QL_ROM_BYTES];
    uint8_t *ram;               /* QL_MAX_RAM bytes, from $20000 */
    uint32_t ram_bytes;         /* how much is fitted: 128 KB on a stock QL */
    uint8_t open[0x10000];      /* reads of empty space */
    /* ZX8302 */
    uint8_t intr_pending;       /* pc_intr bits 4..0 */
    uint8_t intr_mask;          /* bits 7..5 as last written */
    uint8_t tctrl;              /* pc_tctrl */
    uint8_t mctrl;              /* pc_mctrl as last written */
    uint8_t mdv_sel;            /* the select chain: bit n set = mdv(n+1) selected */
    ql_mdv mdv[QL_MDV_DRIVES];
    int mdv_run;                /* the drive whose tape moves (0-7), -1 none */
    uint8_t mdv_rx;             /* the byte read last */
    int mdv_rxrd;               /* pc..rxrd: a byte is ready in pc_trak1/2 */
    int mdv_sync, mdv_zeros, mdv_ffs;   /* the PLL: hunting for a preamble's zeros and two $FFs */
    uint8_t mdv_tx;             /* the byte waiting to be written */
    int mdv_txfl;               /* pc..txfl in microdrive mode */
    int64_t clock_offset;       /* pc_clock = host time + this */
    uint64_t tx_done;           /* the clock at which the byte in pc_tdata has gone (0: none) */
    FILE *serial;               /* ser1's output, unless ser_out takes it */
    void (*ser_out)(void *opaque, int port, uint8_t ch);
    void *ser_opaque;
    /* ZX8301 */
    uint8_t mc_stat;
    uint64_t next_frame;
    uint64_t frames;
    ql_ipc ipc;
    int ntsc;
} ql;

int ql_init(ql *q, const char *rom_path, const char *cart_path, uint32_t ram_kib);
void ql_free(ql *q);
void ql_reset(ql *q, int clear_ram);      /* the reset button: RAM kept unless clear_ram (a power cycle) */
uint64_t ql_run(ql *q, uint64_t until);   /* until the CPU clock reaches until; returns instructions run */

/* Microdrives (drive 0 = mdv1_). An image is 255 sectors of 686 bytes, as
 * QLAY and the MiSTer QL core keep them; a missing or empty file is a blank
 * cartridge to FORMAT, saved when the drive stops after a write, on eject
 * and at ql_free. A read-only file is a cartridge with its tab out. */
/* Saved states (src/state.h): 0, or -1 with the reason in *why. */
int ql_save_state(ql *q, const char *path, const char **why);
int ql_load_state(ql *q, const char *path, const char **why);

int ql_mdv_insert(ql *q, int drive, const char *path, int read_only);
void ql_mdv_eject(ql *q, int drive);
int ql_mdv_save(ql *q, int drive);
/* The image <-> tape conversions, for the cases: from_image builds a tape
 * (malloc'd, *len cells) from n bytes of image; to_image writes the sectors
 * found on the tape into out (QL_MDV_IMAGE bytes) and returns how many. */
#define QL_MDV_SECTORS  255
#define QL_MDV_SECTOR   686
#define QL_MDV_IMAGE    (QL_MDV_SECTORS * QL_MDV_SECTOR)
uint16_t *ql_mdv_from_image(const uint8_t *img, long n, uint32_t *len);
int ql_mdv_to_image(const uint16_t *tape, uint32_t len, uint8_t *out);
/* Internal to the board (ql.c, mdv.c). */
void ql_mdv_advance(ql *q);
uint64_t ql_mdv_next(ql *q);
void ql_mdv_select(ql *q);
void ql_mdv_control(ql *q, uint8_t v);
int ql_mdv_gap(ql *q);

/* Keys by the IPC's keyrow code (0..63). The codes below are the ones the
 * QL's keyboard has; ql_key_for_ascii maps text onto them. */
void ql_key(ql *q, int code, int down);
void ql_modifiers(ql *q, int shift, int ctrl, int alt);
int ql_key_for_ascii(int ch, int *shift);   /* -1 if the QL has no key for it */

/* A byte coming in on ser1 (port 0) or ser2 (1). Returns 0, or -1 when the
 * port's queue is full (the host should hold the byte back). */
int ql_serial_in(ql *q, int port, uint8_t ch);
int ql_serial_space(const ql *q, int port);
enum {
    QLK_ENTER = 48, QLK_LEFT = 49, QLK_UP = 50, QLK_ESC = 51, QLK_RIGHT = 52,
    QLK_SPACE = 54, QLK_DOWN = 55, QLK_F1 = 57, QLK_F2 = 59, QLK_F3 = 60,
    QLK_F4 = 56, QLK_F5 = 61, QLK_TAB = 19, QLK_CAPS = 33
};

/* The picture the ZX8301 shows now: 512 x 256, 0x00RRGGBB. In mode 8 each
 * pixel is two wide. Flashing pixels show as they are in the frame drawn. */
void ql_render(ql *q, uint32_t *rgb);
int ql_mode8(const ql *q);

#endif
