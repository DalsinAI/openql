/*
 * openql: a Sinclair QL or a Super QL in one window, as a single program.
 *
 *   openql [-M ql|superql] [-r ROM] [-m RAM] [-w WIN1 [-R]] [-f FLP [-f FLP]]
 *          [-c CART] [-p PRINTER-FILE] [-s SERIAL-FILE] [-F] [-q]
 *          [-H SECONDS [-o FRAME.ppm] [-i INPUT]...]
 *
 *   -M  the machine: ql (a 68008 QL, Minerva) or superql (the default: a
 *       Q40/Q60-compatible board with a 68040, SMSQ/E)
 *   -r  the Firmware/ROM. Without it, openql looks in $OPENQL_ROMS, then
 *       ~/.local/share/openql/roms (%APPDATA%\openql\roms on Windows), for the
 *       one tools/openql-fetch-roms fetched
 *   -m  RAM: KB on the QL (128 stock, up to 896; 640 by default), MB on the
 *       Super QL (up to 128, the default)
 *   -w  WIN1 on the Super QL: a QXL.WIN file is a whole QL hard disk (-R: read only)
 *   -f  a floppy image in the Super QL's FLP1_ (a second -f: FLP2_): a raw QL
 *       floppy, 720 KB, 1.44 MB or 2.88 MB (a read-only file is write protected)
 *   -c  a 16 KB ROM cartridge for the QL's ROM port
 *   -p  the Super QL's LPT1 output goes to this file
 *   -s  ser1's (COM1's) output goes to this file
 *   -F  start full screen
 *   -q  no sound
 *   -H  run without a window for this many seconds, then stop (tests); -o
 *       writes the last frame there as a PPM
 *   -i  with -H, keys to press: key:NAME@T (f1..f10, enter, esc, space, tab,
 *       up, down, left, right) or type:TEXT@T (\n for ENTER), T in seconds
 *
 * In the window: F11 full screen, Ctrl+F12 the reset button (RAM kept),
 * Ctrl+Shift+F12 a power cycle, Ctrl+F10 pause, Ctrl+F9 takes the disk out
 * of FLP1_ (Ctrl+Shift+F9: FLP2_). A file dropped on the window goes into
 * FLP1_ (FLP2_ with Shift held). On the Super QL the host's pointer over the
 * window moves SMSQ/E's pointer through the serial mouse.
 *
 * The machine keeps pace with real time: one 50 Hz frame of CPU clocks each
 * 20 ms (150,000 for the QL's 68008 at 7.5 MHz, 800,000 for the Super QL's
 * 68040 at 40 MHz), through AC090's interpreter.
 */
#include "ql/ql.h"
#include "superql/superql.h"
#include <SDL.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { M_QL, M_SUPERQL };

static int machine = M_SUPERQL;
static ql q;
static superql s;
static FILE *serial_file, *printer_file;

/* ---- the keyboard ------------------------------------------------------ */

/* Host keys become Amiga raw keys first: both boards already take those (the
 * QL through the keymap below, the Super QL as the PC set 2 codes it wants). */
