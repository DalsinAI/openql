/*
 * The Super QL board's cases, without SMSQ/E: a small ROM (hand assembled
 * below) starts from the reset vectors in low ROM, sets the 68040's VBR,
 * switches to low RAM and takes the master chip's frame and keyboard
 * interrupts. Then the map, the clock, the IDE disk, the ports and the
 * picture through the bus.
 *
 *   gcc -std=gnu11 -O2 ../../../cpus/m68k-ac090/native/ac090.c superql.c superql_test.c -lm -o superql_test && ./superql_test
 */
#include "superql.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void wr(superql *s, uint32_t a, uint32_t v, int size) { ac090_write(&s->cpu, a, v, size); }
static uint32_t rd(superql *s, uint32_t a, int size) { return ac090_read(&s->cpu, a, size); }

static const uint16_t boot[] = {
    0x0002, 0x8000,                 /* SSP $28000 */
    0xfe00, 0x0008,                 /* PC $FE000008 */
    0x46fc, 0x2000,                 /* move.w #$2000,sr */
    0x7000,                         /* moveq #0,d0 */
    0x4e7b, 0x0801,                 /* movec d0,vbr */
    0x21fc, 0xfe00, 0x0028, 0x0068, /* move.l #handler,$68.w (level 2 autovector, written to RAM under low ROM) */
    0x50f9, 0xff01, 0x8000,         /* st $FF018000: low RAM */
    0x50f9, 0xff00, 0x0008,         /* st q40_kie */
    0x60fe,                         /* bra.s * */
    /* handler, $28: */
    0x52b9, 0x0002, 0x8100,         /* addq.l #1,$28100 (low RAM below $18000 is write protected) */
    0x1239, 0xff00, 0x001c,         /* move.b q40_kcod,d1 */
    0x13c1, 0x0002, 0x8104,         /* move.b d1,$28104 */
    0x50f9, 0xff00, 0x0020,         /* st q40_kack */
    0x50f9, 0xff00, 0x0024,         /* st q40_fack */
    0x4e73,                         /* rte */
};

static char ser[64], prn[64];
static int nser, nprn;
static void ser_out(void *o, int port, uint8_t ch) { (void)o; if (port == 0 && nser < 63) ser[nser++] = (char)ch; }
static void par_out(void *o, uint8_t ch) { (void)o; if (nprn < 63) prn[nprn++] = (char)ch; }

