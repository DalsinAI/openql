/*
 * The Super QL board (superql.h): RAM, the low ROM switch, the Q40 master
 * chip, the clock, the multi-I/O card's UARTs, printer port and IDE, and the
 * picture in each display mode.
 */
#include "superql.h"
#include "../state.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Disk offsets past 2 GB: long is 32 bits on Windows. */
#ifdef _WIN32
#define disk_seek(f, o) _fseeki64(f, (__int64)(o), SEEK_SET)
#define disk_size(f) (_fseeki64(f, 0, SEEK_END) ? -1 : (int64_t)_ftelli64(f))
#else
#define disk_seek(f, o) fseeko(f, (off_t)(o), SEEK_SET)
#define disk_size(f) (fseeko(f, 0, SEEK_END) ? -1 : (int64_t)ftello(f))
#endif

#define MASTER_PAGE 0xFF00u
#define LOWSW_PAGE  0xFF01u
#define RTC_PAGE    0xFF02u
#define ISA_BASE    0xFF400000u
#define RTC_REGS    0x7F8u          /* the clock's eight registers: the NVRAM's last eight bytes */

/* ---- interrupts ------------------------------------------------------- */

static int uart_pending(const sql_uart *c) {      /* received data, its interrupt enabled, OUT2 on */
    return c->rx_head != c->rx_tail && (c->ier & 1) && (c->mcr & 8);
}

/* The external interrupt register shows the multi-I/O card's IRQ lines: COM2
 * and COM4 on IRQ 3, COM1 and COM3 on IRQ 4, the floppy controller on IRQ 6
 * (with DOR's DMA/IRQ gate on), the IDE disk on IRQ 14 (unless nIEN). Bit 0
 * of the enable register lets them interrupt, on level 2. SMSQ/E's own
 * drivers poll, so it leaves them off; a driver of one's own can use them. */
static void update_irq(superql *s) {
    int ser = 0;
    for (int u = 0; u < 4; u++) ser |= uart_pending(&s->com[u]);
    if (ser) s->ir |= SQL_IR_SER; else s->ir &= (uint8_t)~SQL_IR_SER;
    if (s->kq_head != s->kq_tail) s->ir |= SQL_IR_KBD; else s->ir &= (uint8_t)~SQL_IR_KBD;
    s->eir = (uint8_t)((uart_pending(&s->com[1]) || uart_pending(&s->com[3]) ? SQL_EIR_IRQ3 : 0) |
                       (uart_pending(&s->com[0]) || uart_pending(&s->com[2]) ? SQL_EIR_IRQ4 : 0) |
                       (fdc_irq(&s->fdc) ? SQL_EIR_FDC : 0) |
                       (s->ide.f && s->ide.intrq && !(s->ide.ctl & 2) ? SQL_EIR_IDE : 0));
    if (s->eir && (s->eie & 1)) s->ir |= SQL_IR_EINT; else s->ir &= (uint8_t)~SQL_IR_EINT;
    int on = ((s->ir & SQL_IR_KBD) && s->kie) || ((s->ir & SQL_IR_SER) && s->sie) ||
             (s->ir & SQL_IR_FRAME) || (s->ir & SQL_IR_EINT);
    /* the Q40 raises level 2 for all of them, and level 6 for the sample interrupt */
    ac090_set_irq(&s->cpu, s->uie && s->sample_pending ? 6 : on ? 2 : 0);
}

/* ---- the low ROM switch ------------------------------------------------ */

static void set_low(superql *s, int low_rom) {
    ac090_bus *b = &s->cpu.bus;
    s->low_rom = low_rom;
    /* page 0 is all below $18000: ROM to read and RAM to write, or RAM to read, write protected */
    b->rpage[0] = low_rom ? s->rom : s->ram;
    b->wpage[0] = low_rom ? s->ram : NULL;
    /* page 1 is split at $18000, so the bus functions take it */
    b->rpage[1] = b->wpage[1] = NULL;
}

/* ---- the clock --------------------------------------------------------- */

static int bcd(int v) { return (v / 10) << 4 | v % 10; }
static int unbcd(int v) { return (v >> 4) * 10 + (v & 15); }

static uint8_t rtc_read(superql *s, unsigned i) {
    if (i < RTC_REGS + 1) return i == RTC_REGS ? s->rtc_ctl : s->nvram[i];
    if (s->rtc_ctl & 0x80) return s->nvram[i];     /* being written: what was written */
    time_t t = time(NULL) + (time_t)s->clock_offset;
    struct tm tm;
    localtime_r(&t, &tm);
    switch (i - RTC_REGS) {
    case 1: return (uint8_t)bcd(tm.tm_sec);
    case 2: return (uint8_t)bcd(tm.tm_min);
    case 3: return (uint8_t)bcd(tm.tm_hour);
    case 4: return (uint8_t)bcd(tm.tm_wday + 1);
    case 5: return (uint8_t)bcd(tm.tm_mday);
    case 6: return (uint8_t)bcd(tm.tm_mon + 1);
    default: return (uint8_t)bcd(tm.tm_year % 100);
    }
}

