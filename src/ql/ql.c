/*
 * Sinclair QL board in C: the ZX8301, the ZX8302 and the IPC, on the C AC090
 * core's 68000 model (ql.h has the map).
 *
 * Written from the QL's documented registers (QL Technical Guide; the
 * register notes QDOS and Minerva's sources publish). No other emulator's
 * code is in it.
 *
 * Microdrives (mdv.c) take cartridge images. A selected drive with no
 * cartridge: its motor runs, the gap signal stays on and a gap interrupt
 * comes each 40 ms, so QDOS finds no sector header, says "not found" and
 * stops the drive, as a QL with no cartridge in does. The network is not
 * fitted. ser1's output is kept; nothing comes in on the serial ports yet.
 */
#define _POSIX_C_SOURCE 199309L
#include "ql.h"
#include "../state.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Seconds from 1 Jan 1961 (the QL's epoch) to 1 Jan 1970: nine years, two
 * of them leap years (1964, 1968). */
#define QL_EPOCH_OFFSET 283996800LL

/* What a 68008 adds to each bus access over the 68000's four clocks: it
 * moves a byte at a time, so a word costs two cycles. */
#define QL_WAIT_RAM 4
/* The ZX8301 takes the RAM's time while it fetches the screen. */
#define QL_WAIT_SCREEN 8

static int64_t host_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec;
}

static uint32_t pc_clock(ql *q) {
    return (uint32_t)(host_seconds() + QL_EPOCH_OFFSET + q->clock_offset);
}

/* ---- the IPC ---------------------------------------------------------- */

/* The parameter bits each command takes. */
static uint32_t ipc_param_bits(int cmd) {
    switch (cmd) {
    case 9: case 12: case 13: return 4;     /* keyrow, mdv sensitivity, baud */
    case 10: return 64;                     /* sound */
    case 15: return 8;                      /* test */
    default: return 0;
    }
}

static void ipc_put(ql_ipc *p, uint32_t value, int bits) {
    for (int i = bits - 1; i >= 0 && p->out_n < (int)sizeof p->out; i--) p->out[p->out_n++] = (value >> i) & 1;
}

static int ipc_key_held(const ql_ipc *p) {
    return p->held != 0;
}

static uint8_t ipc_row(const ql_ipc *p, int row) {
    row &= 7;
    uint8_t bits = (uint8_t)(p->held >> ((7 - row) * 8));
    if (row == 7) bits |= (uint8_t)(p->shift | p->ctrl << 1 | p->alt << 2);
    return bits;
}

static void ser_poll(ql *q);

static void ipc_command(ql *q) {
    ql_ipc *p = &q->ipc;
    p->out_n = p->out_pos = 0;
    switch (p->cmd) {
    case 1: {                               /* status */
        ser_poll(q);
        int queued = p->q_head != p->q_tail;
        uint8_t s = 0;
        if (queued || ipc_key_held(p)) s |= 1;
        if (p->sound_on && q->cpu.clock < p->sound_until) s |= 2;
        if (ipc_key_held(p)) s |= 8;
        if (p->sb_n[0]) s |= 0x10;          /* input waiting on ser1 */
        if (p->sb_n[1]) s |= 0x20;          /* ... on ser2 */
        ipc_put(p, s, 8);
        break;
    }
    case 2: case 3:                         /* open ser1, ser2 */
        p->ser_open[p->cmd - 2] = 1;
        break;
    case 4: case 5:                         /* close: what the IPC held is dropped */
        p->ser_open[p->cmd - 4] = 0;
        p->sb_n[p->cmd - 4] = 0;
        break;
    case 6: case 7: {                       /* read ser1, ser2: a count, then the bytes */
        int port = p->cmd - 6, n = p->sb_n[port];
        ipc_put(p, (uint32_t)n, 8);
        for (int i = 0; i < n; i++) ipc_put(p, p->serbuf[port][i], 8);
        p->sb_n[port] = 0;
        break;
    }
    case 13:                                /* baud rate, both ports */
        p->baud = p->param[0] & 7;
        break;
    case 8: {                               /* read keyboard */
        int n = 0;
        uint8_t keys[7], sca[7];
        while (n < 7 && p->q_head != p->q_tail) {
            keys[n] = p->q_key[p->q_tail];
            sca[n] = p->q_sca[p->q_tail];
            p->q_tail = (p->q_tail + 1) % QL_KEYQ;
            n++;
        }
        /* bit 3: the last key read is still down, so QDOS may repeat it */
        int still = n && (p->held >> keys[n - 1] & 1);
        if (!n && ipc_key_held(p)) still = 1;
        ipc_put(p, (uint32_t)(still << 3 | n), 4);
        for (int i = 0; i < n; i++) {
            int lost = p->lost && i == n - 1;
            ipc_put(p, (uint32_t)(lost << 3 | sca[i]), 4);
            ipc_put(p, keys[i], 8);
        }
        if (n) p->lost = 0;
        break;
    }
    case 9:                                 /* keyrow */
        ipc_put(p, ipc_row(p, p->param[0] & 7), 8);
        break;
    case 10: {                              /* sound: the 16-bit values come ls byte first */
        p->pitch1 = p->param[0];
        p->pitch2 = p->param[1];
        p->interval = (uint16_t)(p->param[2] | p->param[3] << 8);
        p->duration = (uint16_t)(p->param[4] | p->param[5] << 8);
        p->sound_on = 1;
        /* duration counts 72 us steps; 0 plays until stopped */
        p->sound_until = p->duration ? q->cpu.clock + (uint64_t)p->duration * QL_CLOCK_HZ * 72 / 1000000 : UINT64_MAX;
        break;
    }
    case 11:                                /* stop sound */
        p->sound_on = 0;
        break;
    case 14:                                /* random number */
        ipc_put(p, (uint32_t)(q->cpu.clock * 2654435761u >> 7) & 0xffff, 16);
        break;
    case 15:                                /* test: the byte comes back */
        ipc_put(p, p->param[0], 8);
        break;
    default:                                /* reset, open/close serial, baud, mdv sensitivity: nothing to return */
        break;
    }
    p->phase = p->out_n ? 2 : 0;
}