static int amiga_raw(SDL_Scancode k) {
    static const struct { SDL_Scancode sc; uint8_t raw; } t[] = {
        { SDL_SCANCODE_GRAVE, 0x00 }, { SDL_SCANCODE_1, 0x01 }, { SDL_SCANCODE_2, 0x02 }, { SDL_SCANCODE_3, 0x03 },
        { SDL_SCANCODE_4, 0x04 }, { SDL_SCANCODE_5, 0x05 }, { SDL_SCANCODE_6, 0x06 }, { SDL_SCANCODE_7, 0x07 },
        { SDL_SCANCODE_8, 0x08 }, { SDL_SCANCODE_9, 0x09 }, { SDL_SCANCODE_0, 0x0a }, { SDL_SCANCODE_MINUS, 0x0b },
        { SDL_SCANCODE_EQUALS, 0x0c }, { SDL_SCANCODE_BACKSLASH, 0x0d }, { SDL_SCANCODE_KP_0, 0x0f },
        { SDL_SCANCODE_Q, 0x10 }, { SDL_SCANCODE_W, 0x11 }, { SDL_SCANCODE_E, 0x12 }, { SDL_SCANCODE_R, 0x13 },
        { SDL_SCANCODE_T, 0x14 }, { SDL_SCANCODE_Y, 0x15 }, { SDL_SCANCODE_U, 0x16 }, { SDL_SCANCODE_I, 0x17 },
        { SDL_SCANCODE_O, 0x18 }, { SDL_SCANCODE_P, 0x19 }, { SDL_SCANCODE_LEFTBRACKET, 0x1a },
        { SDL_SCANCODE_RIGHTBRACKET, 0x1b }, { SDL_SCANCODE_KP_1, 0x1d }, { SDL_SCANCODE_KP_2, 0x1e },
        { SDL_SCANCODE_KP_3, 0x1f },
        { SDL_SCANCODE_A, 0x20 }, { SDL_SCANCODE_S, 0x21 }, { SDL_SCANCODE_D, 0x22 }, { SDL_SCANCODE_F, 0x23 },
        { SDL_SCANCODE_G, 0x24 }, { SDL_SCANCODE_H, 0x25 }, { SDL_SCANCODE_J, 0x26 }, { SDL_SCANCODE_K, 0x27 },
        { SDL_SCANCODE_L, 0x28 }, { SDL_SCANCODE_SEMICOLON, 0x29 }, { SDL_SCANCODE_APOSTROPHE, 0x2a },
        { SDL_SCANCODE_NONUSHASH, 0x2b }, { SDL_SCANCODE_KP_4, 0x2d }, { SDL_SCANCODE_KP_5, 0x2e },
        { SDL_SCANCODE_KP_6, 0x2f },
        { SDL_SCANCODE_NONUSBACKSLASH, 0x30 }, { SDL_SCANCODE_Z, 0x31 }, { SDL_SCANCODE_X, 0x32 },
        { SDL_SCANCODE_C, 0x33 }, { SDL_SCANCODE_V, 0x34 }, { SDL_SCANCODE_B, 0x35 }, { SDL_SCANCODE_N, 0x36 },
        { SDL_SCANCODE_M, 0x37 }, { SDL_SCANCODE_COMMA, 0x38 }, { SDL_SCANCODE_PERIOD, 0x39 },
        { SDL_SCANCODE_SLASH, 0x3a }, { SDL_SCANCODE_KP_PERIOD, 0x3c }, { SDL_SCANCODE_KP_7, 0x3d },
        { SDL_SCANCODE_KP_8, 0x3e }, { SDL_SCANCODE_KP_9, 0x3f },
        { SDL_SCANCODE_SPACE, 0x40 }, { SDL_SCANCODE_BACKSPACE, 0x41 }, { SDL_SCANCODE_TAB, 0x42 },
        { SDL_SCANCODE_KP_ENTER, 0x43 }, { SDL_SCANCODE_RETURN, 0x44 }, { SDL_SCANCODE_ESCAPE, 0x45 },
        { SDL_SCANCODE_DELETE, 0x46 }, { SDL_SCANCODE_KP_MINUS, 0x4a }, { SDL_SCANCODE_UP, 0x4c },
        { SDL_SCANCODE_DOWN, 0x4d }, { SDL_SCANCODE_RIGHT, 0x4e }, { SDL_SCANCODE_LEFT, 0x4f },
        { SDL_SCANCODE_F1, 0x50 }, { SDL_SCANCODE_F2, 0x51 }, { SDL_SCANCODE_F3, 0x52 }, { SDL_SCANCODE_F4, 0x53 },
        { SDL_SCANCODE_F5, 0x54 }, { SDL_SCANCODE_F6, 0x55 }, { SDL_SCANCODE_F7, 0x56 }, { SDL_SCANCODE_F8, 0x57 },
        { SDL_SCANCODE_F9, 0x58 }, { SDL_SCANCODE_F10, 0x59 }, { SDL_SCANCODE_KP_DIVIDE, 0x5c },
        { SDL_SCANCODE_KP_MULTIPLY, 0x5d }, { SDL_SCANCODE_KP_PLUS, 0x5e },
        { SDL_SCANCODE_LSHIFT, 0x60 }, { SDL_SCANCODE_RSHIFT, 0x61 }, { SDL_SCANCODE_CAPSLOCK, 0x62 },
        { SDL_SCANCODE_LCTRL, 0x63 }, { SDL_SCANCODE_RCTRL, 0x63 }, { SDL_SCANCODE_LALT, 0x64 },
        { SDL_SCANCODE_RALT, 0x65 }, { SDL_SCANCODE_LGUI, 0x66 }, { SDL_SCANCODE_RGUI, 0x67 },
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) if (t[i].sc == k) return t[i].raw;
    return -1;
}