static void rtc_write(superql *s, unsigned i, uint8_t v) {
    if (i != RTC_REGS) { s->nvram[i] = v; return; }
    if ((s->rtc_ctl & 0x80) && !(v & 0x80)) {      /* the write bit cleared: the clock takes the time written */
        struct tm tm = { 0 };
        const uint8_t *r = s->nvram + RTC_REGS;
        tm.tm_sec = unbcd(r[1]); tm.tm_min = unbcd(r[2]); tm.tm_hour = unbcd(r[3]);
        tm.tm_mday = unbcd(r[5]); tm.tm_mon = unbcd(r[6]) - 1;
        int y = unbcd(r[7]);
        tm.tm_year = y < 70 ? y + 100 : y;
        tm.tm_isdst = -1;
        s->clock_offset = (int64_t)(mktime(&tm) - time(NULL));
    }
    if (!(s->rtc_ctl & 0x80) && (v & 0x80))        /* the write bit set: start from the time now */
        for (unsigned k = 1; k < 8; k++) s->nvram[RTC_REGS + k] = rtc_read(s, RTC_REGS + k);
    s->rtc_ctl = v;
}

/* ---- the IDE disk ------------------------------------------------------ */

static void ide_status(sql_ide *d, uint8_t st) { d->status = st; d->intrq = 1; }

static uint32_t ide_lba(const sql_ide *d) {
    if (d->head & 0x40) return (uint32_t)(d->head & 15) << 24 | (uint32_t)d->cylh << 16 | (uint32_t)d->cyll << 8 | d->sect;
    uint32_t cyl = (uint32_t)d->cylh << 8 | d->cyll;
    return (cyl * d->heads + (d->head & 15)) * d->spt + (d->sect ? d->sect - 1u : 0);
}

/* A QXL.WIN made by QPC or Q-emuLator has no geometry in its QLWA header
 * (sectors per track and heads at $24 and $26 are 0). SMSQ/E's Q40 driver
 * takes its CHS geometry from there (dv3/qlf/check40), and with none it puts
 * the bare sector number in the registers, which loses bits 12-15: past the
 * disk's first 2 MB it reads the wrong sectors. So sector 0 shows the drive's
 * own geometry, as a disk prepared on a Q40 has, and the file keeps its zeros. */
static void ide_geometry(sql_ide *d, int show) {
    if (d->lba != 0 || memcmp(d->buf, "QLWA", 4)) return;
    if (show) {
        unsigned spt = (unsigned)d->buf[0x24] << 8 | d->buf[0x25], heads = (unsigned)d->buf[0x26] << 8 | d->buf[0x27];
        d->geometry = !spt && !heads;
        if (!d->geometry) {                        /* a disk from a Q40: the drive takes the geometry it was made with */
            if (spt && spt < 256 && heads && heads <= 16) { d->spt = (uint16_t)spt; d->heads = (uint16_t)heads; }
            return;
        }
        d->buf[0x24] = (uint8_t)(d->spt >> 8); d->buf[0x25] = (uint8_t)d->spt;
        d->buf[0x26] = (uint8_t)(d->heads >> 8); d->buf[0x27] = (uint8_t)d->heads;
        d->buf[0x28] = (uint8_t)(d->cyls >> 8); d->buf[0x29] = (uint8_t)d->cyls;
    } else if (d->geometry) {
        memset(d->buf + 0x24, 0, 6);
    }
}

static int ide_load(sql_ide *d) {
    if (d->lba >= d->sectors || disk_seek(d->f, (int64_t)d->lba * SQL_SECTOR) ||
        fread(d->buf, 1, SQL_SECTOR, d->f) != SQL_SECTOR) { d->err = 0x10; ide_status(d, 0x51); return -1; }
    ide_geometry(d, 1);
    d->pos = 0;
    ide_status(d, 0x58);                           /* ready, seek done, data request */
    return 0;
}

static void ide_put_word(uint8_t *p, int i, uint16_t w) { p[i] = (uint8_t)(w >> 8); p[i + 1] = (uint8_t)w; }

static void ide_identify(sql_ide *d) {
    memset(d->buf, 0, SQL_SECTOR);
    uint16_t w[256] = { 0 };
    w[0] = 0x0040;
    w[1] = d->cyls; w[3] = d->heads; w[6] = d->spt;
    static const char model[40] = "OpenQL Super QL WIN1                    ";
    for (int i = 0; i < 20; i++) w[27 + i] = (uint16_t)(model[2 * i] << 8 | model[2 * i + 1]);
    static const char serial[20] = "ACSQL0001           ";
    for (int i = 0; i < 10; i++) w[10 + i] = (uint16_t)(serial[2 * i] << 8 | serial[2 * i + 1]);
    w[47] = 0x8001;
    w[49] = 0x0200;                                /* LBA */
    w[53] = 1;
    w[54] = d->cyls; w[55] = d->heads; w[56] = d->spt;
    w[57] = (uint16_t)(d->sectors & 0xffff); w[58] = (uint16_t)(d->sectors >> 16);
    w[60] = (uint16_t)(d->sectors & 0xffff); w[61] = (uint16_t)(d->sectors >> 16);
    /* each word as SMSQ/E reads it, high byte first (ide_data_read) */
    for (int i = 0; i < 256; i++) ide_put_word(d->buf, 2 * i, w[i]);
    d->pos = 0; d->left = 1; d->writing = 0;
    ide_status(d, 0x58);
}

static void ide_command(sql_ide *d, uint8_t cmd) {
    d->err = 0;
    if (!d->f || (d->head & 0x10)) { d->err = 0x04; ide_status(d, d->f ? 0x41 : 0x00); return; }   /* no drive, or the slave */
    switch (cmd) {
    case 0x20: case 0x21:                          /* read sectors */
        d->lba = ide_lba(d); d->left = d->scnt ? d->scnt : 256; d->writing = 0;
        if (ide_load(d) == 0) {}
        break;
    case 0x30: case 0x31:                          /* write sectors */
        if (d->read_only) { d->err = 0x04; ide_status(d, 0x51); break; }
        d->lba = ide_lba(d); d->left = d->scnt ? d->scnt : 256; d->writing = 1; d->pos = 0;
        ide_status(d, 0x58);
        break;
    case 0xEC: ide_identify(d); break;             /* identify */
    case 0x91: case 0x10: case 0xEF: case 0xE0: case 0xE1: case 0xE5: case 0x70:   /* parameters, recalibrate, features, power, seek */
        ide_status(d, 0x50);
        break;
    default:
        d->err = 0x04; ide_status(d, 0x51);
        break;
    }
}