/* Serial input: bytes from the host into the IPC's buffer, one per character
 * time at the baud rate, while the port is open. */
static void ser_poll(ql *q) {
    ql_ipc *p = &q->ipc;
    static const uint32_t rates[8] = { 19200, 9600, 4800, 2400, 1200, 600, 300, 75 };
    if (q->cpu.clock < p->ser_next) return;
    int moved = 0;
    for (int port = 0; port < 2; port++) {
        if (!p->ser_open[port] || p->sq_head[port] == p->sq_tail[port] || p->sb_n[port] >= QL_IPC_SERBUF) continue;
        p->serbuf[port][p->sb_n[port]++] = p->serq[port][p->sq_tail[port]];
        p->sq_tail[port] = (p->sq_tail[port] + 1) % QL_SERQ;
        moved = 1;
    }
    if (moved) p->ser_next = q->cpu.clock + (uint64_t)QL_CLOCK_HZ * 11 / rates[p->baud & 7];
}

int ql_serial_space(const ql *q, int port) {
    const ql_ipc *p = &q->ipc;
    if (port < 0 || port > 1) return 0;
    return QL_SERQ - 1 - (p->sq_head[port] - p->sq_tail[port] + QL_SERQ) % QL_SERQ;
}

int ql_serial_in(ql *q, int port, uint8_t ch) {
    ql_ipc *p = &q->ipc;
    if (ql_serial_space(q, port) <= 0) return -1;
    p->serq[port][p->sq_head[port]] = ch;
    p->sq_head[port] = (p->sq_head[port] + 1) % QL_SERQ;
    return 0;
}

/* A write to pc_ipcwr: %11x0 sends bit x, or clocks the next reply bit out. */
static void ipc_write(ql *q, uint8_t v) {
    ql_ipc *p = &q->ipc;
    if ((v & 0x0d) != 0x0c) return;         /* not a link transfer (boot writes %0001) */
    int bit = v >> 1 & 1;
    if (p->phase == 2) {
        p->out_bit = p->out[p->out_pos++];
        if (p->out_pos >= p->out_n) p->phase = 0;
        return;
    }
    p->in_bits = p->in_bits << 1 | (uint32_t)bit;
    p->in_n++;
    if (p->phase == 0) {
        if (p->in_n < 4) return;
        p->cmd = (int)(p->in_bits & 15);
        p->in_bits = p->in_n = 0;
        p->in_want = ipc_param_bits(p->cmd);
        memset(p->param, 0, sizeof p->param);
        if (p->in_want) { p->phase = 1; return; }
        ipc_command(q);
        return;
    }
    if (p->in_n % 8 == 0 || p->in_n == p->in_want) {
        unsigned idx = (p->in_n - 1) / 8;
        if (idx < sizeof p->param) p->param[idx] = (uint8_t)(p->in_bits & 0xff);
        if (p->in_n < p->in_want) p->in_bits = 0;
    }
    if (p->in_n >= p->in_want) {
        p->in_bits = p->in_n = 0;
        ipc_command(q);
    }
}

