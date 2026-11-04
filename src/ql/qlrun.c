/*
 * Boot a Firmware/ROM on the C QL board and report.
 *
 *   qlrun [-s SECONDS] [-m KIB] [-c CART] [-p PREFIX] [-i INPUT]... ROM
 *     -s  emulated seconds to run (default 10)
 *     -m  RAM in KB: 128 (a stock QL) up to 896
 *     -c  a 16 KB ROM cartridge for the ROM port
 *     -p  draw the screen once each emulated second to PREFIX-NNN.ppm
 *     -i  input: key:NAME@T (f1..f5, enter, esc, space, up, down, left, right)
 *         type:TEXT@T (\n for ENTER)
 *
 * ser1's output goes to stdout. Once per emulated second a status line goes
 * to stderr.
 */
#include "ql.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct { double at; int code; int shift; } press;
static press presses[4096];
static int npress;

static int named_key(const char *n) {
    static const struct { const char *name; int code; } k[] = {
        { "f1", QLK_F1 }, { "f2", QLK_F2 }, { "f3", QLK_F3 }, { "f4", QLK_F4 }, { "f5", QLK_F5 },
        { "enter", QLK_ENTER }, { "esc", QLK_ESC }, { "space", QLK_SPACE }, { "tab", QLK_TAB },
        { "up", QLK_UP }, { "down", QLK_DOWN }, { "left", QLK_LEFT }, { "right", QLK_RIGHT },
    };
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) if (!strcmp(n, k[i].name)) return k[i].code;
    return -1;
}

static void add_input(const char *spec) {
    const char *at = strrchr(spec, '@');
    if (!at) { fprintf(stderr, "qlrun: input needs @T: %s\n", spec); exit(2); }
    double t = atof(at + 1);
    if (!strncmp(spec, "key:", 4)) {
        char name[32]; size_t n = (size_t)(at - spec - 4);
        if (n >= sizeof name) n = sizeof name - 1;
        memcpy(name, spec + 4, n); name[n] = 0;
        int code = named_key(name);
        if (code < 0) { fprintf(stderr, "qlrun: no key %s\n", name); exit(2); }
        presses[npress++] = (press){ t, code, 0 };
    } else if (!strncmp(spec, "type:", 5)) {
        for (const char *s = spec + 5; s < at && npress < 4096; s++) {
            int ch = *s, shift;
            if (ch == '\\' && s + 1 < at && s[1] == 'n') { ch = '\n'; s++; }
            int code = ql_key_for_ascii(ch, &shift);
            if (code < 0) { fprintf(stderr, "qlrun: the QL has no key for '%c'\n", ch); exit(2); }
            presses[npress++] = (press){ t, code, shift };
            t += 0.12;                      /* a key each 120 ms: 60 ms down, 60 up */
        }
    } else { fprintf(stderr, "qlrun: unknown input %s\n", spec); exit(2); }
}

static void save_ppm(ql *q, const char *prefix, int n) {
    static uint32_t rgb[QL_SCREEN_W * QL_SCREEN_H];
    ql_render(q, rgb);
    char path[512];
    snprintf(path, sizeof path, "%s-%03d.ppm", prefix, n);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", QL_SCREEN_W, QL_SCREEN_H);
    for (int i = 0; i < QL_SCREEN_W * QL_SCREEN_H; i++) {
        uint8_t px[3] = { (uint8_t)(rgb[i] >> 16), (uint8_t)(rgb[i] >> 8), (uint8_t)rgb[i] };
        fwrite(px, 1, 3, f);
    }
    fclose(f);
}

int main(int argc, char **argv) {
    double seconds = 10;
    unsigned ram = 128;
    const char *cart = NULL, *prefix = NULL;
    int opt;
    while ((opt = getopt(argc, argv, "s:m:c:p:i:")) != -1) {
        switch (opt) {
        case 's': seconds = atof(optarg); break;
        case 'm': ram = (unsigned)atoi(optarg); break;
        case 'c': cart = optarg; break;
        case 'p': prefix = optarg; break;
        case 'i': add_input(optarg); break;
        default: return 2;
        }
    }
    if (optind >= argc) { fprintf(stderr, "usage: qlrun [-s S] [-m KIB] [-c CART] [-p PREFIX] [-i INPUT] ROM\n"); return 2; }
    static ql q;
    if (ql_init(&q, argv[optind], cart, ram)) return 1;
    uint64_t total = (uint64_t)(seconds * QL_CLOCK_HZ);
    uint64_t step = QL_CLOCK_HZ / 50;
    int shot = 0;
    for (uint64_t t = step; t <= total && !q.cpu.halted; t += step) {
        double now = (double)t / QL_CLOCK_HZ;
        for (int i = 0; i < npress; i++) {
            int held = q.ipc.held >> presses[i].code & 1;
            if (!held && now >= presses[i].at && now < presses[i].at + 0.03) {
                ql_modifiers(&q, presses[i].shift, 0, 0);
                ql_key(&q, presses[i].code, 1);
            } else if (held && now >= presses[i].at + 0.06 && now < presses[i].at + 0.09) {
                ql_key(&q, presses[i].code, 0);
                ql_modifiers(&q, 0, 0, 0);
            }
        }
        ql_run(&q, t);
        if (t % QL_CLOCK_HZ == 0) {
            fprintf(stderr, "qlrun: %2llu s  pc %05x  sr %04x  frames %llu  instructions %llu  mode %d\n",
                    (unsigned long long)(t / QL_CLOCK_HZ), q.cpu.pc, ac090_get_sr(&q.cpu),
                    (unsigned long long)q.frames, (unsigned long long)q.cpu.instructions, ql_mode8(&q) ? 8 : 4);
            if (prefix) save_ppm(&q, prefix, shot++);
        }
    }
    if (q.cpu.halted) fprintf(stderr, "qlrun: the CPU halted (double fault) at pc %05x\n", q.cpu.pc);
    if (prefix) save_ppm(&q, prefix, shot);
    ql_free(&q);
    return q.cpu.halted ? 1 : 0;
}