static uint16_t ide_data_read(sql_ide *d) {
    if (!(d->status & 0x08) || d->writing) return 0xffff;
    /* The disk's first byte is the word's high byte. SMSQ/E's Q40 driver
     * (dv3/q40/hd_ide_rsect) takes a disk with no FAT32 on it as CHS and
     * stores each word as it reads it, so a QXL.WIN file is a whole WIN1
     * this way round. (It swaps the bytes only for a QXL.WIN inside FAT32.) */
    uint16_t v = (uint16_t)(d->buf[d->pos] << 8 | d->buf[d->pos + 1]);
    d->pos += 2;
    if (d->pos >= SQL_SECTOR) {
        if (--d->left > 0) { d->lba++; ide_load(d); }
        else ide_status(d, 0x50);
    }
    return v;
}

static void ide_data_write(sql_ide *d, uint16_t v) {
    if (!(d->status & 0x08) || !d->writing) return;
    ide_put_word(d->buf, d->pos, v);
    d->pos += 2;
    if (d->pos < SQL_SECTOR) return;
    ide_geometry(d, 0);
    if (d->lba >= d->sectors || disk_seek(d->f, (int64_t)d->lba * SQL_SECTOR) ||
        fwrite(d->buf, 1, SQL_SECTOR, d->f) != SQL_SECTOR) { d->err = 0x10; ide_status(d, 0x51); return; }
    fflush(d->f);
    d->pos = 0;
    if (--d->left > 0) { d->lba++; ide_status(d, 0x58); }
    else ide_status(d, 0x50);
}

/* ---- the multi-I/O card ------------------------------------------------ */

static int uart_of(unsigned port) {
    switch (port & 0x3f8) { case 0x3f8: return 0; case 0x2f8: return 1; case 0x3e8: return 2; case 0x2e8: return 3; }
    return -1;
}

static void uart_put(sql_uart *c, uint8_t ch) {
    int next = (c->rx_tail + 1) % SQL_RXQ;
    if (next == c->rx_head) return;                   /* overrun: the byte is lost, as on the chip */
    c->rxq[c->rx_tail] = ch;
    c->rx_tail = next;
}

static int uart_space(const sql_uart *c) {
    return SQL_RXQ - 1 - (c->rx_tail - c->rx_head + SQL_RXQ) % SQL_RXQ;
}

static uint8_t isa_read8(superql *s, unsigned port) {
    int u = uart_of(port);
    if (u >= 0) {
        sql_uart *c = &s->com[u];
        int rx = c->rx_head != c->rx_tail;
        switch (port & 7) {
        case 0:
            if (c->lcr & 0x80) return c->dll;
            if (!rx) return 0;
            { uint8_t v = c->rxq[c->rx_head]; c->rx_head = (c->rx_head + 1) % SQL_RXQ; update_irq(s); return v; }
        case 1: return (c->lcr & 0x80) ? c->dlm : c->ier;
        case 2:                                       /* bits 7-6 the FIFOs (com_check tests them); 4 received data, 1 none */
            return (uint8_t)(((c->fcr & 1) ? 0xc0 : 0) | (rx && (c->ier & 1) ? 0x04 : 0x01));
        case 3: return c->lcr;
        case 4: return c->mcr;
        case 5: return (uint8_t)(0x60 | rx);          /* the transmitter is always empty; bit 0 data ready */
        case 6: return 0xb0;                          /* CTS, DSR and DCD */
        default: return c->scr;
        }
    }
    switch (port) {
    case 0x378: return s->lpt_data;
    case 0x379: return 0xdf;                          /* not busy, selected, no error, paper in */
    case 0x37a: return s->lpt_ctl;
    case 0x3f0: case 0x3f1: case 0x3f2: case 0x3f3: case 0x3f4: case 0x3f5: case 0x3f7: {
        uint8_t v = fdc_read(&s->fdc, port);
        update_irq(s);                                /* a result read to its end drops IRQ 6 */
        return v;
    }
    case 0x1f1: return s->ide.err;
    case 0x1f2: return s->ide.scnt;
    case 0x1f3: return s->ide.sect;
    case 0x1f4: return s->ide.cyll;
    case 0x1f5: return s->ide.cylh;
    case 0x1f6: return s->ide.head;
    case 0x1f7:                                       /* the status: reading it drops INTRQ (the alternate at $3F6 does not) */
        s->ide.intrq = 0;
        update_irq(s);
        return s->ide.f && !(s->ide.head & 0x10) ? s->ide.status : 0x00;
    case 0x3f6: return s->ide.f && !(s->ide.head & 0x10) ? s->ide.status : 0x00;
    case 0x177: case 0x376: return 0x00;              /* no second channel */
    default: return 0xff;
    }
}