/* Amiga raw keys onto the QL's keyrow codes. The QL has no Backspace or
 * Delete: CTRL with the left or right arrow does those, so they press CTRL
 * with the arrow. F6 to F10 are SHIFT with F1 to F5, as on the QL. */
typedef struct { int8_t code; uint8_t sca; } qlmap;   /* sca: bit 2 SHIFT, bit 1 CTRL forced on */
static qlmap keymap[128];

static void map(int raw, int code, int sca) { keymap[raw].code = (int8_t)code; keymap[raw].sca = (uint8_t)sca; }

static void keymap_init(void) {
    for (int i = 0; i < 128; i++) keymap[i].code = -1;
    static const char row1[] = "`1234567890-=\\";       /* raw $00-$0D */
    for (int i = 0; row1[i]; i++) { int sh; map(i, ql_key_for_ascii(row1[i], &sh), 0); }
    static const char row2[] = "qwertyuiop[]";          /* raw $10-$1B */
    for (int i = 0; row2[i]; i++) { int sh; map(0x10 + i, ql_key_for_ascii(row2[i], &sh), 0); }
    static const char row3[] = "asdfghjkl;'";           /* raw $20-$2A */
    for (int i = 0; row3[i]; i++) { int sh; map(0x20 + i, ql_key_for_ascii(row3[i], &sh), 0); }
    static const char row4[] = "zxcvbnm,./";            /* raw $31-$3A */
    for (int i = 0; row4[i]; i++) { int sh; map(0x31 + i, ql_key_for_ascii(row4[i], &sh), 0); }
    static const struct { int raw; char ch; } pad[] = {   /* the keypad's digits and signs */
        { 0x0f, '0' }, { 0x1d, '1' }, { 0x1e, '2' }, { 0x1f, '3' }, { 0x2d, '4' }, { 0x2e, '5' }, { 0x2f, '6' },
        { 0x3d, '7' }, { 0x3e, '8' }, { 0x3f, '9' }, { 0x3c, '.' }, { 0x4a, '-' }, { 0x5c, '/' },
    };
    for (size_t i = 0; i < sizeof pad / sizeof pad[0]; i++) { int sh; map(pad[i].raw, ql_key_for_ascii(pad[i].ch, &sh), 0); }
    { int sh; int c = ql_key_for_ascii('*', &sh); map(0x5d, c, sh ? 4 : 0); c = ql_key_for_ascii('+', &sh); map(0x5e, c, sh ? 4 : 0); }
    map(0x40, QLK_SPACE, 0);
    map(0x41, QLK_LEFT, 2);                              /* Backspace: CTRL left, delete the character before */
    map(0x46, QLK_RIGHT, 2);                             /* Del: CTRL right, delete the character under */
    map(0x42, QLK_TAB, 0);
    map(0x43, QLK_ENTER, 0);
    map(0x44, QLK_ENTER, 0);
    map(0x45, QLK_ESC, 0);
    map(0x4c, QLK_UP, 0); map(0x4d, QLK_DOWN, 0); map(0x4e, QLK_RIGHT, 0); map(0x4f, QLK_LEFT, 0);
    static const int fk[5] = { QLK_F1, QLK_F2, QLK_F3, QLK_F4, QLK_F5 };
    for (int i = 0; i < 5; i++) { map(0x50 + i, fk[i], 0); map(0x55 + i, fk[i], 4); }
    map(0x62, QLK_CAPS, 0);
}

static int mod_shift, mod_ctrl, mod_alt;

