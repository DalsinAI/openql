/*
 * The floppy controller's cases (src/superql/fdc.h), driven through its
 * ports as a driver without DMA drives an 82077: reset and its four sense
 * interrupts, SPECIFY, RECALIBRATE and SEEK, READ DATA (one side and both),
 * WRITE DATA, FORMAT TRACK, READ ID, SENSE DRIVE STATUS, VERSION, the disk
 * change line, a drive with no disk and a write-protected one.
 */
#include "superql/fdc.h"
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static sql_fdc f;

static uint8_t msr(void) { return fdc_read(&f, 4); }

static void out(const uint8_t *b, int n) {
    for (int i = 0; i < n; i++) {
        CHECK((msr() & 0xc0) == 0x80, "byte %d of command %02x: MSR %02x, not ready for a command byte", i, b[0], msr());
        fdc_write(&f, 5, b[i]);
    }
}

static int in_result(uint8_t *r, int max) {
    int n = 0;
    while ((msr() & 0xd0) == 0xd0 && n < max) r[n++] = fdc_read(&f, 5);
    return n;
}

static void sense(uint8_t *st0, uint8_t *pcn) {
    uint8_t c = 0x08, r[2];
    out(&c, 1);
    int n = in_result(r, 2);
    CHECK(n == 2, "SENSE INTERRUPT gave %d bytes", n);
    *st0 = r[0]; *pcn = r[1];
}

/* The byte at (track, side, sector, i) in a test image. */
static uint8_t pat(int c, int h, int r, int i) { return (uint8_t)(c * 7 + h * 13 + r * 31 + i); }

static void make_image(const char *path, int spt) {
    FILE *fp = fopen(path, "wb");
    for (int c = 0; c < 80; c++)
        for (int h = 0; h < 2; h++)
            for (int r = 1; r <= spt; r++)
                for (int i = 0; i < 512; i++) fputc(pat(c, h, r, i), fp);
    fclose(fp);
}