static void isa_write8(superql *s, unsigned port, uint8_t v) {
    int u = uart_of(port);
    if (u >= 0) {
        sql_uart *c = &s->com[u];
        switch (port & 7) {
        case 0:
            if (c->lcr & 0x80) c->dll = v;
            else if (s->ser_out) s->ser_out(s->opaque, u, v);
            break;
        case 1: if (c->lcr & 0x80) c->dlm = v; else c->ier = v; break;
        case 2:
            c->fcr = v;
            if (v & 2) c->rx_head = c->rx_tail = 0;   /* clear the receive FIFO */
            break;
        case 3: c->lcr = v; break;
        case 4:
            /* a mouse answers 'M' when DTR and RTS come on (the mouse is powered from them) */
            if (u == s->mouse_port && (v & 3) == 3 && (c->mcr & 3) != 3) {
                uart_put(c, 'M');
                s->mouse_dx = s->mouse_dy = 0;
                s->mouse_sent = s->mouse_buttons;
            }
            c->mcr = v;
            break;
        case 7: c->scr = v; break;
        default: break;
        }
        update_irq(s);
        return;
    }
    sql_ide *d = &s->ide;
    switch (port) {
    case 0x378: s->lpt_data = v; break;
    case 0x37a:                                       /* the strobe's rising edge (bit 0) sends the byte */
        if ((v & 1) && !(s->lpt_ctl & 1) && s->par_out) s->par_out(s->opaque, s->lpt_data);
        s->lpt_ctl = v;
        break;
    case 0x3f2: case 0x3f4: case 0x3f5: case 0x3f7: fdc_write(&s->fdc, port, v); break;
    case 0x1f1: break;
    case 0x1f2: d->scnt = v; break;
    case 0x1f3: d->sect = v; break;
    case 0x1f4: d->cyll = v; break;
    case 0x1f5: d->cylh = v; break;
    case 0x1f6: d->head = v; break;
    case 0x1f7: ide_command(d, v); break;
    case 0x3f6:
        if ((v & 4) && !(d->ctl & 4)) { d->err = 1; ide_status(d, d->f ? 0x50 : 0); }   /* soft reset */
        d->ctl = v;
        break;
    default: break;
    }
    update_irq(s);
}

/* ---- the bus ----------------------------------------------------------- */

static uint8_t master_read8(superql *s, uint32_t off) {
    switch (off >> 2) {
    case 0x00: return s->ir;
    case 0x01: return s->eir;
    case 0x07: return s->kq_head != s->kq_tail ? s->keyq[s->kq_head] : 0;   /* the key code */
    case 0x06: return s->dmode;
    default: return 0xff;
    }
}

static void master_write8(superql *s, uint32_t off, uint8_t v) {
    if (off >= 0x8000) { if (off < 0x8008) s->dac[off >= 0x8004] = v; return; }   /* DAC left $FF008000, right $FF008004 */
    switch (off >> 2) {
    case 0x02: s->kie = v & 1; break;
    case 0x03: s->sie = v & 1; break;
    case 0x04: s->eie = v; break;
    case 0x05:                                        /* the sample interrupt's enable */
        if ((v & 1) && !s->uie) s->next_sample = s->cpu.clock + SQL_CLOCK_HZ / sql_audio_rate(s);
        s->uie = v & 1;
        if (!s->uie) s->sample_pending = 0;
        break;
    case 0x06: s->dmode = v & 3; break;
    case 0x08:                                        /* key acknowledge: the next code */
        if (s->kq_head != s->kq_tail) s->kq_head = (s->kq_head + 1) % SQL_KEYQ;
        break;
    case 0x09: s->ir &= (uint8_t)~SQL_IR_FRAME; break;
    case 0x0a: s->sample_pending = 0; break;          /* the sample interrupt's acknowledge */
    case 0x0b: s->srate = v & 1; break;
    case 0x0c: s->led = v; break;
    default: break;                                   /* 50 us acknowledge, extension bus reset */
    }
    update_irq(s);
}

static uint8_t read8(superql *s, uint32_t a) {
    uint32_t pg = a >> 16, off = a & 0xffff;
    if (pg == 1) return a < SQL_LOW_ROM_END && s->low_rom ? s->rom[a] : s->ram[a];
    if (pg == MASTER_PAGE) return master_read8(s, off);
    if (pg == RTC_PAGE) return rtc_read(s, (off & 0x1fff) >> 2);
    if (a - ISA_BASE < 0x100000u) return isa_read8(s, ((a - ISA_BASE) >> 2) & 0x3ff);
    return 0xff;
}

static void write8(superql *s, uint32_t a, uint8_t v) {
    uint32_t pg = a >> 16, off = a & 0xffff;
    if (pg == 1) { if (a >= SQL_LOW_ROM_END || s->low_rom) s->ram[a] = v; return; }
    if (pg == MASTER_PAGE) { master_write8(s, off, v); return; }
    if (pg == LOWSW_PAGE) { set_low(s, off < 0x8000); return; }   /* $FF010000 low ROM, $FF018000 low RAM */
    if (pg == RTC_PAGE) { rtc_write(s, (off & 0x1fff) >> 2, v); return; }
    if (a - ISA_BASE < 0x100000u) isa_write8(s, ((a - ISA_BASE) >> 2) & 0x3ff, v);
}

static uint32_t bus_read(void *opaque, uint32_t a, int size) {
    superql *s = opaque;
    if (size == 2 && a - ISA_BASE < 0x100000u && (((a - ISA_BASE) >> 2) & 0x3ff) == 0x1f0) {
        uint16_t v = ide_data_read(&s->ide);
        update_irq(s);
        return v;
    }
    uint32_t v = 0;
    for (int i = 0; i < size; i++) v = v << 8 | read8(s, a + (uint32_t)i);
    return v;
}