static void ql_raw_key(uint8_t raw) {
    int up = raw & 0x80, k = raw & 0x7f;
    switch (k) {                                          /* SHIFT, CTRL, ALT (and the Windows keys as ALT) */
    case 0x60: case 0x61: mod_shift = up ? mod_shift & ~(1 << (k - 0x60)) : mod_shift | 1 << (k - 0x60); break;
    case 0x63: mod_ctrl = !up; break;
    case 0x64: case 0x65: case 0x66: case 0x67: mod_alt = up ? mod_alt & ~(1 << (k - 0x64)) : mod_alt | 1 << (k - 0x64); break;
    default: {
        qlmap m = keymap[k];
        if (m.code < 0) return;
        if (!up) ql_modifiers(&q, mod_shift || (m.sca & 4), mod_ctrl || (m.sca & 2), mod_alt);
        ql_key(&q, m.code, !up);
        break;
    }
    }
    ql_modifiers(&q, mod_shift != 0, mod_ctrl, mod_alt != 0);
}

static void raw_key(uint8_t raw) {
    if (machine == M_QL) ql_raw_key(raw);
    else sql_key_amiga(&s, raw);
}

/* ---- the machine -------------------------------------------------------- */

static void serial_out(void *opaque, int port, uint8_t ch) {
    (void)opaque;
    if (port != 0) return;
    FILE *f = serial_file ? serial_file : stdout;
    fputc(ch, f);
    fflush(f);
}

static void printer_out(void *opaque, uint8_t ch) {
    (void)opaque;
    if (printer_file) { fputc(ch, printer_file); fflush(printer_file); }
}

static int pic_w(void) { return machine == M_QL ? QL_SCREEN_W : SQL_OUT_W; }
static int pic_h(void) { return machine == M_QL ? QL_SCREEN_H : SQL_OUT_H; }

static void run_frame(void) {
    if (machine == M_QL) ql_run(&q, q.cpu.clock + QL_FRAME_CLOCKS);
    else sql_run(&s, s.cpu.clock + SQL_FRAME_CLOCKS);
}

static int halted(void) { return machine == M_QL ? q.cpu.halted : s.cpu.halted; }
static uint32_t pc(void) { return machine == M_QL ? q.cpu.pc : s.cpu.pc; }

static void render(uint32_t *rgb) {
    if (machine == M_QL) ql_render(&q, rgb);
    else sql_render(&s, rgb);
}

static void reset(int clear_ram) {
    if (machine == M_QL) ql_reset(&q, clear_ram);
    else sql_reset(&s, clear_ram);
    mod_shift = mod_ctrl = mod_alt = 0;
}

/* The ROM fetched by tools/openql-fetch-roms, when -r is not given. */
static const char *default_rom(void) {
    static char path[1024];
    const char *name = machine == M_QL ? "minerva-1.98j4.rom" : "smsqe-3.43-q40.rom";
    const char *dir = getenv("OPENQL_ROMS");
    if (dir && *dir) {
        snprintf(path, sizeof path, "%s/%s", dir, name);
        if (access(path, R_OK) == 0) return path;
    }
#ifdef _WIN32
    const char *appdata = getenv("APPDATA");
    snprintf(path, sizeof path, "%s/openql/roms/%s", appdata ? appdata : ".", name);
#else
    const char *home = getenv("HOME");
    const char *data = getenv("XDG_DATA_HOME");
    if (data && *data) snprintf(path, sizeof path, "%s/openql/roms/%s", data, name);
    else snprintf(path, sizeof path, "%s/.local/share/openql/roms/%s", home ? home : ".", name);
#endif
    return access(path, R_OK) == 0 ? path : NULL;
}

static void save_ppm(const char *path, const uint32_t *rgb) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    int w = pic_w(), h = pic_h();
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) {
        uint8_t px[3] = { (uint8_t)(rgb[i] >> 16), (uint8_t)(rgb[i] >> 8), (uint8_t)rgb[i] };
        fwrite(px, 1, 3, f);
    }
    fclose(f);
}

/* ---- sound ------------------------------------------------------------------ */

/* The host plays at 48 kHz. The Super QL's DACs come at their sample rate
 * through an SDL audio stream; the QL's BEEP is made here, a square wave
 * at the IPC's first pitch (about 11447 / (10.6 + pitch) Hz). The queue is
 * kept short, so the sound stays with the picture. */
