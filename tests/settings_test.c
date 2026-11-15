/* settings.c: the settings file round trip, and the folders beside the ROMs. */
#include "settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
    const char *path = "settings_test.cfg";
    settings a, b;
    settings_default(&a);
    CHECK(a.machine == SET_SUPERQL && a.ram_sql_mb == 128 && a.ram_ql_kb == 640 && a.sound && a.launcher, "defaults");
    CHECK(settings_load(&b, "no_such_settings.cfg") == -1 && !b.seen && b.machine == SET_SUPERQL, "no file: defaults");
    a.machine = SET_QL; a.ram_ql_kb = 896; a.scale = SCALE_INTEGER; a.speed = SPEED_MAX; a.fullscreen = 1;
    snprintf(a.win1, sizeof a.win1, "/disks/with = signs/QLE.WIN");
    snprintf(a.mdv[1], sizeof a.mdv[1], "C:\\QL\\games.mdv");
    CHECK(settings_save(&a, path) == 0, "saved");
    CHECK(settings_load(&b, path) == 0 && b.seen, "loaded");
    CHECK(b.machine == SET_QL && b.ram_ql_kb == 896 && b.scale == SCALE_INTEGER && b.speed == SPEED_MAX && b.fullscreen, "numbers");
    CHECK(!strcmp(b.win1, a.win1) && !strcmp(b.mdv[1], a.mdv[1]) && !b.flp[0][0], "paths (%s, %s)", b.win1, b.mdv[1]);
    FILE *f = fopen(path, "w");
    fputs("# hand made\nscale = 9\nmachine=0\nsomething_newer = 1\n  speed\t=  1\r\n", f);
    fclose(f);
    CHECK(settings_load(&b, path) == 0 && b.scale == SCALE_43 && b.machine == SET_QL && b.speed == SPEED_2, "hand made: out of range kept in range");
    remove(path);

#ifndef _WIN32
    setenv("OPENQL_ROMS", "/x/y/roms/", 1);
    unsetenv("OPENQL_DISKS");
    CHECK(!strcmp(disk_dir(), "/x/y/disks"), "disks beside $OPENQL_ROMS (%s)", disk_dir());
    setenv("OPENQL_DISKS", "/elsewhere", 1);
    CHECK(!strcmp(disk_dir(), "/elsewhere"), "$OPENQL_DISKS");
    unsetenv("OPENQL_ROMS");
    setenv("XDG_DATA_HOME", "/data", 1);
    CHECK(!strcmp(rom_dir(), "/data/openql/roms") && !strcmp(settings_path(), "/data/openql/openql.cfg"), "XDG_DATA_HOME");
#endif
    printf(fails ? "settings_test: %d failed\n" : "settings_test: all passed\n", fails);
    return fails != 0;
}