static void bus_write(void *opaque, uint32_t a, uint32_t value, int size) {
    superql *s = opaque;
    if (size == 2 && a - ISA_BASE < 0x100000u && (((a - ISA_BASE) >> 2) & 0x3ff) == 0x1f0) { ide_data_write(&s->ide, (uint16_t)value); update_irq(s); return; }
    if (size > 1 && a - ISA_BASE < 0x100000u) { write8(s, a + (uint32_t)size - 1, (uint8_t)value); return; }   /* a wider write to a byte port */
    if (size > 1 && (a >> 16) == LOWSW_PAGE) { set_low(s, (a & 0xffff) < 0x8000); return; }
    for (int i = 0; i < size; i++) write8(s, a + (uint32_t)i, (uint8_t)(value >> (8 * (size - 1 - i))));
}

/* ---- the board --------------------------------------------------------- */

int sql_init(superql *s, const char *rom_path, uint32_t ram_mb) {
    memset(s, 0, sizeof *s);
    memset(s->open, 0xff, sizeof s->open);
    s->mouse_port = 1;                                /* COM2: COM1 stays the serial panel's */
    if (ram_mb < 4) ram_mb = 4;
    if (ram_mb > SQL_MAX_RAM_MB) ram_mb = SQL_MAX_RAM_MB;
    s->ram_bytes = ram_mb << 20;
    s->ram = calloc(1, (size_t)s->ram_bytes);
    s->rom = malloc(SQL_ROM_BYTES);
    s->vram = calloc(1, SQL_VRAM_BYTES);
    if (!s->ram || !s->rom || !s->vram) return -1;
    memset(s->rom, 0xff, SQL_ROM_BYTES);
    FILE *f = fopen(rom_path, "rb");
    if (!f) { fprintf(stderr, "superql: cannot open %s\n", rom_path); return -1; }
    s->rom_len = (uint32_t)fread(s->rom, 1, SQL_ROM_BYTES, f);
    int more = fgetc(f) != EOF;
    fclose(f);
    if (more || s->rom_len < 8) { fprintf(stderr, "superql: %s is not a ROM of 8 bytes to 8 MB\n", rom_path); return -1; }
    s->rom_hash = state_hash(s->rom, s->rom_len);    /* before sql_set_display changes its configuration */

    ac090 *c = &s->cpu;
    ac090_init(c, AC090_68040);
    c->addr_mask = 0xffffffffu;                       /* a 68040: 32-bit addresses */
    c->bus.opaque = s;
    c->bus.read = bus_read;
    c->bus.write = bus_write;
    for (uint32_t pg = 0; pg < 65536; pg++) {
        uint32_t a = pg << 16;
        if (a < s->ram_bytes) c->bus.rpage[pg] = c->bus.wpage[pg] = s->ram + a;
        else if (a - SQL_ROM_BASE < SQL_ROM_BYTES) { c->bus.rpage[pg] = s->rom + (a - SQL_ROM_BASE); c->bus.wpage[pg] = NULL; }
        else if (a - SQL_VRAM_BASE < SQL_VRAM_BYTES) c->bus.rpage[pg] = c->bus.wpage[pg] = s->vram + (a - SQL_VRAM_BASE);
        else if (pg >= MASTER_PAGE) c->bus.rpage[pg] = c->bus.wpage[pg] = NULL;   /* the master chip, clock and I/O */
        else { c->bus.rpage[pg] = s->open; c->bus.wpage[pg] = NULL; }
        c->bus.wait[pg] = 0;
    }
    sql_reset(s, 1);
    return 0;
}

/* SMSQ/E's configuration blocks (<<QCFX>>, level 2, as MenuConfig edits
 * them): the item with this 4-letter name in the block with this name, in
 * the ROM image; NULL if there is none. Each item is 16 bytes: its name,
 * a type and a key byte, then a word that is where its value is, relative
 * to that word. */
static uint8_t *rom_config_item(superql *s, const char *block, const char *item) {
    const uint8_t *r = s->rom;
    uint32_t n = s->rom_len, bl = (uint32_t)strlen(block);
    for (uint32_t i = 0; i + 16 < n; i += 2) {
        if (memcmp(r + i, "<<QCFX>>02", 10)) continue;
        uint32_t p = i + 10, len = (uint32_t)(r[p] << 8 | r[p + 1]);
        if (len != bl || p + 2 + len > n || memcmp(r + p + 2, block, len)) continue;
        p += 2 + ((len + 1) & ~1u);                     /* the name, then the version */
        if (p + 2 > n) return NULL;
        len = (uint32_t)(r[p] << 8 | r[p + 1]);
        p += 2 + ((len + 1) & ~1u);
        for (; p + 16 <= n && !(r[p] == 0xff && r[p + 1] == 0xff); p += 16) {
            if (memcmp(r + p, item, 4)) continue;
            int32_t at = (int32_t)(p + 6) + (int16_t)(r[p + 6] << 8 | r[p + 7]);
            return at >= 0 && (uint32_t)at < n ? s->rom + at : NULL;
        }
        return NULL;
    }
    return NULL;
}

int sql_set_display(superql *s, int dmode) {
    uint8_t *v = rom_config_item(s, "SMSQ", "OSPD");   /* "Initial display mode": 1 QL mode 4, 2 small 16 bit, 3 large 16 bit */
    if (!v || *v < SQL_D_QL4 || *v > SQL_D_LARGE || dmode < SQL_D_QL4 || dmode > SQL_D_LARGE) return -1;
    *v = (uint8_t)dmode;
    return 0;
}

