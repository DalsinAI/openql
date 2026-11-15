/*
 * The QL board's cases, without a Firmware/ROM: the IPC link as QDOS drives
 * it (keyboard, keyrow, status), the ZX8301's two modes, the clock, the frame
 * and gap interrupts, the map, and a microdrive image read through the ZX8302. A 6-byte ROM (vectors and BRA.S *) runs.
 *
 *   gcc -std=gnu11 -O2 ../../../cpus/m68k-ac090/native/ac090.c ql.c ql_test.c -lm -o ql_test && ./ql_test
 */
#include "ql.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void wr(ql *q, uint32_t a, uint32_t v, int size) { ac090_write(&q->cpu, a, v, size); }
static uint32_t rd(ql *q, uint32_t a, int size) { return ac090_read(&q->cpu, a, size); }

/* QDOS's ip_rdwr: each bit is a write of %11x0 to pc_ipcwr, then bit 7 of
 * pc_ipcrd is the IPC's bit (bit 6 clear: taken). Sends n bits of out, msb
 * first, and returns the n bits read back. */
static uint32_t link_bits(ql *q, uint32_t out, int n) {
    uint32_t in = 0;
    for (int i = n - 1; i >= 0; i--) {
        wr(q, 0x18003, 0x0c | ((out >> i & 1) << 1), 1);
        uint8_t s = (uint8_t)rd(q, 0x18020, 1);
        if (s & 0x40) { fails++; printf("FAIL: the IPC never took the bit\n"); }
        in = in << 1 | (s >> 7);
    }
    return in;
}

static uint32_t ipc(ql *q, int cmd, uint32_t param, int pbits, int rbits) {
    link_bits(q, (uint32_t)cmd, 4);
    if (pbits) link_bits(q, param, pbits);
    return rbits ? link_bits(q, (1u << rbits) - 1, rbits) : 0;
}