int main(void) {
    const char *dd = "fdc_test_dd.img";
    make_image(dd, 9);

    memset(&f, 0, sizeof f);
    fdc_reset(&f);
    CHECK(msr() == 0, "held in reset, MSR is %02x", msr());
    CHECK(fdc_insert(&f, 0, dd, 0) == 0, "insert");
    fdc_write(&f, 2, 0x1c);                       /* out of reset, IRQ/DMA gate, motor 0, drive 0 */
    CHECK(msr() == 0x80, "after reset MSR %02x", msr());
    for (int i = 0; i < 4; i++) {
        uint8_t st0, pcn;
        sense(&st0, &pcn);
        CHECK(st0 == (0xc0 | i), "reset status %d: ST0 %02x", i, st0);
    }
    { uint8_t st0, pcn; uint8_t c = 0x08, r[2]; out(&c, 1); int n = in_result(r, 2); (void)st0; (void)pcn;
      CHECK(n == 1 && r[0] == 0x80, "a fifth SENSE INTERRUPT: invalid (%d bytes, %02x)", n, r[0]); }

    { uint8_t c[] = { 0x10 }, r[1]; out(c, 1); CHECK(in_result(r, 1) == 1 && r[0] == 0x90, "VERSION %02x", r[0]); }
    { uint8_t c[] = { 0x03, 0xdf, 0x03 }; out(c, 3); CHECK(msr() == 0x80, "SPECIFY leaves MSR %02x", msr()); }

    CHECK(fdc_read(&f, 7) & 0x80, "a new disk: the change line is set");
    { uint8_t c[] = { 0x07, 0x00 }; out(c, 2); uint8_t st0, pcn; sense(&st0, &pcn);
      CHECK(st0 == 0x20 && pcn == 0, "RECALIBRATE: ST0 %02x PCN %d", st0, pcn); }
    { uint8_t c[] = { 0x0f, 0x00, 5 }; out(c, 3); uint8_t st0, pcn; sense(&st0, &pcn);
      CHECK(st0 == 0x20 && pcn == 5, "SEEK: ST0 %02x PCN %d", st0, pcn); }
    CHECK(!(fdc_read(&f, 7) & 0x80), "after a step the change line is clear");

    { uint8_t c[] = { 0x04, 0x00 }, r[1]; out(c, 2); in_result(r, 1);
      CHECK((r[0] & 0x60) == 0x20 && !(r[0] & 0x10), "SENSE DRIVE STATUS on track 5: ST3 %02x", r[0]); }

    /* READ DATA, track 5 side 1, sectors 3 to 4 */
    {
        uint8_t c[] = { 0x46, 0x04, 5, 1, 3, 2, 4, 0x1b, 0xff };
        out(c, 9);
        int ok = 1, n = 0;
        while ((msr() & 0xf0) == 0xf0) {
            uint8_t v = fdc_read(&f, 5);
            if (v != pat(5, 1, 3 + n / 512, n % 512)) ok = 0;
            n++;
        }
        CHECK(n == 1024 && ok, "READ DATA gave %d bytes%s", n, ok ? "" : ", wrong");
        uint8_t r[7]; int k = in_result(r, 7);
        CHECK(k == 7 && r[0] == 0x44 && r[1] == 0x80 && r[3] == 6 && r[5] == 1, "READ DATA result %d: %02x %02x c%d r%d", k, r[0], r[1], r[3], r[5]);
    }
    /* multi-track: side 0 sector 9, then side 1 sector 1 */
    {
        uint8_t c[] = { 0xc6, 0x00, 5, 0, 9, 2, 9, 0x1b, 0xff };
        out(c, 9);
        int ok = 1, n = 0;
        while ((msr() & 0xf0) == 0xf0) {
            uint8_t v = fdc_read(&f, 5);
            uint8_t want = n < 512 ? pat(5, 0, 9, n) : pat(5, 1, 1 + (n - 512) / 512, n % 512);
            if (v != want) ok = 0;
            n++;
        }
        CHECK(n == 512 * 10 && ok, "a multi-track READ gave %d bytes%s", n, ok ? "" : ", wrong");
        uint8_t r[7]; in_result(r, 7);
    }
    /* WRITE DATA, then read it back */
    {
        uint8_t c[] = { 0x45, 0x00, 10, 0, 2, 2, 2, 0x1b, 0xff };
        out(c, 9);
        int n = 0;
        while ((msr() & 0xf0) == 0xb0) { fdc_write(&f, 5, (uint8_t)(0xa5 ^ n)); n++; }
        uint8_t r[7]; int k = in_result(r, 7);
        CHECK(n == 512 && k == 7 && r[1] == 0x80, "WRITE DATA took %d bytes, result ST1 %02x", n, r[1]);
        uint8_t rd[] = { 0x46, 0x00, 10, 0, 2, 2, 2, 0x1b, 0xff };
        out(rd, 9);
        int ok = 1; n = 0;
        while ((msr() & 0xf0) == 0xf0) { if (fdc_read(&f, 5) != (uint8_t)(0xa5 ^ n)) ok = 0; n++; }
        in_result(r, 7);
        CHECK(n == 512 && ok, "the written sector reads back");
    }
    /* FORMAT TRACK 20 side 0 with $E5, then READ ID and a read */
    {
        uint8_t s1[] = { 0x0f, 0x00, 20 }; out(s1, 3); uint8_t st0, pcn; sense(&st0, &pcn);
        uint8_t c[] = { 0x4d, 0x00, 2, 9, 0x54, 0xe5 };
        out(c, 6);
        for (int r = 1; r <= 9; r++) { uint8_t id[4] = { 20, 0, (uint8_t)r, 2 };
            for (int i = 0; i < 4; i++) { CHECK((msr() & 0xf0) == 0xb0, "FORMAT wants an ID byte"); fdc_write(&f, 5, id[i]); } }
        uint8_t res[7]; int k = in_result(res, 7);
        CHECK(k == 7 && res[0] == 0x00, "FORMAT result ST0 %02x", res[0]);
        uint8_t ri[] = { 0x4a, 0x00 }; out(ri, 2); k = in_result(res, 7);
        CHECK(k == 7 && res[3] == 20 && res[5] == 1 && res[6] == 2, "READ ID: c%d r%d n%d", res[3], res[5], res[6]);
        uint8_t rd[] = { 0x46, 0x00, 20, 0, 5, 2, 5, 0x1b, 0xff };
        out(rd, 9);
        int ok = 1, n = 0;
        while ((msr() & 0xf0) == 0xf0) { if (fdc_read(&f, 5) != 0xe5) ok = 0; n++; }
        in_result(res, 7);
        CHECK(n == 512 && ok, "a formatted sector is $E5");
    }
    /* a sector past the track's: no data */
    {
        uint8_t c[] = { 0x46, 0x00, 5, 0, 12, 2, 12, 0x1b, 0xff }, r[7];
        out(c, 9);
        int k = in_result(r, 7);
        CHECK(k == 7 && r[0] == 0x40 && r[1] == 0x04, "sector 12 on a DD disk: ST0 %02x ST1 %02x", r[0], r[1]);
    }
    /* FLP2_ with no disk: not ready */
    {
        uint8_t c[] = { 0x46, 0x01, 0, 0, 1, 2, 1, 0x1b, 0xff }, r[7];
        out(c, 9);
        int k = in_result(r, 7);
        CHECK(k == 7 && r[0] == 0x49, "no disk: ST0 %02x", r[0]);
    }
    /* a write-protected disk */
    {
        fdc_eject(&f, 0);
        CHECK(fdc_insert(&f, 0, dd, 1) == 0, "insert read only");
        uint8_t c[] = { 0x45, 0x00, 1, 0, 1, 2, 1, 0x1b, 0xff }, r[7];
        out(c, 9);
        int k = in_result(r, 7);
        CHECK(k == 7 && r[0] == 0x40 && r[1] == 0x02, "write protected: ST0 %02x ST1 %02x", r[0], r[1]);
        uint8_t s[] = { 0x04, 0x00 }; out(s, 2); k = in_result(r, 1);
        CHECK(r[0] & 0x40, "SENSE DRIVE STATUS says write protected (%02x)", r[0]);
    }
    /* an image of the wrong size is refused */
    {
        const char *bad = "fdc_test_bad.img";
        FILE *b = fopen(bad, "wb"); fputc('x', b); fclose(b);
        CHECK(fdc_insert(&f, 1, bad, 0) == -1, "a 1-byte image is refused");
        remove(bad);
    }
    fdc_eject(&f, 0);
    remove(dd);
    printf("fdc_test: %s (%d failed)\n", fails ? "FAILED" : "all passed", fails);
    return fails != 0;
}