#define HOST_RATE 48000
static SDL_AudioDeviceID audio;
static SDL_AudioStream *astream;
static unsigned astream_rate;
static double beep_phase;

static void sound_frame(void) {
    if (!audio) return;
    if (SDL_GetQueuedAudioSize(audio) > HOST_RATE / 10 * 4) return;   /* more than 100 ms waiting: drop a frame's */
    static int16_t buf[2 * 4096];
    if (machine == M_SUPERQL) {
        unsigned rate = sql_audio_rate(&s);
        if (!astream || rate != astream_rate) {
            if (astream) SDL_FreeAudioStream(astream);
            astream = SDL_NewAudioStream(AUDIO_S16SYS, 2, (int)rate, AUDIO_S16SYS, 2, HOST_RATE);
            astream_rate = rate;
            if (!astream) return;
        }
        int n;
        while ((n = sql_audio_read(&s, buf, 4096)) > 0) SDL_AudioStreamPut(astream, buf, n * 4);
        while ((n = SDL_AudioStreamGet(astream, buf, sizeof buf)) > 0) SDL_QueueAudio(audio, buf, (Uint32)n);
        return;
    }
    if (!q.ipc.sound_on) { beep_phase = 0; return; }
    const int n = HOST_RATE / 50;
    double hz = 11447.0 / (10.6 + q.ipc.pitch1);
    for (int i = 0; i < n; i++) {
        int16_t v = beep_phase < 0.5 ? 6000 : -6000;
        buf[2 * i] = buf[2 * i + 1] = v;
        beep_phase += hz / HOST_RATE;
        if (beep_phase >= 1) beep_phase -= 1;
    }
    SDL_QueueAudio(audio, buf, (Uint32)(n * 4));
}

/* ---- the Super QL's mouse ------------------------------------------------- */

/* The host's pointer over the picture becomes mouse movement: a QL mode's
 * pixel is two of the picture's, a 16-bit mode's one (the large mode) or two
 * (the small one). A pointer that comes back into the window starts again
 * where it is, without a jump. */
static int aim_x = -1, aim_y = -1;

static void aim(int x, int y) {
    int scale = s.dmode == SQL_D_LARGE ? 1 : 2;
    if (aim_x >= 0) {
        int dx = x / scale - aim_x / scale, dy = y / scale - aim_y / scale;
        if (dx || dy) sql_mouse_move(&s, dx, dy);
    }
    aim_x = x; aim_y = y;
}

/* ---- keys typed without a window (-H with -i) ----------------------------- */

/* Each press is an Amiga raw key, held for 60 ms, with SHIFT around it when
 * the character needs it; the next comes 120 ms later. Both boards take
 * these through raw_key(), as they take the window's keys. */
typedef struct { double at; uint8_t raw; uint8_t shift; } press;
static press presses[4096];
static int npress;

static int raw_for_ascii(int ch, int *shift) {
    static const char plain[] = "`1234567890-=\\qwertyuiop[]asdfghjkl;'zxcvbnm,./";
    static const char shifted[] = "~!@#$%^&*()_+|QWERTYUIOP{}ASDFGHJKL:\"ZXCVBNM<>?";
    static const uint8_t raw[] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d,
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b,
        0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a,
        0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a,
    };
    *shift = 0;
    if (ch == ' ') return 0x40;
    if (ch == '\n') return 0x44;
    const char *p = ch ? strchr(plain, ch) : NULL;
    if (p) return raw[p - plain];
    if (ch && (p = strchr(shifted, ch))) { *shift = 1; return raw[p - shifted]; }
    return -1;
}