void ql_modifiers(ql *q, int shift, int ctrl, int alt) {
    q->ipc.shift = !!shift; q->ipc.ctrl = !!ctrl; q->ipc.alt = !!alt;
}

void ql_key(ql *q, int code, int down) {
    ql_ipc *p = &q->ipc;
    if (code < 0 || code > 63) return;
    if (!down) { p->held &= ~(1ull << code); return; }
    if (p->held >> code & 1) return;
    p->held |= 1ull << code;
    int next = (p->q_head + 1) % QL_KEYQ;
    if (next == p->q_tail) { p->lost = 1; return; }
    p->q_key[p->q_head] = (uint8_t)code;
    p->q_sca[p->q_head] = (uint8_t)(p->shift << 2 | p->ctrl << 1 | p->alt);
    p->q_head = next;
}

/* Keyrow codes for text. Index = the code; 0 = no key there. Shifted
 * characters are in the second string at the same place. */
static const char key_plain[64] =
    "\0\0\0xv/n,"  "826qe0tu"  "9wi\0r-yo"  "l3h1apdj"
    "[\0ksf=g;"    "]z.cb`m'"  "\0\0\0\0\0\\ \0" "\0\0005\0\0\00047";
static const char key_shift[64] =
    "\0\0\0XV?N<"  "*@^QE)TU"  "(WI\0R_YO"  "L#H!APDJ"
    "{\0KSF+G:"    "}Z>CB~M\"" "\0\0\0\0\0| \0"  "\0\0%\0\0\0$&";

int ql_key_for_ascii(int ch, int *shift) {
    *shift = 0;
    if (ch == '\n' || ch == '\r') return QLK_ENTER;
    if (ch == '\t') return QLK_TAB;
    if (ch == 27) return QLK_ESC;
    if (ch == ' ') return QLK_SPACE;
    for (int i = 0; i < 64; i++) if (key_plain[i] == ch) return i;
    for (int i = 0; i < 64; i++) if (key_shift[i] == ch) { *shift = 1; return i; }
    return -1;
}

/* ---- the ZX8302 and mc_stat ------------------------------------------- */

static void update_irq(ql *q) {
    uint8_t live = q->intr_pending & (uint8_t)(0x18 | (q->intr_mask >> 5));
    ac090_set_irq(&q->cpu, live ? 2 : 0);
}

static void tx_poll(ql *q) {
    if (q->tx_done && q->cpu.clock >= q->tx_done) {
        q->tx_done = 0;
        q->intr_pending |= 0x04;                /* transmit: ready for the next byte */
        update_irq(q);
    }
}

static uint32_t baud_of(uint8_t tctrl) {
    static const uint32_t rates[8] = { 19200, 9600, 4800, 2400, 1200, 600, 300, 75 };
    return rates[tctrl & 7];
}

static uint8_t io_read8(ql *q, uint32_t a) {
    if (a < 0x18000 || a > 0x1bfff) return 0xff;
    if (a & 0x40) return 0xff;              /* mc_stat is write only */
    switch (a & 0x23) {
    case 0x00: case 0x01: case 0x02: case 0x03:
        return (uint8_t)(pc_clock(q) >> (8 * (3 - (a & 3))));
    case 0x20: {                            /* pc_ipcrd */
        tx_poll(q);
        ql_mdv_advance(q);
        uint8_t v = 0;
        if (ql_mdv_gap(q)) v |= 0x08;       /* gap: no signal passes the head */
        if (q->mdv_rxrd) v |= 0x04;         /* a byte read from the tape */
        if (q->ipc.out_bit) v |= 0x80;      /* bit 6 clear: the IPC has the bit already */
        if ((q->tctrl & 0x18) == 0x10 ? q->mdv_txfl : q->tx_done != 0) v |= 0x02;   /* transmit buffer full */
        update_irq(q);
        return v;
    }
    case 0x21: {                            /* pc_intr */
        tx_poll(q);
        ql_mdv_advance(q);
        update_irq(q);
        uint64_t c = q->cpu.clock;
        uint8_t v = q->intr_pending & 0x1f;
        if (!q->mdv_sel) v |= 0x40;
        if ((c * 65536 / QL_CLOCK_HZ) & 1) v |= 0x20;
        if ((c * baud_of(q->tctrl) * 2 / QL_CLOCK_HZ) & 1) v |= 0x80;
        return v;
    }
    default:                                /* pc_trak1, pc_trak2: the byte read from the tape */
        ql_mdv_advance(q);
        q->mdv_rxrd = 0;
        return q->mdv_rx;
    }
}

