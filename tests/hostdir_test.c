/* hostdir.c: a folder into a QLWA image and back again. */
#include "hostdir.h"
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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
    CHECK(hostdir_sync("hd_test.img", "hd_out", why, sizeof why) == 3, "three files written (%s)", why);
    static char back[200000];
    CHECK(get("hd_out/hello_bas", back, sizeof back) == 12 && !memcmp(back, "10 PRINT 42\n", 12), "hello.bas came back as hello_bas");
    CHECK(get("hd_out/progs/data_bin", back, sizeof back) == (long)sizeof big && !memcmp(back, big, sizeof big), "a file in a folder");
    CHECK(get("hd_out/progs/run_exe", back, sizeof back) == 16 && !memcmp(back, exe, 16), "an executable keeps its data space");
    CHECK(hostdir_sync("hd_test.img", "hd_out", why, sizeof why) == 0, "the same again: nothing written");
    /* back into the folder it came from: the host's names are kept, nothing changes */
    CHECK(hostdir_sync("hd_test.img", "hd_in", why, sizeof why) == 0, "into its own folder: nothing written");
    CHECK(hostdir_build("no_such_folder", "hd_test.img", NULL, why, sizeof why) == -1, "no folder");

    remove("hd_in/hello.bas"); remove("hd_in/progs/data.bin"); remove("hd_in/progs/run.exe"); rmdir("hd_in/progs"); rmdir("hd_in");
    remove("hd_out/hello_bas"); remove("hd_out/progs/data_bin"); remove("hd_out/progs/run_exe"); rmdir("hd_out/progs"); rmdir("hd_out");
    remove("hd_test.img");
    printf(fails ? "hostdir_test: %d failed\n" : "hostdir_test: all passed\n", fails);
    return fails != 0;
}