static void add_input(const char *spec) {
    const char *at = strrchr(spec, '@');
    if (!at) { fprintf(stderr, "openql: input needs @T: %s\n", spec); exit(2); }
    double t = atof(at + 1);
    if (!strncmp(spec, "key:", 4)) {
        static const struct { const char *name; uint8_t raw; } k[] = {
            { "f1", 0x50 }, { "f2", 0x51 }, { "f3", 0x52 }, { "f4", 0x53 }, { "f5", 0x54 },
            { "f6", 0x55 }, { "f7", 0x56 }, { "f8", 0x57 }, { "f9", 0x58 }, { "f10", 0x59 },
            { "enter", 0x44 }, { "esc", 0x45 }, { "space", 0x40 }, { "tab", 0x42 },
            { "up", 0x4c }, { "down", 0x4d }, { "right", 0x4e }, { "left", 0x4f },
        };
        size_t n = (size_t)(at - spec - 4);
        for (size_t i = 0; i < sizeof k / sizeof k[0]; i++)
            if (strlen(k[i].name) == n && !strncmp(spec + 4, k[i].name, n) && npress < 4096) {
                presses[npress++] = (press){ t, k[i].raw, 0 };
                return;
            }
        fprintf(stderr, "openql: no key %.*s\n", (int)n, spec + 4);
        exit(2);
    } else if (!strncmp(spec, "type:", 5)) {
        for (const char *c = spec + 5; c < at && npress < 4096; c++) {
            int ch = *c, shift;
            if (ch == '\\' && c + 1 < at && c[1] == 'n') { ch = '\n'; c++; }
            int r = raw_for_ascii(ch, &shift);
            if (r < 0) { fprintf(stderr, "openql: no key for '%c'\n", ch); exit(2); }
            presses[npress++] = (press){ t, (uint8_t)r, (uint8_t)shift };
            t += 0.12;
        }
    } else { fprintf(stderr, "openql: unknown input %s (key:NAME@T or type:TEXT@T)\n", spec); exit(2); }
}

/* The presses that start or end in the frame that begins at `now`. */
static void inputs_at(double now) {
    const double half = 0.5 / 50;
    for (int i = 0; i < npress; i++) {
        double down = presses[i].at, up = presses[i].at + 0.06;
        if (now >= down - half && now < down + half) {
            if (presses[i].shift) raw_key(0x60);
            raw_key(presses[i].raw);
        } else if (now >= up - half && now < up + half) {
            raw_key(presses[i].raw | 0x80);
            if (presses[i].shift) raw_key(0x60 | 0x80);
        }
    }
}

/* ---- main ---------------------------------------------------------------- */

static void usage(void) {
    fprintf(stderr, "usage: openql [-M ql|superql] [-r ROM] [-m RAM] [-w WIN1 [-R]] [-f FLP [-f FLP]]\n"
                    "              [-c CART] [-p PRINTER-FILE] [-s SERIAL-FILE] [-F] [-q]\n"
                    "              [-H SECONDS [-o FRAME.ppm] [-i key:NAME@T | -i type:TEXT@T]...]\n");
}