static void io_write8(ql *q, uint32_t a, uint8_t v) {
    if (a < 0x18000 || a > 0x1bfff) return;
    if (a & 0x40) { q->mc_stat = v; return; }
    switch (a & 0x23) {
    case 0x00:                              /* reset the clock */
        q->clock_offset = -(host_seconds() + QL_EPOCH_OFFSET);
        break;
    case 0x01: {                            /* step the clock: a 0 in bit 4..1 adds one to byte 3..0 */
        static const int shift[4] = { 24, 16, 8, 0 };
        for (int b = 0; b < 4; b++) if (!(v >> (4 - b) & 1)) q->clock_offset += 1ll << shift[b];
        break;
    }
    case 0x02:
        q->tctrl = v;
        break;
    case 0x03:
        ipc_write(q, v);
        break;
    case 0x20: {                            /* pc_mctrl: the select chain clocks on sclk falling */
        ql_mdv_advance(q);
        if ((q->mctrl & 2) && !(v & 2)) q->mdv_sel = (uint8_t)(q->mdv_sel << 1 | (v & 1));
        q->mctrl = v;
        ql_mdv_select(q);
        ql_mdv_control(q, v);
        break;
    }
    case 0x21: {                            /* clear interrupts, set masks */
        ql_mdv_advance(q);
        int gap_on = (v & 0x20) && !(q->intr_mask & 0x20);
        q->intr_pending &= (uint8_t)~(v & 0x1f);
        q->intr_mask = v & 0xe0;
        /* the gap interrupt enabled while a cartridge's gap passes: it comes now */
        if (gap_on && q->mdv_run >= 0 && q->mdv[q->mdv_run].tape && ql_mdv_gap(q)) q->intr_pending |= 0x01;
        update_irq(q);
        break;
    }
    case 0x22:                              /* pc_tdata */
        if ((q->tctrl & 0x18) == 0x10) {    /* microdrive: the next byte to write */
            ql_mdv_advance(q);
            q->mdv_tx = v;
            q->mdv_txfl = 1;
        } else if (!(q->tctrl & 0x10)) {    /* serial: ser1 or ser2 by bit 3 */
            int port = q->tctrl >> 3 & 1;
            if (q->ser_out) q->ser_out(q->ser_opaque, port, v);
            else if (!port && q->serial) { fputc(v, q->serial); fflush(q->serial); }
            /* start bit, 8 data bits, 2 stop bits */
            q->tx_done = q->cpu.clock + (uint64_t)QL_CLOCK_HZ * 11 / baud_of(q->tctrl);
        }
        break;
    case 0x23:                              /* older QLs: mc_stat here too */
        q->mc_stat = v;
        break;
    }
}

static uint32_t bus_read(void *opaque, uint32_t a, int size) {
    ql *q = opaque;
    uint32_t v = 0;
    for (int i = 0; i < size; i++) v = v << 8 | io_read8(q, (a + (uint32_t)i) & 0xfffff);
    return v;
}

static void bus_write(void *opaque, uint32_t a, uint32_t value, int size) {
    ql *q = opaque;
    for (int i = 0; i < size; i++) io_write8(q, (a + (uint32_t)i) & 0xfffff, (uint8_t)(value >> (8 * (size - 1 - i))));
}

/* ---- the board -------------------------------------------------------- */

static long load(const char *path, uint8_t *dst, long max) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "ql: cannot open %s\n", path); return -1; }
    long n = (long)fread(dst, 1, (size_t)max, f);
    int more = fgetc(f) != EOF;
    fclose(f);
    if (more) { fprintf(stderr, "ql: %s is larger than %ld bytes\n", path, max); return -1; }
    return n;
}