int main(void) {
    const char *rom = "superql_test_rom.bin", *disk = "superql_test_disk.win";   /* the current folder: Windows has no /tmp */
    uint8_t img[sizeof boot];
    for (size_t i = 0; i < sizeof boot / 2; i++) { img[2 * i] = (uint8_t)(boot[i] >> 8); img[2 * i + 1] = (uint8_t)boot[i]; }
    FILE *wf = fopen(rom, "wb");
    if (!wf || fwrite(img, 1, sizeof img, wf) != sizeof img) return 1;
    fclose(wf);

    static superql s;
    CHECK(sql_init(&s, rom, 128) == 0, "init");
    s.ser_out = ser_out; s.par_out = par_out;

    /* the map */
    CHECK(s.ram_bytes == 128u << 20, "128 MB fitted");
    CHECK(rd(&s, 0, 4) == 0x00028000, "low ROM: address 0 reads the ROM's SSP (%08x)", rd(&s, 0, 4));
    CHECK(rd(&s, 0x08000000, 4) == 0xffffffff, "nothing above 128 MB");
    wr(&s, 0x08000000, 0x1357fdb9, 4);
    CHECK(rd(&s, 0x08000000, 4) != 0x1357fdb9, "SMSQ/E's RAM test finds the top at 128 MB");
    CHECK(rd(&s, 0xfe000004, 4) == 0xfe000008, "the ROM at $FE000000");

    /* the program: vectors from low ROM, VBR, low RAM, frame and keyboard interrupts */
    sql_run(&s, s.cpu.clock + SQL_CLOCK_HZ / 5);
    uint32_t ticks = rd(&s, 0x28100, 4);
    CHECK(!s.low_rom, "the program switched to low RAM");
    CHECK(ticks >= 9 && ticks <= 11, "about ten frame interrupts in 0.2 s (%u)", ticks);
    CHECK(s.cpu.pc == 0xfe000026, "waiting in its loop (pc %08x)", s.cpu.pc);
    sql_key_amiga(&s, 0x20);                         /* A */
    sql_run(&s, s.cpu.clock + SQL_CLOCK_HZ / 50);
    CHECK(rd(&s, 0x28104, 1) == 0x1c, "A is set 2 code $1C (%02x)", rd(&s, 0x28104, 1));
    sql_key_amiga(&s, 0x4c | 0x80);                  /* cursor up released: E0 F0 75 */
    CHECK(s.keyq[(s.kq_tail + SQL_KEYQ - 3) % SQL_KEYQ] == 0xe0 && s.keyq[(s.kq_tail + SQL_KEYQ - 2) % SQL_KEYQ] == 0xf0 &&
          s.keyq[(s.kq_tail + SQL_KEYQ - 1) % SQL_KEYQ] == 0x75, "an extended key's release");

    /* low RAM is write protected below $18000; above it is plain RAM */
    wr(&s, 0x200, 0x12345678, 4);
    CHECK(rd(&s, 0x200, 4) != 0x12345678, "low RAM is write protected");
    wr(&s, 0xff010000, 0xff, 1);                     /* low ROM: writes go to RAM */
    wr(&s, 0x200, 0x12345678, 4);
    wr(&s, 0x17ffe, 0xabcd, 2);
    CHECK(rd(&s, 0x200, 4) == 0xffffffff, "low ROM: reads still come from the ROM (past its end: $FF)");
    wr(&s, 0xff018000, 0xff, 1);
    CHECK(rd(&s, 0x200, 4) == 0x12345678 && rd(&s, 0x17ffe, 2) == 0xabcd, "written under low ROM, read under low RAM");
    wr(&s, 0x18000, 0x55aa, 2);
    CHECK(rd(&s, 0x18000, 2) == 0x55aa, "$18000 up is RAM");

    /* the clock: BCD, and setting it moves it */
    wr(&s, 0xff021fe0, 0x40, 1);
    int sec = (int)rd(&s, 0xff021fe4, 1), yr = (int)rd(&s, 0xff021ffc, 1);
    CHECK((sec & 15) < 10 && sec < 0x60, "seconds in BCD (%02x)", sec);
    time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm);
    CHECK(yr == ((tm.tm_year % 100) / 10 << 4 | tm.tm_year % 10), "the year (%02x)", yr);
    wr(&s, 0xff021fe0, 0x80, 1);
    wr(&s, 0xff021ffc, 0x99, 1);                     /* 1999 */
    wr(&s, 0xff021fe0, 0x00, 1);
    CHECK(rd(&s, 0xff021ffc, 1) == 0x99, "set to 1999");
    wr(&s, 0xff020010, 0x5a, 1);
    CHECK(rd(&s, 0xff020010, 1) == 0x5a, "NVRAM keeps a byte");

    /* the ports */
    for (const char *p = "QL"; *p; p++) wr(&s, 0xff400000 + (0x3f8 << 2) + 1, (uint8_t)*p, 1);
    CHECK(nser == 2 && !memcmp(ser, "QL", 2), "COM1 out");
    CHECK(rd(&s, 0xff400000 + (0x3fd << 2) + 1, 1) == 0x60, "COM1 always ready");
    wr(&s, 0xff400000 + (0x378 << 2) + 1, 'P', 1);
    wr(&s, 0xff400000 + (0x37a << 2) + 1, 1, 1);
    wr(&s, 0xff400000 + (0x37a << 2) + 1, 0, 1);
    CHECK(nprn == 1 && prn[0] == 'P', "LPT1 strobe sends a byte to the printer");

    /* IDE: no disk, then a disk */
    CHECK(rd(&s, 0xff400000 + (0x1f7 << 2) + 1, 1) == 0, "no disk: status 0");
    static uint8_t sec512[512 * 8];
    for (int i = 0; i < (int)sizeof sec512; i++) sec512[i] = (uint8_t)(i / 512 * 16 + (i & 15));
    memcpy(sec512 + 1024, "QLWA", 4);
    wf = fopen(disk, "wb");
    if (!wf || fwrite(sec512, 1, sizeof sec512, wf) != sizeof sec512) return 1;
    fclose(wf);
    CHECK(sql_attach_disk(&s, disk, 0) == 0, "attach");
    uint32_t io = 0xff400000;
