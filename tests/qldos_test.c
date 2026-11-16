/* qldos.c: DOS1_'s open, input/output and close, as the driver's line A
 * words call them, on a QL with a 6-byte ROM (no QDOS needed). */
#include "ql/qldos.h"
#include "ql/qldos_rom.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include <direct.h>
#define mkdir1(p) _mkdir(p)
#else
#define mkdir1(p) mkdir(p, 0755)
#endif

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static ql q;
static uint32_t at_open, at_io, at_close;   /* the line A words in the ROM port */
enum { BLK = 0x30000, PHYS = 0x31000, BUF = 0x32000 };

static void put(const char *path, const void *p, size_t n) { FILE *f = fopen(path, "wb"); fwrite(p, 1, n, f); fclose(f); }
static long get(const char *path, char *buf, size_t max) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    long n = (long)fread(buf, 1, max, f);
    fclose(f);
    return n;
}
static void wr(uint32_t a, uint32_t v, int size) { ac090_write(&q.cpu, a, v, size); }
static uint32_t rd(uint32_t a, int size) { return ac090_read(&q.cpu, a, size); }
static void wstr(uint32_t a, const char *s) { size_t n = strlen(s); wr(a, (uint32_t)n, 2); for (size_t i = 0; i < n; i++) wr(a + 2 + (uint32_t)i, (uint8_t)s[i], 1); }

static int32_t call(uint32_t pc) {
    CHECK(q.cpu.guard_hook(&q.cpu, 10, pc) == 1, "the driver's call at %x taken", pc);
    CHECK(q.cpu.pc == pc + 2, "on past the word");
    return (int32_t)q.cpu.d[0];
}

/* io_fopen's part: the channel block with the key and the name, then the driver's open. */
static int32_t dos_open(uint32_t blk, const char *name, int key) {
    for (uint32_t i = 0; i < 0xa0; i++) wr(blk + i, 0, 1);
    wr(blk + 0x1c, (uint8_t)key, 1);
    wstr(blk + 0x32, name);
    q.cpu.a[0] = blk; q.cpu.a[1] = PHYS; q.cpu.d[3] = (uint32_t)key;
    return call(at_open);
}
static int32_t dos_io(uint32_t blk, int op, uint32_t d1, uint32_t d2, uint32_t a1) {
    q.cpu.a[0] = blk; q.cpu.d[0] = (uint32_t)op; q.cpu.d[1] = d1; q.cpu.d[2] = d2; q.cpu.a[1] = a1;
    return call(at_io);
}
static void dos_close(uint32_t blk) { q.cpu.a[0] = blk; call(at_close); }