int sql_attach_disk(superql *s, const char *path, int read_only) {
    sql_ide *d = &s->ide;
    FILE *f = fopen(path, read_only ? "rb" : "r+b");
    if (!f) { fprintf(stderr, "superql: cannot open %s\n", path); return -1; }
    int64_t n = disk_size(f);
    if (n < 0) { fclose(f); fprintf(stderr, "superql: cannot size %s\n", path); return -1; }
    d->f = f;
    d->read_only = read_only;
    d->sectors = (uint32_t)(n / SQL_SECTOR);
    d->heads = 16; d->spt = 63;
    uint32_t cyl = d->sectors / (16u * 63u);
    d->cyls = (uint16_t)(cyl > 16383 ? 16383 : cyl ? cyl : 1);
    d->status = 0x50;
    return 0;
}

void sql_reset(superql *s, int clear_ram) {
    if (clear_ram) memset(s->ram, 0, s->ram_bytes);
    s->ir = s->eir = s->kie = s->sie = s->eie = s->uie = s->srate = 0;
    s->dmode = SQL_D_QL4;
    s->kq_head = s->kq_tail = 0;
    memset(s->com, 0, sizeof s->com);
    s->mouse_dx = s->mouse_dy = 0;
    s->lpt_ctl = 0;
    s->sample_pending = 0;
    s->dac[0] = s->dac[1] = 0x80;
    s->aq_head = s->aq_tail = 0;
    s->fdc.dor = 0;                                   /* the floppy controller held in reset until the driver lets it go */
    fdc_reset(&s->fdc);
    set_low(s, 1);                                    /* the reset vectors come from the ROM */
    ac090_set_irq(&s->cpu, 0);
    ac090_reset(&s->cpu);
    s->next_frame = s->cpu.clock + SQL_FRAME_CLOCKS;
}

void sql_free(superql *s) {
    if (s->ide.f) fclose(s->ide.f);
    for (int i = 0; i < FDC_DRIVES; i++) fdc_eject(&s->fdc, i);
    free(s->ram); free(s->rom); free(s->vram);
    s->ram = s->rom = s->vram = NULL;
    s->ide.f = NULL;
}

/* ---- the mouse ---------------------------------------------------------- */

/* One Microsoft mouse packet a frame while there is movement or a button
 * change, 7 bits a byte: 1 L R Y7 Y6 X7 X6, then 0 X5-X0, then 0 Y5-Y0.
 * 1200 baud carries 40 packets a second; a frame's 50 is near enough. */
static void mouse_frame(superql *s) {
    if (s->mouse_port < 0) return;
    sql_uart *c = &s->com[s->mouse_port];
    if ((c->mcr & 3) != 3) return;                    /* DTR and RTS off: the mouse has no power */
    if (!s->mouse_dx && !s->mouse_dy && s->mouse_buttons == s->mouse_sent) return;
    if (uart_space(c) < 3) return;
    int dx = s->mouse_dx < -128 ? -128 : s->mouse_dx > 127 ? 127 : s->mouse_dx;
    int dy = s->mouse_dy < -128 ? -128 : s->mouse_dy > 127 ? 127 : s->mouse_dy;
    s->mouse_dx -= dx; s->mouse_dy -= dy;
    uint8_t x = (uint8_t)dx, y = (uint8_t)dy, b = s->mouse_buttons;
    uart_put(c, (uint8_t)(0x40 | (b & 1) << 5 | (b & 2) << 3 | (y >> 6) << 2 | (x >> 6)));
    uart_put(c, x & 0x3f);
    uart_put(c, y & 0x3f);
    s->mouse_sent = b;
    update_irq(s);
}

void sql_mouse_move(superql *s, int dx, int dy) {
    s->mouse_dx += dx; s->mouse_dy += dy;
    if (s->mouse_dx > 4096) s->mouse_dx = 4096;
    if (s->mouse_dx < -4096) s->mouse_dx = -4096;
    if (s->mouse_dy > 4096) s->mouse_dy = 4096;
    if (s->mouse_dy < -4096) s->mouse_dy = -4096;
}

void sql_mouse_buttons(superql *s, uint8_t buttons) { s->mouse_buttons = buttons & 3; }

int sql_serial_space(const superql *s, int port) {
    if (port < 0 || port > 3 || port == s->mouse_port) return 0;
    const sql_uart *c = &s->com[port];
    if (!(c->ier & 1) || (c->mcr & 9) != 9) return 0;   /* until a channel is open (DTR, OUT2), what came would be lost */
    return uart_space(c);
}

void sql_serial_in(superql *s, int port, uint8_t ch) {
    if (port < 0 || port > 3 || port == s->mouse_port) return;
    uart_put(&s->com[port], ch);
    update_irq(s);
}

uint64_t sql_run(superql *s, uint64_t until) {
    ac090 *c = &s->cpu;
    uint64_t start = c->instructions;
    while (c->clock < until && !c->halted && !c->dbg_stop) {   /* (dbg_stop: the debugger stopped the CPU) */
        uint64_t next = s->next_frame < until ? s->next_frame : until;
        if (s->uie && s->next_sample < next) next = s->next_sample;
        update_irq(s);
        if (next > c->clock) ac090_run(c, next - c->clock);
        while (s->uie && c->clock >= s->next_sample) {   /* a sample: the DACs as they are now, then the interrupt */
            s->next_sample += SQL_CLOCK_HZ / sql_audio_rate(s);
            int t = (s->aq_tail + 1) % SQL_AUDIOQ;
            if (t != s->aq_head) {
                s->audioq[s->aq_tail][0] = (int16_t)((s->dac[0] - 128) * 256);
                s->audioq[s->aq_tail][1] = (int16_t)((s->dac[1] - 128) * 256);
                s->aq_tail = t;
            }
            s->sample_pending = 1;
        }
        while (c->clock >= s->next_frame) {
            s->next_frame += SQL_FRAME_CLOCKS;
            s->frames++;
            s->ir |= SQL_IR_FRAME;
            mouse_frame(s);
        }
    }
    update_irq(s);
    return c->instructions - start;
}