#define IDE(port) (io + ((port) << 2) + 1)
    wr(&s, IDE(0x1f6), 0xe0, 1);                      /* master, LBA */
    wr(&s, IDE(0x1f2), 1, 1);
    wr(&s, IDE(0x1f3), 2, 1); wr(&s, IDE(0x1f4), 0, 1); wr(&s, IDE(0x1f5), 0, 1);
    wr(&s, IDE(0x1f7), 0x20, 1);
    CHECK(rd(&s, IDE(0x1f7), 1) & 0x08, "read: data request");
    uint32_t w0 = rd(&s, io + (0x1f0 << 2), 2);
    CHECK(w0 == ('Q' << 8 | 'L'), "the disk's first byte is the word's high byte: 'QL' (%04x)", w0);
    for (int i = 1; i < 256; i++) rd(&s, io + (0x1f0 << 2), 2);
    CHECK(!(rd(&s, IDE(0x1f7), 1) & 0x08), "sector done");
    wr(&s, IDE(0x1f3), 3, 1);
    wr(&s, IDE(0x1f7), 0x30, 1);
    for (int i = 0; i < 256; i++) wr(&s, io + (0x1f0 << 2), 0x4241, 2);
    FILE *f = fopen(disk, "rb"); uint8_t back[2] = { 0 };
    fseek(f, 3 * 512, SEEK_SET); if (fread(back, 1, 2, f) != 2) back[0] = 0; fclose(f);
    CHECK(back[0] == 'B' && back[1] == 'A', "write: the word's high byte first on disk");
    wr(&s, IDE(0x1f7), 0xec, 1);
    uint32_t id0 = rd(&s, io + (0x1f0 << 2), 2);
    CHECK(id0 == 0x0040, "identify word 0 (%04x)", id0);
    for (int i = 1; i < 60; i++) rd(&s, io + (0x1f0 << 2), 2);
    uint32_t lo = rd(&s, io + (0x1f0 << 2), 2), hi = rd(&s, io + (0x1f0 << 2), 2);
    CHECK((hi << 16 | lo) == 8, "identify: 8 sectors (%u)", hi << 16 | lo);
    wr(&s, IDE(0x1f6), 0xf0, 1);                      /* the slave: none */
    wr(&s, IDE(0x1f7), 0xec, 1);
    CHECK(rd(&s, IDE(0x1f7), 1) == 0, "no slave");

    /* A QXL.WIN from QPC names no geometry: sector 0 shows the drive's, and the file keeps its zeros */
    memset(sec512, 0, 512);
    memcpy(sec512, "QLWA", 4);
    wf = fopen(disk, "r+b");
    if (!wf || fwrite(sec512, 1, 512, wf) != 512) return 1;
    fclose(wf);
    wr(&s, IDE(0x1f6), 0xe0, 1);
    wr(&s, IDE(0x1f2), 1, 1);
    wr(&s, IDE(0x1f3), 0, 1); wr(&s, IDE(0x1f4), 0, 1); wr(&s, IDE(0x1f5), 0, 1);
    wr(&s, IDE(0x1f7), 0x20, 1);
    uint16_t hdr[256];
    for (int i = 0; i < 256; i++) hdr[i] = (uint16_t)rd(&s, io + (0x1f0 << 2), 2);
    CHECK(hdr[0x24 / 2] == 63 && hdr[0x26 / 2] == 16, "QLWA with no geometry: 63 sectors, 16 heads shown (%u %u)",
          hdr[0x24 / 2], hdr[0x26 / 2]);
    wr(&s, IDE(0x1f7), 0x30, 1);
    for (int i = 0; i < 256; i++) wr(&s, io + (0x1f0 << 2), hdr[i], 2);
    f = fopen(disk, "rb"); uint8_t geo[4] = { 1, 1, 1, 1 };
    fseek(f, 0x24, SEEK_SET); if (fread(geo, 1, 4, f) != 4) geo[0] = 1; fclose(f);
    CHECK(!geo[0] && !geo[1] && !geo[2] && !geo[3], "written back: the file keeps no geometry");
    sec512[0x25] = 17; sec512[0x27] = 4;               /* a disk made on a Q40: 17 sectors, 4 heads */
    wf = fopen(disk, "r+b");
    if (!wf || fwrite(sec512, 1, 512, wf) != 512) return 1;
    fclose(wf);
    wr(&s, IDE(0x1f7), 0x20, 1);
    for (int i = 0; i < 256; i++) rd(&s, io + (0x1f0 << 2), 2);
    CHECK(s.ide.spt == 17 && s.ide.heads == 4, "a Q40 disk's geometry is the drive's (%u %u)", s.ide.spt, s.ide.heads);

    /* the serial mouse on COM2, as SMSQ/E's com_check and com_mouse find it */