int main(void) {
    const char *rom_path = "ql_test_rom.bin";    /* in the current folder: Windows has no /tmp */
    uint8_t rom[12] = { 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x60, 0xfe, 0x4e, 0x71 };   /* SSP $30000, PC 8: BRA.S * */
    FILE *rf = fopen(rom_path, "wb");
    if (!rf || fwrite(rom, 1, sizeof rom, rf) != sizeof rom) return 2;
    fclose(rf);
    static ql q;
    if (ql_init(&q, rom_path, NULL, 640)) return 2;
    remove(rom_path);

    /* the map */
    CHECK(rd(&q, 0, 4) == 0x00030000u, "the reset SSP from the ROM");
    wr(&q, 0x00100, 0x1234, 2);
    CHECK(rd(&q, 0x00100, 2) == 0xffffu, "the ROM page is read only (%04x)", rd(&q, 0x00100, 2));
    wr(&q, 0x20000, 0xa5a5, 2);
    CHECK(rd(&q, 0x20000, 2) == 0xa5a5u, "screen RAM");
    wr(&q, 0xbfffe, 0x5a5a, 2);
    CHECK(rd(&q, 0xbfffe, 2) == 0x5a5au, "the last word of 640 KB");
    wr(&q, 0xc0000, 0x1111, 2);
    CHECK(rd(&q, 0xc0000, 2) == 0xffffu, "past the RAM fitted: open bus");
    CHECK(rd(&q, 0x120000, 2) == 0xa5a5u, "20 address lines: $120000 is $20000");

    /* the clock: seconds from 1961 */
    uint32_t t = rd(&q, 0x18000, 4);
    uint32_t want = (uint32_t)(time(NULL) + 283996800LL);
    CHECK(t >= want - 2 && t <= want + 2, "pc_clock %u, host %u", t, want);
    wr(&q, 0x18000, 0, 1);
    CHECK(rd(&q, 0x18000, 4) <= 1, "a write resets the clock");
    wr(&q, 0x18001, 0xee, 1);                           /* bit 4 clear: the msb up one */
    CHECK(rd(&q, 0x18000, 4) >> 24 == 1, "stepping the msb");

    /* the IPC: status, keyboard, keyrow */
    CHECK((ipc(&q, 1, 0, 0, 8) & 1) == 0, "no key: status bit 0 clear");
    int shift, a = ql_key_for_ascii('a', &shift);
    CHECK(a == 28 && !shift, "'a' is keyrow code 28 (%d)", a);
    ql_modifiers(&q, 1, 0, 0);
    ql_key(&q, a, 1);
    CHECK(ipc(&q, 1, 0, 0, 8) & 1, "a key: status bit 0 set");
    uint32_t head = ipc(&q, 8, 0, 0, 4);
    CHECK((head & 7) == 1 && (head & 8), "one key, still held (%x)", head);
    uint32_t keydef = link_bits(&q, 0xfff, 12);
    CHECK((keydef >> 8) == 4 && (keydef & 0xff) == 28, "SHIFT and 'a' (%03x)", keydef);
    { uint32_t r4 = ipc(&q, 9, 4, 4, 8); CHECK(r4 == 0x10, "KEYROW(4): 'a' is bit 4 (%02x)", r4); }
    CHECK(ipc(&q, 9, 7, 4, 8) == 0x01, "KEYROW(7): SHIFT");
    ql_key(&q, a, 0);
    ql_modifiers(&q, 0, 0, 0);
    CHECK((ipc(&q, 8, 0, 0, 4) & 0xf) == 0, "nothing more to read");
    CHECK(ipc(&q, 15, 0x5c, 8, 8) == 0x5c, "the test command echoes its byte");
    int sh; CHECK(ql_key_for_ascii('"', &sh) == 47 && sh, "'\"' is SHIFT and the quote key");
    CHECK(ql_key_for_ascii('\n', &sh) == QLK_ENTER, "ENTER");

    /* the ZX8301 */
    static uint32_t px[QL_SCREEN_W * QL_SCREEN_H];
    memset(q.ram, 0, 0x8000);
    q.ram[0] = 0x80; q.ram[1] = 0x40;                   /* mode 4: pixel 0 green, pixel 1 red */
    q.ram[2] = 0x01; q.ram[3] = 0x01;                   /* pixel 15 white */
    wr(&q, 0x18063, 0x00, 1);
    ql_render(&q, px);
    CHECK(px[0] == 0x00ff00 && px[1] == 0xff0000 && px[2] == 0 && px[15] == 0xffffff, "mode 4 (%06x %06x %06x)", px[0], px[1], px[15]);
    wr(&q, 0x18063, 0x08, 1);                           /* mode 8 */
    q.ram[0] = 0x80; q.ram[1] = 0x40;                   /* pixel 0: G, B: cyan */
    q.ram[2] = 0x00; q.ram[3] = 0x00;
    ql_render(&q, px);
    CHECK(ql_mode8(&q) && px[0] == 0x00ffff && px[1] == 0x00ffff && px[2] == 0, "mode 8, two wide (%06x)", px[0]);
    wr(&q, 0x18063, 0x80, 1);                           /* scr1 at $28000 */
    q.ram[0x8000] = 0x80;
    ql_render(&q, px);
    CHECK(px[0] == 0x00ff00, "screen 1");
    wr(&q, 0x18063, 0x02, 1);
    ql_render(&q, px);
    CHECK(px[0] == 0, "blanked");

    /* interrupts: the frame each 20 ms, at level 2; a gap only with a drive selected */
    wr(&q, 0x18021, 0x1f, 1);
    ql_run(&q, q.cpu.clock + QL_FRAME_CLOCKS + 10);
    CHECK(rd(&q, 0x18021, 1) & 0x08, "a frame interrupt");
    CHECK(q.cpu.irq == 2, "on level 2 (%d)", q.cpu.irq);
    wr(&q, 0x18021, 0x1f, 1);
    CHECK(q.cpu.irq == 0, "cleared");
    wr(&q, 0x18021, 0x20, 1);                           /* the gap mask on */
    wr(&q, 0x18020, 0x03, 1); wr(&q, 0x18020, 0x01, 1);  /* select mdv1: sel with sclk falling */
    ql_run(&q, q.cpu.clock + 3 * QL_FRAME_CLOCKS);
    CHECK(rd(&q, 0x18021, 1) & 0x01, "an empty drive's gap");
    CHECK((rd(&q, 0x18021, 1) & 0x40) == 0, "bit 6 clear while a drive runs");

    /* microdrives: an image becomes a tape and back unchanged */
    static uint8_t img[QL_MDV_IMAGE], back[QL_MDV_IMAGE];
    for (int r = 0; r < 3; r++) {
        uint8_t *rec = img + r * QL_MDV_SECTOR;
        rec[10] = rec[11] = 0xff;                       /* the header's preamble */
        rec[12] = 0xff; rec[13] = (uint8_t)(2 - r);     /* flag, sector number */
        memcpy(rec + 14, "CASE      ", 10);
        uint8_t *blk = rec + 28;
        blk[10] = blk[11] = 0xff;
        for (int i = 12; i < 658; i++) blk[i] = (uint8_t)(i * 7 + r);
        for (int i = 16; i < 22; i++) blk[i] = 0;        /* the data's own preamble */
        blk[22] = blk[23] = 0xff;
    }
    uint32_t len;
    uint16_t *tape = ql_mdv_from_image(img, QL_MDV_IMAGE, &len);
    CHECK(tape && len >= 180000, "a tape of %u cells", len);
    CHECK(tape && ql_mdv_to_image(tape, len, back) == 3, "three sectors back from the tape");
    CHECK(!memcmp(img, back, QL_MDV_IMAGE), "the image comes back as it went in");
    free(tape);

    /* ... and QDOS's way of reading it: select mdv1_, wait for the gap's end,
     * reset the PLL, take each byte while pc..rxrd is set */
    const char *mdv_path = "ql_test.mdv";
    FILE *mf = fopen(mdv_path, "wb");
    if (!mf || fwrite(img, 1, QL_MDV_IMAGE, mf) != QL_MDV_IMAGE) return 2;
    fclose(mf);
    CHECK(ql_mdv_insert(&q, 0, mdv_path, 1) == 0, "a cartridge in mdv1_");
    wr(&q, 0x18021, 0x3f, 1);                           /* clear all, the gap interrupt on */
    wr(&q, 0x18002, 0x10, 1);                           /* microdrive mode */
    wr(&q, 0x18020, 0x03, 1); wr(&q, 0x18020, 0x01, 1); wr(&q, 0x18020, 0x02, 1);
    int got_gap = 0, signal = 0;
    for (int i = 0; i < 2000 && !signal; i++) {         /* the first gap, then its end */
        ql_run(&q, q.cpu.clock + 100);
        uint8_t st = (uint8_t)rd(&q, 0x18020, 1);
        if (st & 0x08) got_gap = 1;
        else if (got_gap) signal = 1;
    }
    CHECK(signal, "a gap, then the signal of a block");
    wr(&q, 0x18020, 0x02, 1);                           /* reset the PLL */
    uint8_t hdr[16];
    int n = 0;
    for (int i = 0; i < 20000 && n < 16; i++) {
        ql_run(&q, q.cpu.clock + 50);
        if (rd(&q, 0x18020, 1) & 0x04) { hdr[n] = (uint8_t)rd(&q, 0x18022u + (uint32_t)(n & 1), 1); n++; }
    }
    CHECK(n == 16 && hdr[0] == 0xff && hdr[1] == 2 && !memcmp(hdr + 2, "CASE", 4), "sector 2's header (%d bytes, %02x %02x)", n, hdr[0], hdr[1]);
    for (int i = 0; i < 2000 && !(rd(&q, 0x18021, 1) & 0x01); i++) ql_run(&q, q.cpu.clock + 100);
    CHECK(rd(&q, 0x18021, 1) & 0x01, "the gap after the header interrupts");
    ql_mdv_eject(&q, 0);
    remove(mdv_path);
    /* serial input: held until ser1 is open, then in the IPC's buffer at the baud rate */
    for (const char *p = "QL!"; *p; p++) ql_serial_in(&q, 0, (uint8_t)*p);
    ql_run(&q, q.cpu.clock + QL_FRAME_CLOCKS);
    CHECK(!(ipc(&q, 1, 0, 0, 8) & 0x10), "ser1 closed: nothing comes in");
    ipc(&q, 2, 0, 0, 0);                                /* open ser1 */
    ipc(&q, 13, 1, 4, 0);                               /* 9600 baud */
    ql_run(&q, q.cpu.clock + QL_FRAME_CLOCKS);
    for (int i = 0; i < 4; i++) { ipc(&q, 1, 0, 0, 8); ql_run(&q, q.cpu.clock + QL_FRAME_CLOCKS / 8); }
    CHECK(ipc(&q, 1, 0, 0, 8) & 0x10, "input waiting on ser1");
    uint32_t cnt = ipc(&q, 6, 0, 0, 8);
    char got[4] = { 0 };
    for (uint32_t i = 0; i < cnt && i < 3; i++) got[i] = (char)link_bits(&q, 0xff, 8);
    CHECK(cnt == 3 && !strcmp(got, "QL!"), "read ser1: %u bytes, \"%s\"", cnt, got);
    CHECK(!(ipc(&q, 1, 0, 0, 8) & 0x10), "and nothing more");

    /* a saved state: the same machine, run on from it, does the same again */
    {
        const char *st = "ql_test_state.bin", *why = "";
        wr(&q, 0x30000, 0x4242, 2);
        CHECK(ql_save_state(&q, st, &why) == 0, "save a state (%s)", why);
        ql_run(&q, q.cpu.clock + QL_FRAME_CLOCKS);
        uint32_t pc1 = q.cpu.pc; uint64_t clk1 = q.cpu.clock;
        wr(&q, 0x30000, 0x1111, 2);
        CHECK(ql_load_state(&q, st, &why) == 0, "load it (%s)", why);
        CHECK(rd(&q, 0x30000, 2) == 0x4242, "RAM as it was");
        ql_run(&q, q.cpu.clock + QL_FRAME_CLOCKS);
        CHECK(q.cpu.pc == pc1 && q.cpu.clock - clk1 + 64 < 128, "ran on the same (clock %llu %llu)", (unsigned long long)q.cpu.clock, (unsigned long long)clk1);
        FILE *f = fopen(st, "r+b"); fseek(f, 24, SEEK_SET); fputc(0x55, f); fclose(f);
        CHECK(ql_load_state(&q, st, &why) != 0 && strstr(why, "ROM"), "another ROM's state is refused (%s)", why);
        CHECK(ql_load_state(&q, "no_such_state", &why) != 0, "no state");
        remove(st);
    }

    printf(fails ? "ql_test: %d failed\n" : "ql_test: all passed\n", fails);
    ql_free(&q);
    return fails != 0;
}
