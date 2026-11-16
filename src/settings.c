/* openql's settings file and folders (settings.h). */
#include "settings.h"
#include <stddef.h>
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

void settings_default(settings *s) {
    memset(s, 0, sizeof *s);
    s->machine = SET_SUPERQL;
    s->ram_ql_kb = 640;
    s->ram_sql_mb = 128;
    s->scale = SCALE_43;
    s->speed = SPEED_1;
    s->sound = 1;
    s->launcher = 1;
}

/* key = value lines; unknown keys are left alone, so older and newer openqls share a file */
static const struct field { const char *key; int type; size_t off; } fields[] = {
#define S(k, m) { k, 0, offsetof(settings, m) }
#define I(k, m) { k, 1, offsetof(settings, m) }
    I("machine", machine), I("ram_ql_kb", ram_ql_kb), I("ram_superql_mb", ram_sql_mb),
    S("rom_ql", rom_ql), S("rom_superql", rom_sql), S("win1", win1), I("win1_read_only", win1_ro),
    S("flp1", flp[0]), S("flp2", flp[1]), S("mdv1", mdv[0]), S("mdv2", mdv[1]), S("host_folder", host_dir),
    I("scale", scale), I("fullscreen", fullscreen), I("speed", speed), I("sound", sound), I("launcher", launcher),
    I("superql_display", display),
#undef S
#undef I
};

int settings_load(settings *s, const char *path) {
    settings_default(s);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[SET_PATH + 64];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *k = line;
        while (*k == ' ' || *k == '\t') k++;
        char *eq = strchr(k, '=');
        if (!eq || *k == '#') continue;
        char *v = eq + 1, *e = eq;
        while (e > k && (e[-1] == ' ' || e[-1] == '\t')) e--;
        *e = 0;
        while (*v == ' ' || *v == '\t') v++;
        for (size_t i = 0; i < sizeof fields / sizeof fields[0]; i++) {
            if (strcmp(fields[i].key, k)) continue;
            char *p = (char *)s + fields[i].off;
            if (fields[i].type) *(int *)p = atoi(v);
            else snprintf(p, SET_PATH, "%s", v);
        }
    }
    fclose(f);
    if (s->machine != SET_QL) s->machine = SET_SUPERQL;
    if (s->scale < 0 || s->scale >= SCALE_MODES) s->scale = SCALE_43;
    if (s->speed < 0 || s->speed >= SPEEDS) s->speed = SPEED_1;
    if (s->display < 0 || s->display >= DISPLAYS) s->display = DISPLAY_QL;
    s->seen = 1;
    return 0;
}

int settings_save(const settings *s, const char *path) {
    char dir[SET_PATH];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) { *slash = 0; make_dirs(dir); }
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "# openql's settings: the launcher writes these\n");
    for (size_t i = 0; i < sizeof fields / sizeof fields[0]; i++) {
        const char *p = (const char *)s + fields[i].off;
        if (fields[i].type) fprintf(f, "%s = %d\n", fields[i].key, *(const int *)p);
        else fprintf(f, "%s = %s\n", fields[i].key, p);
    }
    return fclose(f) ? -1 : 0;
}

int file_exists(const char *path) {
    struct stat st;
    return path && *path && stat(path, &st) == 0 && !S_ISDIR(st.st_mode);
}

int make_dirs(const char *path) {
    char p[SET_PATH];
    snprintf(p, sizeof p, "%s", path);
    for (char *c = p + 1; *c; c++) {
        if (*c != '/' && *c != '\\') continue;
        char was = *c;
        *c = 0;
        if (!(c == p + 2 && p[1] == ':')) mkdir1(p);   /* not a drive letter alone */
        *c = was;
    }
    mkdir1(p);
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode) ? 0 : -1;
}

static void slashes(char *p) {
    for (; *p; p++) if (*p == '\\') *p = '/';
}

const char *data_dir(void) {
    static char d[SET_PATH];
#ifdef _WIN32
    const char *appdata = getenv("APPDATA");
    snprintf(d, sizeof d, "%s/openql", appdata ? appdata : ".");
#else
    const char *data = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    if (data && *data) snprintf(d, sizeof d, "%s/openql", data);
    else snprintf(d, sizeof d, "%s/.local/share/openql", home ? home : ".");
#endif
    slashes(d);
    return d;
}

const char *rom_dir(void) {
    static char d[SET_PATH];
    const char *env = getenv("OPENQL_ROMS");
    if (env && *env) snprintf(d, sizeof d, "%s", env);
    else snprintf(d, sizeof d, "%s/roms", data_dir());
    slashes(d);
    return d;
}

/* as the fetcher has it: $OPENQL_DISKS, else beside the roms folder */
const char *disk_dir(void) {
    static char d[SET_PATH];
    const char *env = getenv("OPENQL_DISKS");
    if (env && *env) { snprintf(d, sizeof d, "%s", env); slashes(d); return d; }
    snprintf(d, sizeof d, "%s", rom_dir());
    size_t n = strlen(d);
    while (n > 1 && d[n - 1] == '/') d[--n] = 0;
    char *slash = strrchr(d, '/');
    if (slash) snprintf(slash, sizeof d - (size_t)(slash - d), "/disks");
    else snprintf(d, sizeof d, "disks");
    return d;
}

const char *state_dir(void) {
    static char d[SET_PATH];
    snprintf(d, sizeof d, "%s/states", data_dir());
    make_dirs(d);
    return d;
}

const char *settings_path(void) {
    static char p[SET_PATH];
    snprintf(p, sizeof p, "%s/openql.cfg", data_dir());
    return p;
}

const char *fetched_rom(int machine) {
    static char p[SET_PATH];
    snprintf(p, sizeof p, "%s/%s", rom_dir(), machine == SET_QL ? "minerva-1.98j4.rom" : "smsqe-3.43-q40.rom");
    return file_exists(p) ? p : NULL;
}

const char *fetched_qle(void) {
    static char p[SET_PATH];
    snprintf(p, sizeof p, "%s/QLE.WIN", disk_dir());
    return file_exists(p) ? p : NULL;
}

const char *speed_name(int speed) {
    static const char *n[SPEEDS] = { "real time", "2 x", "4 x", "as fast as it goes" };
    return speed >= 0 && speed < SPEEDS ? n[speed] : "?";
}

const char *display_name(int display) {
    static const char *n[DISPLAYS] = { "QL colours, 512 x 256 (mode 4)", "High colour, 512 x 256", "High colour, 1024 x 512" };
    return display >= 0 && display < DISPLAYS ? n[display] : "?";
}

const char *scale_name(int scale) {
    static const char *n[SCALE_MODES] = { "4:3, the window's size", "4:3, whole pixels", "fill the window" };
    return scale >= 0 && scale < SCALE_MODES ? n[scale] : "?";
}