int main(void) {
    const char *rom_path = "qldos_test_rom.bin";
    uint8_t rom[12] = { 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x60, 0xfe, 0x4e, 0x71 };
    put(rom_path, rom, sizeof rom);
    if (ql_init(&q, rom_path, NULL, 640)) return 2;
    remove(rom_path);
    CHECK(ql_dos_install(&q) == 0 && ql_dos_installed(&q), "the driver goes in the empty ROM port");
    CHECK(rd(0xc000, 4) == 0x4afb0001u, "a ROM header at $C000");
    for (uint32_t i = 0; i + 1 < sizeof qldos_rom; i += 2) {
        unsigned w = (unsigned)qldos_rom[i] << 8 | qldos_rom[i + 1];
        if (w == 0xa0d1) at_open = 0xc000 + i;
        if (w == 0xa0d2) at_io = 0xc000 + i;
        if (w == 0xa0d3) at_close = 0xc000 + i;
    }
    CHECK(at_open && at_io && at_close, "the driver's three calls");
    wr(PHYS + 0x14, 1, 1);                                        /* drive 1 */

    mkdir1("dos_in"); mkdir1("dos_in/progs");
    put("dos_in/hello.bas", "10 PRINT 42\n", 12);
    uint8_t exe[16] = { 0x60, 0xfe, 0, 0, 0, 0, 0, 0, 'X', 'T', 'c', 'c', 0, 0, 0x10, 0 };
    put("dos_in/progs/run.exe", exe, sizeof exe);
    char why[200];
    CHECK(dos_open(BLK, "hello_bas", 1) == -7, "no folder yet: not found");
    CHECK(ql_dos_attach(&q, "no_such_folder", why, sizeof why) == -1, "not a folder");
    CHECK(ql_dos_attach(&q, "dos_in", why, sizeof why) == 0, "attached (%s)", why);

    /* reading: a host file under its QL name, in any case */
    CHECK(dos_open(BLK, "HELLO_BAS", 1) == 0, "hello.bas opens as HELLO_BAS");
    CHECK(dos_io(BLK, 0x47, 0, 14, BUF) == 0 && rd(BUF, 4) == 12 && rd(BUF + 5, 1) == 0, "its header: 12 bytes, type 0");
    CHECK(dos_io(BLK, 2, 0, 100, BUF) == 0 && q.cpu.d[1] == 12 && q.cpu.a[1] == BUF + 12, "io.fline reads the line");
    char line[13] = { 0 };
    for (int i = 0; i < 12; i++) line[i] = (char)rd(BUF + (uint32_t)i, 1);
    CHECK(!strcmp(line, "10 PRINT 42\n"), "the line's bytes (%s)", line);
    CHECK(dos_io(BLK, 1, 0, 0, 0) == -10, "then the end of the file");
    CHECK(dos_io(BLK, 0x42, 3, 0, 0) == 0 && q.cpu.d[1] == 3, "fs.posab");
    CHECK(dos_io(BLK, 1, 0, 0, 0) == 0 && (q.cpu.d[1] & 0xff) == 'P', "io.fbyte from there");
    CHECK(dos_io(BLK, 7, 0, 1, BUF) == -20, "a shared channel is read only");
    dos_close(BLK);
    CHECK(dos_open(BLK, "progs_run_exe", 0) == 0, "a file in a folder");
    CHECK(dos_io(BLK, 0x47, 0, 64, BUF) == 0 && rd(BUF, 4) == 8 && rd(BUF + 5, 1) == 1 && rd(BUF + 6, 4) == 0x1000,
          "an executable: 8 bytes, type 1, its data space");
    dos_close(BLK);
    CHECK(dos_open(BLK, "nothing_here", 0) == -7, "not found");
    CHECK(dos_open(BLK, "hello_bas", 2) == -8, "a new file that exists already");

    /* writing: whole on close, the type and data space at the end */
    CHECK(dos_open(BLK, "progs_new_exe", 2) == 0, "a new file");
    const char *text = "abcdef";
    for (int i = 0; i < 6; i++) wr(BUF + (uint32_t)i, (uint8_t)text[i], 1);
    CHECK(dos_io(BLK, 0x49, 0, 6, BUF) == 0 && q.cpu.d[1] == 6, "fs.save");
    wr(BUF + 5, 1, 1); wr(BUF + 6, 0x2000, 4);
    CHECK(dos_io(BLK, 0x46, 0, 14, BUF) == 0, "fs.heads: an executable");
    struct stat st;
    CHECK(stat("dos_in/progs/new_exe", &st) != 0, "nothing on the host before the close");
    dos_close(BLK);
    char back[100];
    CHECK(get("dos_in/progs/new_exe", back, sizeof back) == 14 && !memcmp(back, "abcdef", 6) && !memcmp(back + 6, "XTcc", 4) &&
          (uint8_t)back[12] == 0x20, "in progs/, with XTcc and the data space");
    CHECK(stat("dos_in/progs/new_exe.openql-new", &st) != 0, "no file left half way");
    CHECK(dos_open(BLK, "hello_bas", 3) == 0 && dos_io(BLK, 5, 'X', 0, 0) == 0, "overwrite");
    dos_close(BLK);
    CHECK(get("dos_in/hello.bas", back, sizeof back) == 1 && back[0] == 'X', "hello.bas keeps its host name");

    /* a directory: every file the name starts with, as 64-byte entries */
    CHECK(dos_open(BLK, "progs_", 4) == 0, "a directory");
    int n = 0, saw_new = 0;
    while (dos_io(BLK, 3, 0, 64, BUF) == 0) {
        n++;
        char nm[40] = { 0 };
        unsigned l = rd(BUF + 14, 2);
        for (unsigned i = 0; i < l && i < 36; i++) nm[i] = (char)rd(BUF + 16 + i, 1);
        if (!strcmp(nm, "progs_new_exe")) saw_new = rd(BUF, 4) == 6 + 64 && rd(BUF + 5, 1) == 1;
    }
    CHECK(n == 2 && saw_new, "two entries in progs_ (%d), new_exe's length with its header", n);
    CHECK(dos_io(BLK, 7, 0, 1, BUF) == -20, "a directory is read only");
    dos_close(BLK);

    /* rename and delete: nothing is lost on the host */
    CHECK(dos_open(BLK, "progs_new_exe", 0) == 0, "open to rename");
    wstr(BUF, "dos1_progs_renamed");
    CHECK(dos_io(BLK, 0x4a, 0, 0, BUF) == 0, "fs.renam");
    dos_close(BLK);
    CHECK(stat("dos_in/progs/renamed", &st) == 0 && stat("dos_in/progs/new_exe", &st) != 0, "renamed on the host");
    CHECK(dos_open(BLK, "progs_renamed", -1) == 0, "delete");
    CHECK(stat("dos_in/progs/renamed", &st) != 0 && stat("dos_in/.openql-deleted/progs_renamed", &st) == 0, "kept in .openql-deleted");
    CHECK(dos_io(0x33000, 1, 0, 0, 0) == -6, "a channel the driver never opened");

    ql_dos_detach(&q);
    CHECK(dos_open(BLK, "hello_bas", 1) == -7, "detached: not found");
    ql_free(&q);

    remove("dos_in/hello.bas"); remove("dos_in/progs/run.exe"); remove("dos_in/.openql-deleted/progs_renamed");
    rmdir("dos_in/.openql-deleted"); rmdir("dos_in/progs"); rmdir("dos_in");
    printf(fails ? "qldos_test: %d failed\n" : "qldos_test: all passed\n", fails);
    return fails != 0;
}