/* ---- saved states -------------------------------------------------------- */

int sql_save_state(superql *s, const char *path, const char **why) {
    FILE *f = fopen(path, "wb");
    if (!f) { *why = "cannot write the file"; return -1; }
    int ok = state_write_header(f, 2, sizeof *s, s->ram_bytes, s->rom_hash) == 0 &&
             fwrite(s, sizeof *s, 1, f) == 1 && fwrite(s->ram, 1, s->ram_bytes, f) == s->ram_bytes &&
             fwrite(s->vram, 1, SQL_VRAM_BYTES, f) == SQL_VRAM_BYTES;
    if (fclose(f)) ok = 0;
    if (!ok) { *why = "cannot write the file"; remove(path); return -1; }
    return 0;
}

int sql_load_state(superql *s, const char *path, const char **why) {
    FILE *f = fopen(path, "rb");
    if (!f) { *why = "no saved state"; return -1; }
    superql *t = malloc(sizeof *t);
    int ok = t && state_check_header(f, 2, sizeof *s, s->ram_bytes, s->rom_hash, why) == 0;
    if (ok && fread(t, sizeof *t, 1, f) != 1) { ok = 0; *why = "the file is cut short"; }
    /* RAM and video RAM straight in: a file cut short past here leaves a machine to reset */
    if (ok && (fread(s->ram, 1, s->ram_bytes, f) != s->ram_bytes || fread(s->vram, 1, SQL_VRAM_BYTES, f) != SQL_VRAM_BYTES)) {
        ok = 0; *why = "the file is cut short: reset";
    }
    fclose(f);
    if (ok) {
        state_cpu_take(&s->cpu, &t->cpu);
        s->ir = t->ir; s->eir = t->eir; s->kie = t->kie; s->sie = t->sie; s->eie = t->eie; s->uie = t->uie;
        s->dmode = t->dmode; s->srate = t->srate; s->led = t->led;
        memcpy(s->keyq, t->keyq, sizeof s->keyq); s->kq_head = t->kq_head; s->kq_tail = t->kq_tail;
        s->next_frame = t->next_frame; s->frames = t->frames;
        memcpy(s->nvram, t->nvram, sizeof s->nvram); s->clock_offset = t->clock_offset; s->rtc_ctl = t->rtc_ctl;
        memcpy(s->com, t->com, sizeof s->com); s->lpt_data = t->lpt_data; s->lpt_ctl = t->lpt_ctl;
        /* the disks in now stay in; their controllers' registers come back */
        sql_ide *d = &s->ide;
        d->err = t->ide.err; d->scnt = t->ide.scnt; d->sect = t->ide.sect; d->cyll = t->ide.cyll; d->cylh = t->ide.cylh;
        d->head = t->ide.head; d->status = t->ide.status; d->ctl = t->ide.ctl; d->intrq = t->ide.intrq;
        memcpy(d->buf, t->ide.buf, sizeof d->buf); d->pos = t->ide.pos; d->left = t->ide.left; d->writing = t->ide.writing; d->lba = t->ide.lba;
        sql_fdc keep = s->fdc;
        s->fdc = t->fdc;
        for (int i = 0; i < FDC_DRIVES; i++) { uint8_t trk = s->fdc.d[i].track; s->fdc.d[i] = keep.d[i]; s->fdc.d[i].track = trk; }
        memcpy(s->dac, t->dac, sizeof s->dac); s->sample_pending = t->sample_pending; s->next_sample = t->next_sample;
        s->aq_head = s->aq_tail = 0;
        s->mouse_dx = s->mouse_dy = 0; s->mouse_buttons = t->mouse_buttons; s->mouse_sent = t->mouse_sent;
        set_low(s, t->low_rom);
        update_irq(s);
    }
    free(t);
    return ok ? 0 : -1;
}

/* ---- floppies and sound --------------------------------------------------- */

int sql_insert_floppy(superql *s, int drive, const char *path, int read_only) { return fdc_insert(&s->fdc, drive, path, read_only); }
void sql_eject_floppy(superql *s, int drive) { fdc_eject(&s->fdc, drive); }

unsigned sql_audio_rate(const superql *s) { return s->srate ? 40000u : 20000u; }

int sql_audio_read(superql *s, int16_t *out, int max) {
    int n = 0;
    while (n < max && s->aq_head != s->aq_tail) {
        out[2 * n] = s->audioq[s->aq_head][0];
        out[2 * n + 1] = s->audioq[s->aq_head][1];
        s->aq_head = (s->aq_head + 1) % SQL_AUDIOQ;
        n++;
    }
    return n;
}

/* ---- the keyboard ------------------------------------------------------ */

void sql_key_raw(superql *s, uint8_t code) {
    int next = (s->kq_tail + 1) % SQL_KEYQ;
    if (next == s->kq_head) return;                   /* full: the keyboard's own buffer would drop it too */
    s->keyq[s->kq_tail] = code;
    s->kq_tail = next;
    update_irq(s);
}