int ql_init(ql *q, const char *rom_path, const char *cart_path, uint32_t ram_kib) {
    memset(q, 0, sizeof *q);
    memset(q->rom, 0xff, sizeof q->rom);
    memset(q->open, 0xff, sizeof q->open);
    if (load(rom_path, q->rom, 0xc000) < 0) return -1;
    if (cart_path && load(cart_path, q->rom + 0xc000, 0x4000) < 0) return -1;
    if (ram_kib < 128) ram_kib = 128;
    if (ram_kib > QL_MAX_RAM >> 10) ram_kib = QL_MAX_RAM >> 10;
    q->ram_bytes = ram_kib << 10;
    q->ram = calloc(1, QL_MAX_RAM);
    if (!q->ram) return -1;
    q->serial = stdout;

    ac090 *c = &q->cpu;
    ac090_init(c, AC090_68000);
    c->addr_mask = 0x000fffff;              /* 20 address lines */
    c->bus.opaque = q;
    c->bus.read = bus_read;
    c->bus.write = bus_write;
    /* Page 0: the ROM and the ROM port, read only. Page 1 (from $10000):
     * the I/O, through the bus functions. From $20000: RAM as fitted, then
     * open bus. The map repeats above 1 MB, as the CPU masks addresses. */
    for (uint32_t pg = 0; pg < 16; pg++) {
        if (pg == 0) { c->bus.rpage[pg] = q->rom; c->bus.wpage[pg] = NULL; }
        else if (pg == 1) { c->bus.rpage[pg] = NULL; c->bus.wpage[pg] = NULL; }
        else if ((pg << 16) - QL_RAM_BASE < q->ram_bytes) {
            c->bus.rpage[pg] = c->bus.wpage[pg] = q->ram + ((pg << 16) - QL_RAM_BASE);
        } else { c->bus.rpage[pg] = q->open; c->bus.wpage[pg] = NULL; }
        c->bus.wait[pg] = pg == 2 ? QL_WAIT_SCREEN : QL_WAIT_RAM;
    }
    ac090_reset(c);
    q->next_frame = QL_FRAME_CLOCKS;
    q->mdv_run = -1;
    return 0;
}

void ql_reset(ql *q, int clear_ram) {
    if (clear_ram) memset(q->ram, 0, QL_MAX_RAM);
    q->intr_pending = q->intr_mask = q->tctrl = q->mctrl = q->mdv_sel = q->mc_stat = 0;
    q->tx_done = 0;
    ql_mdv_select(q);                       /* the drives stop; their cartridges stay in */
    q->mdv_rxrd = q->mdv_txfl = q->mdv_sync = 0;
    uint64_t held = q->ipc.held;
    int s = q->ipc.shift, c = q->ipc.ctrl, a = q->ipc.alt;
    memset(&q->ipc, 0, sizeof q->ipc);
    q->ipc.held = held; q->ipc.shift = s; q->ipc.ctrl = c; q->ipc.alt = a;   /* keys still down stay down */
    ac090_set_irq(&q->cpu, 0);
    ac090_reset(&q->cpu);
    q->next_frame = q->cpu.clock + QL_FRAME_CLOCKS;
}

/* ---- saved states ------------------------------------------------------ */

int ql_save_state(ql *q, const char *path, const char **why) {
    FILE *f = fopen(path, "wb");
    if (!f) { *why = "cannot write the file"; return -1; }
    int ok = state_write_header(f, 1, sizeof *q, q->ram_bytes, state_hash(q->rom, sizeof q->rom)) == 0 &&
             fwrite(q, sizeof *q, 1, f) == 1 && fwrite(q->ram, 1, q->ram_bytes, f) == q->ram_bytes;
    if (fclose(f)) ok = 0;
    if (!ok) { *why = "cannot write the file"; remove(path); return -1; }
    return 0;
}

int ql_load_state(ql *q, const char *path, const char **why) {
    FILE *f = fopen(path, "rb");
    if (!f) { *why = "no saved state"; return -1; }
    ql *t = malloc(sizeof *t);
    uint8_t *ram = malloc(q->ram_bytes);
    int ok = t && ram && state_check_header(f, 1, sizeof *q, q->ram_bytes, state_hash(q->rom, sizeof q->rom), why) == 0;
    if (ok && (fread(t, sizeof *t, 1, f) != 1 || fread(ram, 1, q->ram_bytes, f) != q->ram_bytes)) { ok = 0; *why = "the file is cut short"; }
    fclose(f);
    if (ok) {
        memcpy(q->ram, ram, q->ram_bytes);
        state_cpu_take(&q->cpu, &t->cpu);
        /* the chips' registers; the host's ends and the cartridges stay */
        q->intr_pending = t->intr_pending; q->intr_mask = t->intr_mask; q->tctrl = t->tctrl;
        q->mctrl = t->mctrl; q->mdv_sel = t->mdv_sel; q->clock_offset = t->clock_offset; q->tx_done = t->tx_done;
        q->mc_stat = t->mc_stat; q->next_frame = t->next_frame; q->frames = t->frames; q->ipc = t->ipc;
        q->mdv_rxrd = q->mdv_txfl = q->mdv_sync = 0;
        q->mdv_run = -1;
        ql_mdv_select(q);
        ac090_set_irq(&q->cpu, q->cpu.irq);
    }
    free(t); free(ram);
    return ok ? 0 : -1;
}

