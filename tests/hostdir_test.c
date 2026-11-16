/* hostdir.c: a folder into a QLWA image and back again. */
#include "hostdir.h"
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef _WIN32
#include <direct.h>
#define mkdir1(p) _mkdir(p)
#else
#define mkdir1(p) mkdir(p, 0755)
#endif

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void put(const char *path, const void *p, size_t n) { FILE *f = fopen(path, "wb"); fwrite(p, 1, n, f); fclose(f); }
static long get(const char *path, char *buf, size_t max) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    long n = (long)fread(buf, 1, max, f);
    fclose(f);
    return n;
}

int main(void) {
    char why[200] = "";
    int skipped = -1;
    mkdir1("hd_in"); mkdir1("hd_in/progs"); mkdir1("hd_out");
    put("hd_in/hello.bas", "10 PRINT 42\n", 12);
    static char big[100000];
    for (size_t i = 0; i < sizeof big; i++) big[i] = (char)(i * 7);
    put("hd_in/progs/data.bin", big, sizeof big);
    uint8_t exe[16] = { 0x60, 0xfe, 0, 0, 0, 0, 0, 0, 'X', 'T', 'c', 'c', 0, 0, 0x10, 0 };
    put("hd_in/progs/run.exe", exe, sizeof exe);
    CHECK(hostdir_build("hd_in", "hd_test.img", &skipped, why, sizeof why) == 0 && skipped == 0, "built (%s, %d skipped)", why, skipped);
    struct stat st;
    CHECK(stat("hd_test.img", &st) == 0 && st.st_size == 2949120, "a 2.88 MB image");

    /* the QL writes a new file: here, by hand, as SMSQ/E would leave it is not
     * tried; the round trip of what was there is */
    CHECK(hostdir_sync("hd_test.img", "hd_out", 0, NULL, why, sizeof why) == 3, "three files written (%s)", why);
    static char back[200000];
    CHECK(get("hd_out/hello_bas", back, sizeof back) == 12 && !memcmp(back, "10 PRINT 42\n", 12), "hello.bas came back as hello_bas");
    CHECK(get("hd_out/progs/data_bin", back, sizeof back) == (long)sizeof big && !memcmp(back, big, sizeof big), "a file in a folder");
    CHECK(get("hd_out/progs/run_exe", back, sizeof back) == 16 && !memcmp(back, exe, 16), "an executable keeps its data space");
    CHECK(hostdir_sync("hd_test.img", "hd_out", 0, NULL, why, sizeof why) == 0, "the same again: nothing written");
    /* back into the folder it came from: the host's names are kept, nothing changes */
    CHECK(hostdir_sync("hd_test.img", "hd_in", 0, NULL, why, sizeof why) == 0, "into its own folder: nothing written");
    CHECK(hostdir_build("no_such_folder", "hd_test.img", NULL, why, sizeof why) == -1, "no folder");

    /* a hard disk (WIN2_): 64 MB at least, 16 heads and 63 sectors, with a
     * file larger than any floppy */
    static char huge[4 << 20];
    for (size_t i = 0; i < sizeof huge; i++) huge[i] = (char)(i * 13 + (i >> 12));
    put("hd_in/huge.bin", huge, sizeof huge);
    CHECK(hostdir_build_hd("hd_in", "hd_test.win", &skipped, why, sizeof why) == 0 && skipped == 0, "a hard disk (%s, %d skipped)", why, skipped);
    CHECK(stat("hd_test.win", &st) == 0 && st.st_size >= 64L * 1024 * 1024 && st.st_size % (16 * 63 * 512) == 0,
          "64 MB or more, whole cylinders (%ld)", (long)st.st_size);
    uint8_t h[64];
    CHECK(get("hd_test.win", (char *)h, 64) == 64 && !memcmp(h, "QLWA", 4) && h[0x25] == 63 && h[0x27] == 16,
          "QLWA, 63 sectors, 16 heads");
    unsigned sctg = h[0x22] << 8 | h[0x23], ngrp = h[0x2a] << 8 | h[0x2b];
    CHECK(sctg >= 4 && ngrp * sctg <= st.st_size / 512 && ngrp > 30000, "groups of %u sectors, %u of them", sctg, ngrp);
    mkdir1("hd_out2");
    CHECK(hostdir_sync("hd_test.win", "hd_out2", 0, NULL, why, sizeof why) == 4, "four files back (%s)", why);
    static char back2[5 << 20];
    CHECK(get("hd_out2/huge_bin", back2, sizeof back2) == (long)sizeof huge && !memcmp(back2, huge, sizeof huge), "4 MB intact");

    /* changed on both sides: the host's stays, the QL's goes beside it */
    time_t built = time(NULL) - 10;
    put("hd_out2/hello_bas", "HOST\n", 5);
    int kept = -1;
    CHECK(hostdir_sync("hd_test.win", "hd_out2", built, &kept, why, sizeof why) == 1 && kept == 1, "one kept (%d, %s)", kept, why);
    CHECK(get("hd_out2/hello_bas", back, sizeof back) == 5 && !memcmp(back, "HOST\n", 5), "the host's change stays");
    CHECK(get("hd_out2/hello_bas.from-ql", back, sizeof back) == 12 && !memcmp(back, "10 PRINT 42\n", 12), "the QL's beside it");
    CHECK(stat("hd_out2/hello_bas.openql-new", &st) != 0, "nothing half written left");
    remove("hd_out2/hello_bas"); remove("hd_out2/hello_bas.from-ql"); remove("hd_out2/huge_bin");
    remove("hd_out2/progs/data_bin"); remove("hd_out2/progs/run_exe"); rmdir("hd_out2/progs"); rmdir("hd_out2");
    remove("hd_in/huge.bin"); remove("hd_test.win");

    remove("hd_in/hello.bas"); remove("hd_in/progs/data.bin"); remove("hd_in/progs/run.exe"); rmdir("hd_in/progs"); rmdir("hd_in");
    remove("hd_out/hello_bas"); remove("hd_out/progs/data_bin"); remove("hd_out/progs/run_exe"); rmdir("hd_out/progs"); rmdir("hd_out");
    remove("hd_test.img");
    printf(fails ? "hostdir_test: %d failed\n" : "hostdir_test: all passed\n", fails);
    return fails != 0;
}