/* Amiga raw key -> PC set 2: the low byte the code, bit 8 the E0 prefix. */
static const uint16_t set2[0x68] = {
    [0x00] = 0x0e, [0x01] = 0x16, [0x02] = 0x1e, [0x03] = 0x26, [0x04] = 0x25, [0x05] = 0x2e, [0x06] = 0x36,
    [0x07] = 0x3d, [0x08] = 0x3e, [0x09] = 0x46, [0x0a] = 0x45, [0x0b] = 0x4e, [0x0c] = 0x55, [0x0d] = 0x5d,
    [0x0f] = 0x70,
    [0x10] = 0x15, [0x11] = 0x1d, [0x12] = 0x24, [0x13] = 0x2d, [0x14] = 0x2c, [0x15] = 0x35, [0x16] = 0x3c,
    [0x17] = 0x43, [0x18] = 0x44, [0x19] = 0x4d, [0x1a] = 0x54, [0x1b] = 0x5b,
    [0x1d] = 0x69, [0x1e] = 0x72, [0x1f] = 0x7a,
    [0x20] = 0x1c, [0x21] = 0x1b, [0x22] = 0x23, [0x23] = 0x2b, [0x24] = 0x34, [0x25] = 0x33, [0x26] = 0x3b,
    [0x27] = 0x42, [0x28] = 0x4b, [0x29] = 0x4c, [0x2a] = 0x52, [0x2b] = 0x5d,
    [0x2d] = 0x6b, [0x2e] = 0x73, [0x2f] = 0x74,
    [0x30] = 0x61, [0x31] = 0x1a, [0x32] = 0x22, [0x33] = 0x21, [0x34] = 0x2a, [0x35] = 0x32, [0x36] = 0x31,
    [0x37] = 0x3a, [0x38] = 0x41, [0x39] = 0x49, [0x3a] = 0x4a, [0x3c] = 0x71, [0x3d] = 0x6c, [0x3e] = 0x75, [0x3f] = 0x7d,
    [0x40] = 0x29, [0x41] = 0x66, [0x42] = 0x0d, [0x43] = 0x15a, [0x44] = 0x5a, [0x45] = 0x76, [0x46] = 0x171,
    [0x4a] = 0x7b, [0x4c] = 0x175, [0x4d] = 0x172, [0x4e] = 0x174, [0x4f] = 0x16b,
    [0x50] = 0x05, [0x51] = 0x06, [0x52] = 0x04, [0x53] = 0x0c, [0x54] = 0x03, [0x55] = 0x0b, [0x56] = 0x83,
    [0x57] = 0x0a, [0x58] = 0x01, [0x59] = 0x09, [0x5a] = 0x77, [0x5b] = 0x7e, [0x5c] = 0x14a, [0x5d] = 0x7c,
    [0x5e] = 0x79, [0x5f] = 0x78,                     /* Help is F11 */
    [0x60] = 0x12, [0x61] = 0x59, [0x62] = 0x58, [0x63] = 0x14, [0x64] = 0x11, [0x65] = 0x111,
    [0x66] = 0x11f, [0x67] = 0x127,
};

void sql_key_amiga(superql *s, uint8_t raw) {
    int up = raw & 0x80, k = raw & 0x7f;
    if (k >= 0x68 || !set2[k]) return;
    uint16_t code = set2[k];
    if (code & 0x100) sql_key_raw(s, 0xe0);
    if (up) sql_key_raw(s, 0xf0);
    sql_key_raw(s, (uint8_t)code);
}

/* ---- the picture ------------------------------------------------------- */

static uint32_t rgb16(uint16_t v) {             /* GGGGG RRRRR BBBBB x (iod/con2/q4016 palette) */
    uint32_t g = v >> 11 & 31, r = v >> 6 & 31, b = v >> 1 & 31;
    return (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2);
}

void sql_render(superql *s, uint32_t *rgb) {
    static const uint32_t pal[8] = { 0x000000, 0x0000ff, 0xff0000, 0xff00ff, 0x00ff00, 0x00ffff, 0xffff00, 0xffffff };
    if (s->dmode == SQL_D_LARGE) {
        for (int y = 0; y < SQL_OUT_H; y++)
            for (int x = 0; x < SQL_OUT_W; x++) {
                const uint8_t *p = s->vram + (size_t)y * 2048 + (size_t)x * 2;
                rgb[y * SQL_OUT_W + x] = rgb16((uint16_t)(p[0] << 8 | p[1]));
            }
        return;
    }
    for (int y = 0; y < 256; y++) {
        uint32_t line[512];
        if (s->dmode == SQL_D_SMALL) {
            for (int x = 0; x < 512; x++) {
                const uint8_t *p = s->vram + (size_t)y * 1024 + (size_t)x * 2;
                line[x] = rgb16((uint16_t)(p[0] << 8 | p[1]));
            }
        } else {
            const uint8_t *l = s->ram + SQL_QL_SCREEN + y * 128;
            for (int w = 0; w < 64; w++) {
                uint8_t e = l[w * 2], o = l[w * 2 + 1];
                if (s->dmode == SQL_D_QL4) {
                    for (int b = 0; b < 8; b++) {
                        int g = e >> (7 - b) & 1, r = o >> (7 - b) & 1;
                        line[w * 8 + b] = (g ? 0x00ff00 : 0) | (r ? 0xff0000 : 0) | (g && r ? 0x0000ff : 0);
                    }
                } else {
                    for (int b = 0; b < 4; b++) {
                        int g = e >> (7 - 2 * b) & 1, r = o >> (7 - 2 * b) & 1, bl = o >> (6 - 2 * b) & 1;
                        line[w * 8 + b * 2] = line[w * 8 + b * 2 + 1] = pal[g << 2 | r << 1 | bl];
                    }
                }
            }
        }
        for (int x = 0; x < 512; x++) {
            uint32_t v = line[x];
            uint32_t *o = rgb + (size_t)(2 * y) * SQL_OUT_W + 2 * x;
            o[0] = o[1] = o[SQL_OUT_W] = o[SQL_OUT_W + 1] = v;
        }
    }
}