void ql_free(ql *q) {
    if (q->dos_free) q->dos_free(q);
    for (int i = 0; i < QL_MDV_DRIVES; i++) ql_mdv_eject(q, i);
    free(q->ram);
    q->ram = NULL;
}

uint64_t ql_run(ql *q, uint64_t until) {
    ac090 *c = &q->cpu;
    uint64_t start = c->instructions;
    while (c->clock < until && !c->halted && !c->dbg_stop) {   /* (dbg_stop: the debugger stopped the CPU) */
        uint64_t next = q->next_frame;
        if (q->tx_done && q->tx_done < next) next = q->tx_done;
        uint64_t tape = ql_mdv_next(q);
        if (tape < next) next = tape;
        if (until < next) next = until;
        update_irq(q);
        if (next > c->clock) ac090_run(c, next - c->clock);
        tx_poll(q);
        ql_mdv_advance(q);
        while (c->clock >= q->next_frame) {
            q->next_frame += QL_FRAME_CLOCKS;
            q->frames++;
            q->intr_pending |= 0x08;        /* the frame interrupt */
            if (q->mdv_run >= 0 && !q->mdv[q->mdv_run].tape && !(q->frames & 1)) q->intr_pending |= 0x01;   /* an empty drive's gap */
            update_irq(q);
        }
        if (q->ipc.sound_on && c->clock >= q->ipc.sound_until) q->ipc.sound_on = 0;
    }
    return c->instructions - start;
}

int ql_mode8(const ql *q) {
    return q->mc_stat >> 3 & 1;
}

void ql_render(ql *q, uint32_t *rgb) {
    static const uint32_t pal[8] = {
        0x000000, 0x0000ff, 0xff0000, 0xff00ff, 0x00ff00, 0x00ffff, 0xffff00, 0xffffff
    };
    if (q->mc_stat & 0x02) { memset(rgb, 0, sizeof(uint32_t) * QL_SCREEN_W * QL_SCREEN_H); return; }
    const uint8_t *scr = q->ram + ((q->mc_stat & 0x80) ? 0x8000 : 0);
    int flash_off = (q->frames / 32) & 1;   /* the ZX8301 flashes at about 0.8 Hz */
    for (int y = 0; y < QL_SCREEN_H; y++) {
        const uint8_t *line = scr + y * 128;
        uint32_t *out = rgb + y * QL_SCREEN_W;
        if (!ql_mode8(q)) {
            /* mode 4: each word is 8 pixels, green in the even byte, red in the odd */
            for (int w = 0; w < 64; w++) {
                uint8_t g = line[w * 2], r = line[w * 2 + 1];
                for (int b = 0; b < 8; b++) {
                    int gb = g >> (7 - b) & 1, rb = r >> (7 - b) & 1;
                    out[w * 8 + b] = (gb ? 0x00ff00 : 0) | (rb ? 0xff0000 : 0) | (gb && rb ? 0x0000ff : 0);
                }
            }
        } else {
            /* mode 8: each word is 4 pixels, G F in the even byte, R B in the odd.
             * A set F starts or ends flashing; flashing shows the colour in
             * force where it began as background. */
            int flashing = 0;
            uint32_t bg = 0;
            for (int w = 0; w < 64; w++) {
                uint8_t e = line[w * 2], o = line[w * 2 + 1];
                for (int b = 0; b < 4; b++) {
                    int g = e >> (7 - 2 * b) & 1, f = e >> (6 - 2 * b) & 1;
                    int r = o >> (7 - 2 * b) & 1, bl = o >> (6 - 2 * b) & 1;
                    uint32_t col = pal[g << 2 | r << 1 | bl];
                    if (f) { if (!flashing) bg = col; flashing = !flashing; }
                    uint32_t show = flashing && flash_off ? bg : col;
                    out[w * 8 + b * 2] = out[w * 8 + b * 2 + 1] = show;
                }
            }
        }
    }
}