#define COM2(r) (0xff400000 + ((0x2f8 + (r)) << 2) + 1)
#define ISA(p) (0xff400000 + ((p) << 2) + 1)
    wr(&s, COM2(2), 0x00, 1);                         /* FIFOs off: IIR bit 7 clear */
    CHECK(!(rd(&s, COM2(2), 1) & 0x80), "com_check: FIFO off reads bit 7 clear");
    wr(&s, COM2(2), 0x01, 1);
    CHECK(rd(&s, COM2(2), 1) & 0x80, "com_check: FIFO on reads bit 7 set");
    CHECK(!(rd(&s, COM2(5), 1) & 1), "nothing in before DTR and RTS");
    sql_mouse_move(&s, 5, -3);
    sql_mouse_buttons(&s, 1);
    sql_run(&s, s.cpu.clock + 2 * SQL_FRAME_CLOCKS);
    CHECK(!(rd(&s, COM2(5), 1) & 1), "an unpowered mouse sends nothing");
    wr(&s, COM2(4), 0x0b, 1);                         /* DTR, RTS, OUT2 */
    CHECK((rd(&s, COM2(5), 1) & 1) && rd(&s, COM2(0), 1) == 'M', "the mouse answers 'M'");
    CHECK(!(rd(&s, COM2(5), 1) & 1), "then nothing");
    wr(&s, COM2(1), 0x01, 1);                         /* receive interrupt on */
    sql_mouse_move(&s, 5, -3);
    sql_mouse_buttons(&s, 1);
    sql_run(&s, s.cpu.clock + SQL_FRAME_CLOCKS + 10);
    CHECK(s.ir & SQL_IR_SER, "the packet raises the serial interrupt (ir %02x)", s.ir);
    CHECK((rd(&s, COM2(2), 1) & 0x0f) == 0x04, "IIR: received data");
    uint8_t p0 = (uint8_t)rd(&s, COM2(0), 1), p1 = (uint8_t)rd(&s, COM2(0), 1), p2 = (uint8_t)rd(&s, COM2(0), 1);
    CHECK(p0 == (0x40 | 0x20 | 3 << 2) && p1 == 5 && p2 == (0xfd & 0x3f), "packet: left, x +5, y -3 (%02x %02x %02x)", p0, p1, p2);
    CHECK(!(s.ir & SQL_IR_SER) && (rd(&s, COM2(2), 1) & 0x0f) == 0x01, "read out: no interrupt");
    sql_run(&s, s.cpu.clock + 2 * SQL_FRAME_CLOCKS);
    CHECK(!(rd(&s, COM2(5), 1) & 1), "no change, no packet");
    sql_mouse_move(&s, 300, 0);
    sql_run(&s, s.cpu.clock + 3 * SQL_FRAME_CLOCKS);
    p0 = (uint8_t)rd(&s, COM2(0), 1); p1 = (uint8_t)rd(&s, COM2(0), 1); rd(&s, COM2(0), 1);
    CHECK((p0 & 3) == 1 && p1 == 0x3f, "a big move goes in steps of 127 (%02x %02x)", p0, p1);

    /* the picture */
    static uint32_t pic[SQL_OUT_W * SQL_OUT_H];
    wr(&s, 0xff000018, SQL_D_QL4, 1);
    wr(&s, 0x20000, 0x80c0, 2);                       /* pixel 0 green+red (white), pixel 1 red */
    sql_render(&s, pic);
    CHECK(pic[0] == 0xffffff && pic[1] == 0xffffff && pic[SQL_OUT_W] == 0xffffff, "mode 4 white, doubled");
    CHECK(pic[2] == 0xff0000, "mode 4 red (%06x)", pic[2]);
    wr(&s, 0xff000018, SQL_D_LARGE, 1);
    wr(&s, 0xfe800000, 0x07c0, 2);                    /* red in GGGGGRRRRRBBBBBx */
    wr(&s, 0xfe800000 + 2048 + 2, 0xf800, 2);         /* green, at 1,1 */
    sql_render(&s, pic);
    CHECK(pic[0] == 0xff0000, "16-bit red (%06x)", pic[0]);
    CHECK(pic[SQL_OUT_W + 1] == 0x00ff00, "16-bit green at 1,1 (%06x)", pic[SQL_OUT_W + 1]);

    /* sound: the sample interrupt at level 6, 20 kHz, and the DACs' samples */
    {
        static const uint16_t ack[] = { 0x52b9, 0x0002, 0x8108,   /* addq.l #1,$28108 */
                                        0x50f9, 0xff00, 0x0028,   /* st q40_sack */
                                        0x4e73 };                 /* rte */
        for (int i = 0; i < 7; i++) { s.ram[0x28200 + 2 * i] = (uint8_t)(ack[i] >> 8); s.ram[0x28201 + 2 * i] = (uint8_t)ack[i]; }
        s.ram[0x78] = 0x00; s.ram[0x79] = 0x02; s.ram[0x7a] = 0x82; s.ram[0x7b] = 0x00;   /* level 6 autovector (VBR 0) */
        int16_t got[2 * 1024];
        sql_audio_read(&s, got, 1024);
        wr(&s, 0xff008000, 0xc0, 1);
        wr(&s, 0xff008004, 0x40, 1);
        wr(&s, 0xff000014, 1, 1);
        sql_run(&s, s.cpu.clock + SQL_FRAME_CLOCKS);
        int n = sql_audio_read(&s, got, 1024);
        CHECK(sql_audio_rate(&s) == 20000, "20 kHz at reset");
        CHECK(n >= 399 && n <= 401, "a frame of samples at 20 kHz (%d)", n);
        CHECK(n > 0 && got[0] == 0x4000 && got[1] == -0x4000, "the DACs as signed samples (%d %d)", got[0], got[1]);
        uint32_t taken = rd(&s, 0x28108, 4);
        CHECK(taken >= 399 && taken <= 401, "each sample interrupt taken once and acknowledged (%u)", taken);
        wr(&s, 0xff00002c, 1, 1);
        CHECK(sql_audio_rate(&s) == 40000, "40 kHz");
        wr(&s, 0xff000014, 0, 1);
        sql_run(&s, s.cpu.clock + SQL_FRAME_CLOCKS);
        CHECK(sql_audio_read(&s, got, 1024) == 0, "no samples with the sample interrupt off");
    }

    /* the floppy controller at $3F0: out of reset, VERSION */
    {
        wr(&s, ISA(0x3f2), 0x1c, 1);
        CHECK(rd(&s, ISA(0x3f4), 1) == 0x80, "FDC ready for a command (MSR %02x)", rd(&s, ISA(0x3f4), 1));
        wr(&s, ISA(0x3f5), 0x10, 1);
        CHECK(rd(&s, ISA(0x3f4), 1) == 0xd0 && rd(&s, ISA(0x3f5), 1) == 0x90, "VERSION through the ISA ports");
    }

    /* its interrupt: IRQ 6 in the external interrupt register, on level 2 once enabled */
    {
        wr(&s, ISA(0x3f2), 0x18, 1);                 /* into reset, the DMA/IRQ gate on */
        wr(&s, ISA(0x3f2), 0x1c, 1);                 /* out: the reset's interrupt */
        CHECK(rd(&s, 0xff000004, 1) & 0x08, "IRQ 6 after a reset (eir %02x)", rd(&s, 0xff000004, 1));
        CHECK(!(rd(&s, 0xff000000, 1) & 0x10), "no external interrupt while it is not enabled");
        wr(&s, 0xff000010, 1, 1);
        CHECK((rd(&s, 0xff000000, 1) & 0x10) && s.cpu.irq == 2, "enabled: the external interrupt, level 2 (irq %d)", s.cpu.irq);
        for (int i = 0; i < 4; i++) { wr(&s, ISA(0x3f5), 0x08, 1); rd(&s, ISA(0x3f5), 1); rd(&s, ISA(0x3f5), 1); }   /* SENSE INTERRUPT for each drive */
        CHECK(!(rd(&s, 0xff000004, 1) & 0x08), "sensed: IRQ 6 drops");
        wr(&s, ISA(0x3f5), 0x07, 1); wr(&s, ISA(0x3f5), 0x00, 1);   /* RECALIBRATE drive 0 */
        CHECK(rd(&s, 0xff000004, 1) & 0x08, "a seek's end raises IRQ 6");
        wr(&s, ISA(0x3f5), 0x08, 1);
        CHECK(rd(&s, ISA(0x3f5), 1) == 0x20 && rd(&s, ISA(0x3f5), 1) == 0, "SENSE INTERRUPT: seek end, track 0");
        CHECK(!(rd(&s, 0xff000004, 1) & 0x08), "and it drops (eir %02x)", rd(&s, 0xff000004, 1));
        wr(&s, 0xff000010, 0, 1);
    }

    /* serial input on COM1: taken once a channel is open (receive interrupt on, DTR and OUT2) */
    {
        wr(&s, ISA(0x3f8 + 1), 0x00, 1);
        wr(&s, ISA(0x3f8 + 4), 0x08, 1);
        CHECK(sql_serial_space(&s, 0) == 0, "COM1 not open: no room");
        wr(&s, ISA(0x3f8 + 1), 0x01, 1);
        wr(&s, ISA(0x3f8 + 4), 0x0b, 1);
        CHECK(sql_serial_space(&s, 0) > 0, "open: room (%d)", sql_serial_space(&s, 0));
        sql_serial_in(&s, 0, 'Q');
        CHECK((rd(&s, ISA(0x3f8 + 5), 1) & 1) && rd(&s, ISA(0x3f8), 1) == 'Q', "the byte in COM1's receive FIFO");
        CHECK(sql_serial_space(&s, 1) == 0, "COM2 is the mouse's");
    }

    sql_free(&s);
    remove(rom); remove(disk);
    printf("%s (%d failed)\n", fails ? "FAIL" : "ok", fails);
    return fails != 0;
}