int main(int argc, char **argv) {
    const char *rom = NULL, *win1 = NULL, *cart = NULL, *prn = NULL, *ser = NULL, *dump = NULL, *flp[2] = { NULL, NULL };
    int ram = -1, ro = 0, full = 0, quiet = 0, nflp = 0, opt;
    double headless = 0;
    while ((opt = getopt(argc, argv, "M:r:m:w:Rf:c:p:s:FqH:o:i:h")) != -1) {
        switch (opt) {
        case 'M':
            if (!strcmp(optarg, "ql")) machine = M_QL;
            else if (!strcmp(optarg, "superql") || !strcmp(optarg, "super-ql")) machine = M_SUPERQL;
            else { fprintf(stderr, "openql: no machine %s (ql or superql)\n", optarg); return 2; }
            break;
        case 'r': rom = optarg; break;
        case 'm': {
            char *end;
            long v = strtol(optarg, &end, 10);
            if (end == optarg || *end || v <= 0 || v > 1 << 20) { fprintf(stderr, "openql: -m %s is not a RAM size\n", optarg); return 2; }
            ram = (int)v;
            break;
        }
        case 'w': win1 = optarg; break;
        case 'R': ro = 1; break;
        case 'f': if (nflp < 2) flp[nflp++] = optarg; break;
        case 'q': quiet = 1; break;
        case 'c': cart = optarg; break;
        case 'p': prn = optarg; break;
        case 's': ser = optarg; break;
        case 'F': full = 1; break;
        case 'H': headless = atof(optarg); break;
        case 'o': dump = optarg; break;
        case 'i': add_input(optarg); break;
        default: usage(); return 2;
        }
    }
    if (!rom && !(rom = default_rom())) {
        fprintf(stderr, "openql: no ROM: give -r, or fetch one with tools/openql-fetch-roms %s\n",
                machine == M_QL ? "minerva" : "smsqe");
        return 1;
    }
    if (ser && !(serial_file = fopen(ser, "ab"))) perror(ser);
    if (prn && !(printer_file = fopen(prn, "ab"))) perror(prn);

    if (ram > 0) {   /* say so when the board will take a different size */
        int lo = machine == M_QL ? 128 : 4, hi = machine == M_QL ? 896 : (int)SQL_MAX_RAM_MB;
        if (ram < lo || ram > hi)
            fprintf(stderr, "openql: -m %d is outside %d to %d %s; using %d\n", ram, lo, hi,
                    machine == M_QL ? "KB" : "MB", ram < lo ? lo : hi);
    }
    keymap_init();
    const char *title;
    if (machine == M_QL) {
        if (win1 || nflp) fprintf(stderr, "openql: the QL has no disks yet; -w and -f are for the Super QL\n");
        if (ql_init(&q, rom, cart, (uint32_t)(ram > 0 ? ram : 640))) return 1;
        q.ser_out = serial_out;
        title = "OpenQL · Sinclair QL";
        fprintf(stderr, "openql: Sinclair QL, 68008 at 7.5 MHz, %u KB RAM, ROM %s\n", q.ram_bytes >> 10, rom);
    } else {
        if (sql_init(&s, rom, (uint32_t)(ram > 0 ? ram : (int)SQL_MAX_RAM_MB))) return 1;
        if (win1 && sql_attach_disk(&s, win1, ro)) return 1;
        for (int i = 0; i < nflp; i++) if (sql_insert_floppy(&s, i, flp[i], 0)) return 1;
        s.ser_out = serial_out;
        s.par_out = printer_out;
        title = "OpenQL · Super QL";
        fprintf(stderr, "openql: Super QL (Q40/Q60 map), 68040 at 40 MHz, %u MB RAM, ROM %s%s%s\n",
                s.ram_bytes >> 20, rom, win1 ? ", WIN1 " : "", win1 ? win1 : "");
    }

    int w = pic_w(), h = pic_h();
    uint32_t *pic = calloc((size_t)w * h, 4);
    if (!pic) return 1;

    if (headless > 0) {                    /* as fast as the host goes: tests and boot checks */
        for (long f = 0; f < (long)(headless * 50) && !halted(); f++) { inputs_at(f / 50.0); run_frame(); }
        if (halted()) fprintf(stderr, "openql: the CPU halted (double fault) at pc %08x\n", pc());
        render(pic);
        if (dump) save_ppm(dump, pic);
        fprintf(stderr, "openql: pc %08x after %.1f s\n", pc(), headless);
        int rc = halted() ? 1 : 0;
        free(pic);
        if (machine == M_QL) ql_free(&q);
        else sql_free(&s);
        return rc;
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) { fprintf(stderr, "openql: %s\n", SDL_GetError()); return 1; }
    if (!quiet && SDL_InitSubSystem(SDL_INIT_AUDIO) == 0) {
        SDL_AudioSpec want = { 0 }, have;
        want.freq = HOST_RATE; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024;
        audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
        if (audio) SDL_PauseAudioDevice(audio, 0);
        else fprintf(stderr, "openql: no sound: %s\n", SDL_GetError());
    }
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);
    /* A QL drew its picture on a 4:3 television or monitor, whatever its mode. */
    SDL_Window *win = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1024, 768,
                                       SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | (full ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
    if (!win) { fprintf(stderr, "openql: %s\n", SDL_GetError()); return 1; }
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) ren = SDL_CreateRenderer(win, -1, 0);
    if (!ren) { fprintf(stderr, "openql: %s\n", SDL_GetError()); return 1; }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
    SDL_RenderSetLogicalSize(ren, 1024, 768);
    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
    if (!tex) { fprintf(stderr, "openql: %s\n", SDL_GetError()); return 1; }
    if (machine == M_SUPERQL) SDL_ShowCursor(SDL_DISABLE);   /* SMSQ/E draws its own */

    int quit = 0, paused = 0;
    uint64_t freq = SDL_GetPerformanceFrequency(), next = SDL_GetPerformanceCounter();
    const uint64_t frame = freq / 50;
    while (!quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_QUIT: quit = 1; break;
            case SDL_KEYDOWN: case SDL_KEYUP: {
                SDL_Scancode sc = e.key.keysym.scancode;
                int ctrl = (e.key.keysym.mod & KMOD_CTRL) != 0, down = e.type == SDL_KEYDOWN;
                if (sc == SDL_SCANCODE_F11) {
                    if (down && !e.key.repeat)
                        SDL_SetWindowFullscreen(win, SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                    break;
                }
                if (sc == SDL_SCANCODE_F12) {
                    if (down && !e.key.repeat && ctrl) {
                        int power = (e.key.keysym.mod & KMOD_SHIFT) != 0;
                        reset(power);
                        fprintf(stderr, "openql: %s\n", power ? "power cycle (RAM cleared)" : "reset (RAM kept)");
                    }
                    break;
                }
                if (sc == SDL_SCANCODE_F9 && ctrl) {
                    if (down && !e.key.repeat && machine == M_SUPERQL) {
                        int drive = (e.key.keysym.mod & KMOD_SHIFT) ? 1 : 0;
                        sql_eject_floppy(&s, drive);
                        fprintf(stderr, "openql: FLP%d_ is empty\n", drive + 1);
                    }
                    break;
                }
                if (sc == SDL_SCANCODE_F10 && ctrl) {
                    if (down && !e.key.repeat) { paused = !paused; SDL_SetWindowTitle(win, paused ? "OpenQL · paused" : title); }
                    break;
                }
                if (e.key.repeat) break;   /* the QL's own keyboard repeats */
                int raw = amiga_raw(sc);
                if (raw >= 0) raw_key((uint8_t)(raw | (down ? 0 : 0x80)));
                break;
            }
            case SDL_MOUSEMOTION:
                if (machine == M_SUPERQL) {
                    /* with a logical size set, SDL gives the motion in its 1024 x 768 already */
                    aim(e.motion.x * w / 1024, e.motion.y * h / 768);
                }
                break;
            case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP:
                if (machine == M_SUPERQL) {
                    Uint32 b = SDL_GetMouseState(NULL, NULL);
                    sql_mouse_buttons(&s, (uint8_t)((b & SDL_BUTTON_LMASK ? 1 : 0) | (b & SDL_BUTTON_RMASK ? 2 : 0)));
                }
                break;
            case SDL_DROPFILE:
                if (machine == M_SUPERQL) {
                    int drive = (SDL_GetModState() & KMOD_SHIFT) ? 1 : 0;
                    if (sql_insert_floppy(&s, drive, e.drop.file, 0) == 0)
                        fprintf(stderr, "openql: %s in FLP%d_\n", e.drop.file, drive + 1);
                }
                SDL_free(e.drop.file);
                break;
            case SDL_WINDOWEVENT:
                if (e.window.event == SDL_WINDOWEVENT_LEAVE) aim_x = aim_y = -1;
                break;
            default: break;
            }
        }
        if (!paused) {
            run_frame();
            if (halted()) {
                fprintf(stderr, "openql: the CPU halted (double fault) at pc %08x; Ctrl+F12 to reset\n", pc());
                paused = 1;
            }
            render(pic);
            SDL_UpdateTexture(tex, NULL, pic, w * 4);
            sound_frame();
        }
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, NULL, NULL);
        SDL_RenderPresent(ren);
        next += frame;
        uint64_t t = SDL_GetPerformanceCounter();
        if (next + freq / 4 < t) next = t;   /* fell behind (a stall): start again from now */
        if (next > t) SDL_Delay((Uint32)((next - t) * 1000 / freq));
    }
    if (machine == M_QL) ql_free(&q);
    else sql_free(&s);
    if (audio) SDL_CloseAudioDevice(audio);
    if (astream) SDL_FreeAudioStream(astream);
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    free(pic);
    return 0;
}
