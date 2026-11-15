/*
 * openql: a Sinclair QL or a Super QL in one window, as a single program.
 *
 * With nothing on its command line it opens its launcher (-L: anyway), where
 * the machine, its disks and the picture are chosen and QL/E is fetched.
 *
 *   openql [-L] [-M ql|superql] [-r ROM] [-m RAM] [-w WIN1 [-R]] [-f FLP [-f FLP]]
 *          [-D FOLDER] [-d MDV [-d MDV]] [-c CART] [-p PRINTER-FILE] [-s FILE|tcp:[HOST:]PORT] [-S FILE] [-F] [-q]
 *          [-l STATE] [-H SECONDS [-o FRAME.ppm] [-O STATE] [-i INPUT]...]
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
 *   -D  a folder on the host as the Super QL's FLP2_ (hostdir.h): written
 *       back to the folder when the disk comes out or openql stops
 *   -d  a microdrive cartridge image in the QL's mdv1_ (a second -d: mdv2_):
 *       255 sectors of 686 bytes (QLAY, MiSTer); a new or empty file is a
 *       blank cartridge to FORMAT; a read-only file has its tab out
 *   -c  a 16 KB ROM cartridge for the QL's ROM port
 *   -p  the Super QL's LPT1 output goes to this file
 *   -s  ser1 (COM1): its output goes to this file; or tcp:PORT, a null-modem
 *       cable on 127.0.0.1:PORT both ways (openql listens), or tcp:HOST:PORT
 *       (openql connects: two openqls, or a terminal program)
 *   -S  ser1's input: the bytes of this file come in once, at the baud rate
 *   -F  start full screen
 *   -q  no sound
 *   -H  run without a window for this many seconds, then stop (tests); -o
 *       writes the last frame there as a PPM
 *   -i  with -H, keys to press: key:NAME@T (f1..f10, enter, esc, space, tab,
 *       up, down, left, right) or type:TEXT@T (\n for ENTER), T in seconds
 *
 *   -l  start from a saved state (-O saves one at the end of -H)
 *
 * In the window: F12 the menu (disks, saved states, speed, picture, paste,
 * the launcher), Ctrl+Shift+V types the clipboard in, Ctrl+F8 runs as fast
 * as the host goes, F11 full screen, Ctrl+F12 the reset button (RAM kept),
 * Ctrl+Shift+F12 a power cycle, Ctrl+F10 pause, Ctrl+F9 takes the disk out
 * of FLP1_ or the cartridge out of mdv1_ (Ctrl+Shift+F9: FLP2_, mdv2_). A
 * file dropped on the window goes into FLP1_ or mdv1_ (FLP2_, mdv2_ with
 * Shift held). On the Super QL the host's pointer over the
 * window moves SMSQ/E's pointer through the serial mouse.
 *
 * The machine keeps pace with real time: one 50 Hz frame of CPU clocks each
 * 20 ms (150,000 for the QL's 68008 at 7.5 MHz, 800,000 for the Super QL's
 * 68040 at 40 MHz), through AC090's interpreter.
 */
#include "ql/ql.h"
#include "superql/superql.h"
#include "hostdir.h"
#include "hostser.h"
#include "settings.h"
#include "gui.h"
#include <SDL.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { M_QL, M_SUPERQL };

static int machine = M_SUPERQL;
static ql q;
static superql s;
static FILE *printer_file;
static hostser *ser1;               /* ser1's (COM1's) host end: stdout, a file, input from a file, TCP */

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
    hostser_put(ser1, ch);
}

/* What came in on ser1's host end goes to the machine, as fast as it takes it. */
static void serial_frame(void) {
    hostser_poll(ser1);
    for (int n = 0; n < 4096; n++) {
        int room = machine == M_QL ? ql_serial_space(&q, 0) : sql_serial_space(&s, 0);
        if (room <= 0) break;
        int c = hostser_get(ser1);
        if (c < 0) break;
        if (machine == M_QL) ql_serial_in(&q, 0, (uint8_t)c);
        else sql_serial_in(&s, 0, (uint8_t)c);
    }
}

static void printer_out(void *opaque, uint8_t ch) {
    (void)opaque;
    if (printer_file) { fputc(ch, printer_file); fflush(printer_file); }
}

static int pic_w(void) { return machine == M_QL ? QL_SCREEN_W : SQL_OUT_W; }
static int pic_h(void) { return machine == M_QL ? QL_SCREEN_H : SQL_OUT_H; }

static void run_frame(void) {
    serial_frame();
    if (machine == M_QL) ql_run(&q, q.cpu.clock + QL_FRAME_CLOCKS);
    else sql_run(&s, s.cpu.clock + SQL_FRAME_CLOCKS);
}

static int halted(void) { return machine == M_QL ? q.cpu.halted : s.cpu.halted; }
static uint32_t pc(void) { return machine == M_QL ? q.cpu.pc : s.cpu.pc; }

static void render(uint32_t *rgb) {
    if (machine == M_QL) ql_render(&q, rgb);
    else sql_render(&s, rgb);
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
    if (machine == M_SUPERQL) {   /* SMSQ/E's keyboard is a UK one: " and @ swap, # and \ have keys of their own */
        static const struct { char ch; uint8_t raw, shift; } uk[] = {
            { '"', 0x02, 1 }, { '@', 0x2a, 1 }, { '#', 0x2b, 0 }, { '~', 0x2b, 1 },
            { '\\', 0x30, 0 }, { '|', 0x30, 1 },
        };
        for (size_t i = 0; i < sizeof uk / sizeof uk[0]; i++)
            if (uk[i].ch == ch) { *shift = uk[i].shift; return uk[i].raw; }
    }
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

/* ---- pasting text: the host's clipboard typed in ------------------------------ */

/* A character a step: its key down for two frames, up for one, with SHIFT
 * around it when it needs it: about 16 characters a second, which both
 * machines' keyboard queues keep up with. */
static char *paste_buf;
static size_t paste_pos, paste_len;
static int paste_phase, paste_raw = -1, paste_shift;

static void paste_start(void) {
    char *t = SDL_GetClipboardText();
    if (!t) return;
    free(paste_buf);
    paste_len = strlen(t);
    paste_buf = malloc(paste_len + 1);
    if (paste_buf) memcpy(paste_buf, t, paste_len + 1);
    else paste_len = 0;
    paste_pos = 0;
    paste_phase = 0;
    SDL_free(t);
}

static int pasting(void) { return paste_buf && (paste_pos < paste_len || paste_raw >= 0); }

static void paste_frame(void) {
    if (!pasting()) return;
    if (paste_raw >= 0) {                       /* a key is down */
        if (++paste_phase < 2) return;
        raw_key((uint8_t)(paste_raw | 0x80));
        if (paste_shift) raw_key(0x60 | 0x80);
        paste_raw = -1;
        return;                                 /* the up frame */
    }
    while (paste_pos < paste_len) {
        int ch = (unsigned char)paste_buf[paste_pos++], shift = 0, raw;
        if (ch == '\r') continue;
        if (ch == '\t') raw = 0x42;
        else if ((raw = raw_for_ascii(ch, &shift)) < 0) continue;   /* no key for it */
        if (shift) raw_key(0x60);
        raw_key((uint8_t)raw);
        paste_raw = raw; paste_shift = shift; paste_phase = 0;
        return;
    }
    free(paste_buf); paste_buf = NULL;
}

/* ---- the window ----------------------------------------------------------------- */

static settings cfg;                   /* what the launcher and the menu set */
static int use_launcher;               /* openql started without a machine on its command line */
static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_Texture *tex;
static uint32_t *pic;
static int tex_w, tex_h;
static SDL_Rect pic_dst;               /* where the picture went, in the renderer's pixels */
static const char *title = "OpenQL";
static int machine_up;
static char status[200];               /* the launcher's and the menu's last word */
static uint32_t status_colour;
enum { UI_GOOD = 1, UI_BAD, UI_ACCENT, UI_TEXT };

static void say(uint32_t colour, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(status, sizeof status, fmt, ap);
    va_end(ap);
    status_colour = colour;
    fprintf(stderr, "openql: %s\n", status);
}

/* The window's points in the renderer's pixels (they differ on a high-DPI screen). */
static double dpi_scale(void) {
    int ww, wh, ow, oh;
    SDL_GetWindowSize(win, &ww, &wh);
    SDL_GetRendererOutputSize(ren, &ow, &oh);
    return ww > 0 ? (double)ow / ww : 1;
}

static SDL_Rect fit43(int ow, int oh) {
    SDL_Rect r;
    if (ow * 3 > oh * 4) { r.h = oh; r.w = oh * 4 / 3; }
    else { r.w = ow; r.h = ow * 3 / 4; }
    r.x = (ow - r.w) / 2; r.y = (oh - r.h) / 2;
    return r;
}

/* Where the machine's picture goes: a 4:3 screen as large as the window
 * takes, or the largest whole multiples of its pixels that still look 4:3,
 * or the whole window. */
static SDL_Rect picture_rect(int ow, int oh) {
    SDL_Rect r = fit43(ow, oh);
    if (cfg.scale == SCALE_STRETCH) { r.x = r.y = 0; r.w = ow; r.h = oh; }
    else if (cfg.scale == SCALE_INTEGER) {
        int w = pic_w(), h = pic_h(), bw = 0, bh = 0;
        for (int sx = 1; sx <= 12; sx++)
            for (int sy = 1; sy <= 12; sy++) {
                int W = w * sx, H = h * sy;
                double aspect = (double)W / H;
                if (W > ow || H > oh || aspect < 4.0 / 3 * 0.97 || aspect > 4.0 / 3 * 1.03) continue;
                if (W * H > bw * bh) { bw = W; bh = H; }
            }
        if (bw) { r.w = bw; r.h = bh; r.x = (ow - bw) / 2; r.y = (oh - bh) / 2; }
    }
    return r;
}

static void present(void) {
    if (!ren) return;
    int ow, oh;
    SDL_GetRendererOutputSize(ren, &ow, &oh);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    if (tex && machine_up) {
        pic_dst = picture_rect(ow, oh);
        SDL_RenderCopy(ren, tex, NULL, &pic_dst);
    }
    SDL_RenderPresent(ren);
}

static void set_fullscreen(int on) {
    cfg.fullscreen = on;
    if (win) SDL_SetWindowFullscreen(win, on ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

/* ---- starting and stopping a machine -------------------------------------------- */

static const char *rom_for(const settings *c) {
    const char *own = c->machine == SET_QL ? c->rom_ql : c->rom_sql;
    return own[0] ? own : fetched_rom(c->machine);
}


/* ---- a host folder in FLP2_ (hostdir.h) ---------------------------------------- */

static int hd_in;                      /* the host folder's image is in FLP2_ */
static char hd_folder[SET_PATH], hd_img[SET_PATH];

static void say(uint32_t colour, const char *fmt, ...);

static int hostdir_in(const char *folder) {
    char why[300];
    int skipped = 0;
    snprintf(hd_img, sizeof hd_img, "%s/host-folder-flp2.img", data_dir());
    make_dirs(data_dir());
    if (hostdir_build(folder, hd_img, &skipped, why, sizeof why) || sql_insert_floppy(&s, 1, hd_img, 0)) {
        say(UI_BAD, "the host folder: %s", why);
        return -1;
    }
    snprintf(hd_folder, sizeof hd_folder, "%s", folder);
    hd_in = 1;
    if (skipped) say(UI_ACCENT, "%s in FLP2_; %d files did not fit (2.8 MB) or had names too long", folder, skipped);
    return 0;
}

/* What the QL wrote, back to the folder; with eject, the disk comes out too. */
static void hostdir_out(int eject) {
    if (!hd_in) return;
    if (eject) { sql_eject_floppy(&s, 1); hd_in = 0; }
    else if (s.fdc.d[1].f) fflush(s.fdc.d[1].f);
    char why[300];
    int n = hostdir_sync(hd_img, hd_folder, why, sizeof why);
    if (n < 0) say(UI_BAD, "the host folder: %s", why);
    else say(UI_GOOD, "%d file%s written to %s", n, n == 1 ? "" : "s", hd_folder);
}

static void machine_down(void) {
    if (!machine_up) return;
    if (machine == M_SUPERQL) hostdir_out(1);
    if (machine == M_QL) ql_free(&q);
    else sql_free(&s);
    machine_up = 0;
    free(paste_buf); paste_buf = NULL; paste_raw = -1;
}

/* The machine the settings describe; -1 with the reason said. */
static int machine_start(const settings *c, const char *cart) {
    machine_down();
    machine = c->machine == SET_QL ? M_QL : M_SUPERQL;
    const char *rom = rom_for(c);
    if (!rom || !file_exists(rom)) {
        say(UI_BAD, "no %s ROM yet: Fetch the ROMs first", machine == M_QL ? "Minerva" : "SMSQ/E");
        return -1;
    }
    if (machine == M_QL) {
        memset(&q, 0, sizeof q);
        if (ql_init(&q, rom, cart, (uint32_t)c->ram_ql_kb)) { say(UI_BAD, "the QL did not start: ROM %s", rom); return -1; }
        machine_up = 1;
        for (int i = 0; i < 2; i++)
            if (c->mdv[i][0] && ql_mdv_insert(&q, i, c->mdv[i], 0)) { say(UI_BAD, "cannot open mdv%d_'s %s", i + 1, c->mdv[i]); machine_down(); return -1; }
        q.ser_out = serial_out;
        title = "OpenQL · Sinclair QL";
        fprintf(stderr, "openql: Sinclair QL, 68008 at 7.5 MHz, %u KB RAM, ROM %s\n", q.ram_bytes >> 10, rom);
    } else {
        memset(&s, 0, sizeof s);
        if (sql_init(&s, rom, (uint32_t)c->ram_sql_mb)) { say(UI_BAD, "the Super QL did not start: ROM %s", rom); return -1; }
        machine_up = 1;
        if (c->win1[0] && sql_attach_disk(&s, c->win1, c->win1_ro)) { say(UI_BAD, "cannot open WIN1_'s %s", c->win1); machine_down(); return -1; }
        for (int i = 0; i < 2; i++)
            if (c->flp[i][0] && !(i == 1 && c->host_dir[0]) && sql_insert_floppy(&s, i, c->flp[i], 0)) { say(UI_BAD, "cannot open FLP%d_'s %s", i + 1, c->flp[i]); machine_down(); return -1; }
        if (c->host_dir[0] && hostdir_in(c->host_dir)) { machine_down(); return -1; }
        s.ser_out = serial_out;
        s.par_out = printer_out;
        title = "OpenQL · Super QL";
        fprintf(stderr, "openql: Super QL (Q40/Q60 map), 68040 at 40 MHz, %u MB RAM, ROM %s%s%s\n",
                s.ram_bytes >> 20, rom, c->win1[0] ? ", WIN1 " : "", c->win1);
    }
    mod_shift = mod_ctrl = mod_alt = 0;
    int w = pic_w(), h = pic_h();
    free(pic);
    pic = calloc((size_t)w * h, 4);
    if (win) {
        if (!tex || w != tex_w || h != tex_h) {
            if (tex) SDL_DestroyTexture(tex);
            tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
            tex_w = w; tex_h = h;
        }
        SDL_SetWindowTitle(win, title);
    }
    if (!hd_in || status_colour != UI_ACCENT) status[0] = 0;
    return pic ? 0 : -1;
}

/* ---- fetching the ROMs and QL/E -------------------------------------------------- */

/* tools/openql-fetch-roms does the downloading and checking, in a thread of
 * its own; the launcher shows how it went. */
static SDL_atomic_t fetch_busy;        /* 1 while it runs */
static int fetch_rc = -1, fetch_what;  /* its exit status; FETCH_ROMS or FETCH_QLE */
static char fetch_cmd[4096], fetch_log[SET_PATH];
enum { FETCH_ROMS = 1, FETCH_QLE };

static int fetch_thread(void *unused) {
    (void)unused;
    fetch_rc = system(fetch_cmd);
    SDL_AtomicSet(&fetch_busy, 0);
    return 0;
}

static const char *fetcher_path(void) {
    static char p[SET_PATH];
    char *base = SDL_GetBasePath();
    static const char *rel[] = { "openql-fetch-roms", "openql-fetch-roms.py", "tools/openql-fetch-roms", "../tools/openql-fetch-roms" };
    for (size_t i = 0; i < sizeof rel / sizeof rel[0]; i++) {
        snprintf(p, sizeof p, "%s%s", base ? base : "./", rel[i]);
        if (file_exists(p)) { SDL_free(base); return p; }
    }
    SDL_free(base);
    snprintf(p, sizeof p, "tools/openql-fetch-roms");
    return file_exists(p) ? p : NULL;
}

/* QLE_WIN.ZIP already downloaded by hand: beside openql, here, or in Downloads. */
static const char *qle_zip_at_hand(void) {
    static char p[SET_PATH];
    char *base = SDL_GetBasePath();
    const char *home = getenv("HOME");
#ifdef _WIN32
    if (!home) home = getenv("USERPROFILE");
#endif
    const char *dirs[3] = { base ? base : "", "", home };
    const char *sub[3] = { "", "", "/Downloads/" };
    for (int i = 0; i < 3; i++) {
        if (!dirs[i]) continue;
        snprintf(p, sizeof p, "%s%sQLE_WIN.ZIP", dirs[i], sub[i]);
        if (file_exists(p)) { SDL_free(base); return p; }
    }
    SDL_free(base);
    return NULL;
}

static void fetch_start(int what) {
    if (SDL_AtomicGet(&fetch_busy)) return;
    const char *tool = fetcher_path();
    if (!tool) { say(UI_BAD, "openql-fetch-roms is not beside openql"); return; }
    make_dirs(data_dir());
    snprintf(fetch_log, sizeof fetch_log, "%s/fetch.log", data_dir());
    char ids[SET_PATH + 64];
    const char *zip = what == FETCH_QLE ? qle_zip_at_hand() : NULL;
    if (what == FETCH_ROMS) snprintf(ids, sizeof ids, "minerva smsqe");
    else if (zip) snprintf(ids, sizeof ids, "--zip \"%s\" qle", zip);
    else snprintf(ids, sizeof ids, "%sqle", fetched_rom(SET_SUPERQL) ? "" : "smsqe ");
#ifdef _WIN32
    /* cmd.exe: the whole line in quotes of its own; Python as python, else the py launcher */
    snprintf(fetch_cmd, sizeof fetch_cmd, "\"(python \"%s\" %s || py -3 \"%s\" %s) > \"%s\" 2>&1\"", tool, ids, tool, ids, fetch_log);
#else
    snprintf(fetch_cmd, sizeof fetch_cmd, "python3 '%s' %s > '%s' 2>&1", tool, ids, fetch_log);
#endif
    if (zip && what == FETCH_ROMS) zip = NULL;
    if (what == FETCH_QLE && !fetched_rom(SET_SUPERQL) && zip) {   /* the zip is one ID only: the ROM after it */
        char more[3 * SET_PATH];
        snprintf(more, sizeof more,
#ifdef _WIN32
                 " & (python \"%s\" smsqe || py -3 \"%s\" smsqe) >> \"%s\" 2>&1", tool, tool, fetch_log);
        size_t n = strlen(fetch_cmd);
        fetch_cmd[n - 1] = 0;                                    /* inside the outer quotes */
        strncat(fetch_cmd, more, sizeof fetch_cmd - strlen(fetch_cmd) - 2);
        strcat(fetch_cmd, "\"");
#else
                 " && python3 '%s' smsqe >> '%s' 2>&1", tool, fetch_log);
        strncat(fetch_cmd, more, sizeof fetch_cmd - strlen(fetch_cmd) - 1);
#endif
    }
    fetch_what = what;
    fetch_rc = -1;
    SDL_AtomicSet(&fetch_busy, 1);
    say(UI_ACCENT, what == FETCH_QLE ? (zip ? "unpacking QL/E from %s ..." : "downloading QL/E from sinclairql.net ...")
                                     : "downloading the ROMs ...", zip ? zip : "");
    SDL_Thread *t = SDL_CreateThread(fetch_thread, "fetch", NULL);
    if (t) SDL_DetachThread(t);
    else { SDL_AtomicSet(&fetch_busy, 0); say(UI_BAD, "cannot start the download"); }
}

/* The fetch has finished: say how, and use QL/E once it is here. */
static void fetch_done(void) {
    if (!fetch_what || SDL_AtomicGet(&fetch_busy)) return;
    int what = fetch_what;
    fetch_what = 0;
    if (what == FETCH_QLE && fetched_qle()) {
        cfg.machine = SET_SUPERQL;
        cfg.ram_sql_mb = 128;
        snprintf(cfg.win1, sizeof cfg.win1, "%s", fetched_qle());
        cfg.win1_ro = 0;
        say(UI_GOOD, "QL/E is in WIN1_: Start");
        return;
    }
    if (fetch_rc == 0 && what == FETCH_ROMS) { say(UI_GOOD, "the ROMs are here"); return; }
    char last[400] = "", line[400];
    FILE *f = fopen(fetch_log, "r");
    while (f && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0]) snprintf(last, sizeof last, "%s", line);
    }
    if (f) fclose(f);
    if (strstr(last, "not found") || strstr(last, "not recognized") || fetch_rc == 127 || fetch_rc == 9009)
        say(UI_BAD, "the download needs Python 3 (python.org)");
    else say(UI_BAD, "the download failed: %s", last[0] ? last : "see fetch.log");
}

/* ---- what the launcher and the menu ask of openql (gui.h) -------------------------- */

static const char *base_name(const char *p) {
    const char *a = strrchr(p, '/'), *b = strrchr(p, '\\');
    if (b > a) a = b;
    return a ? a + 1 : p;
}

static const char *state_file(int n) {
    static char p[SET_PATH];
    snprintf(p, sizeof p, "%s/%s-%d.state", state_dir(), machine == M_QL ? "ql" : "superql", n);
    return p;
}

static void reset(int clear_ram);
static void make_window(void);

const char *app_status(int *bad) { *bad = status_colour == UI_BAD; return status; }
void app_fetch(int qle) { fetch_start(qle ? FETCH_QLE : FETCH_ROMS); }
int app_fetch_busy(void) { return SDL_AtomicGet(&fetch_busy); }
int app_fetch_done(void) {
    if (!fetch_what || SDL_AtomicGet(&fetch_busy)) return 0;
    fetch_done();
    return 1;
}

int app_start(void) {
    if (SDL_AtomicGet(&fetch_busy)) { say(UI_ACCENT, "Still downloading: a moment."); return -1; }
    cfg.seen = 1;
    return machine_start(&cfg, NULL);
}

int app_is_ql(void) { return machine == M_QL; }
int app_drive_in(int drive) { return machine == M_QL ? q.mdv[drive].tape != NULL : s.fdc.d[drive].f != NULL && !(drive == 1 && hd_in); }

void app_insert(int drive, const char *path) {
    if (machine == M_SUPERQL && drive == 1 && hd_in) { hostdir_out(1); cfg.host_dir[0] = 0; }
    if (machine == M_SUPERQL) {
        if (sql_insert_floppy(&s, drive, path, 0) == 0) { snprintf(cfg.flp[drive], SET_PATH, "%s", path); say(UI_GOOD, "%s is in FLP%d_.", base_name(path), drive + 1); }
        else say(UI_BAD, "FLP%d_ cannot take %s.", drive + 1, base_name(path));
    } else {
        if (ql_mdv_insert(&q, drive, path, 0) == 0) { snprintf(cfg.mdv[drive], SET_PATH, "%s", path); say(UI_GOOD, "%s is in mdv%d_.", base_name(path), drive + 1); }
        else say(UI_BAD, "mdv%d_ cannot take %s.", drive + 1, base_name(path));
    }
}

void app_eject(int drive) {
    if (machine == M_SUPERQL && drive == 1 && hd_in) { hostdir_out(1); cfg.host_dir[0] = 0; return; }
    if (machine == M_SUPERQL) { sql_eject_floppy(&s, drive); cfg.flp[drive][0] = 0; }
    else { ql_mdv_eject(&q, drive); cfg.mdv[drive][0] = 0; }
    say(UI_TEXT, "%s%d_ is empty.", machine == M_SUPERQL ? "FLP" : "mdv", drive + 1);
}

void app_blank_cartridge(void) {
    char p[SET_PATH];
    make_dirs(disk_dir());
    for (int k = 1; k < 1000; k++) {
        snprintf(p, sizeof p, "%s/cartridge-%d.mdv", disk_dir(), k);
        if (!file_exists(p)) break;
    }
    app_insert(!q.mdv[0].tape ? 0 : 1, p);   /* written when the drive stops */
}

int app_hostdir_in(void) { return hd_in; }
void app_hostdir_insert(const char *folder) {
    if (s.fdc.d[1].f) app_eject(1);
    if (hostdir_in(folder) == 0) {
        snprintf(cfg.host_dir, SET_PATH, "%s", folder);
        if (status_colour != UI_ACCENT) say(UI_GOOD, "%s is FLP2_.", folder);
    }
}
void app_hostdir_write(void) { hostdir_out(0); }

int app_state(int save, int slot) {
    const char *why = "";
    int rc = save ? (machine == M_QL ? ql_save_state(&q, state_file(slot), &why) : sql_save_state(&s, state_file(slot), &why))
                  : (machine == M_QL ? ql_load_state(&q, state_file(slot), &why) : sql_load_state(&s, state_file(slot), &why));
    if (rc) say(UI_BAD, "Not %s: %s.", save ? "saved" : "loaded", why);
    else {
        if (!save) mod_shift = mod_ctrl = mod_alt = 0;
        say(UI_GOOD, save ? "Saved in slot %d." : "Slot %d loaded.", slot);
    }
    return rc;
}
int app_state_exists(int slot) { return file_exists(state_file(slot)); }
void app_paste(void) { paste_start(); }
void app_reset(int power) { reset(power); }
void app_fullscreen(int on) { set_fullscreen(on); }
void app_redraw(void) { present(); }

/* While the menu is open: the machine's window still answers. */
int app_quit_asked(void) {
    SDL_PumpEvents();
    SDL_Event e;
    return SDL_PeepEvents(&e, 1, SDL_GETEVENT, SDL_QUIT, SDL_QUIT) > 0;
}

int app_menu_key(void) {
    SDL_Event e;
    int r = 0;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) r = 2;
        else if (e.type == SDL_KEYDOWN && !e.key.repeat && (e.key.keysym.sym == SDLK_F12 || e.key.keysym.sym == SDLK_ESCAPE) && !r) r = 1;
    }
    SDL_Delay(15);
    return r;
}

/* ---- the machine in the window -------------------------------------------------------- */

enum { RUN_QUIT, RUN_SETTINGS };

static int gui_ok;                     /* the launcher and the menu can be shown */

/* The machine's window, made the first time and shown again after the launcher. */
static void make_window(void) {
    if (!win) {
        /* A QL drew its picture on a 4:3 television or monitor, whatever its mode. */
        win = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1024, 768,
                               SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | (cfg.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
        if (!win) { fprintf(stderr, "openql: %s\n", SDL_GetError()); exit(1); }
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (!ren) ren = SDL_CreateRenderer(win, -1, 0);
        if (!ren) { fprintf(stderr, "openql: %s\n", SDL_GetError()); exit(1); }
    } else {
        SDL_SetWindowFullscreen(win, cfg.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
        SDL_ShowWindow(win);
        SDL_RaiseWindow(win);
    }
    int w = pic_w(), h = pic_h();
    if (!tex || w != tex_w || h != tex_h) {
        if (tex) SDL_DestroyTexture(tex);
        tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
        tex_w = w; tex_h = h;
    }
    SDL_SetWindowTitle(win, title);
}

static int run_window(void) {
    make_window();
    int w = pic_w(), h = pic_h();
    SDL_ShowCursor(machine == M_SUPERQL ? SDL_DISABLE : SDL_ENABLE);   /* SMSQ/E draws its own */
    int paused = 0, warp = 0;
    uint64_t freq = SDL_GetPerformanceFrequency(), next = SDL_GetPerformanceCounter();
    const uint64_t frame = freq / 50;
    for (;;) {
        SDL_Event e;
        int open_menu = 0;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_QUIT: return RUN_QUIT;
            case SDL_KEYDOWN: case SDL_KEYUP: {
                SDL_Scancode sc = e.key.keysym.scancode;
                int ctrl = (e.key.keysym.mod & KMOD_CTRL) != 0, shift = (e.key.keysym.mod & KMOD_SHIFT) != 0, down = e.type == SDL_KEYDOWN;
                if (sc == SDL_SCANCODE_F11) {
                    if (down && !e.key.repeat) set_fullscreen(!cfg.fullscreen);
                    break;
                }
                if (sc == SDL_SCANCODE_F12) {
                    if (down && !e.key.repeat) {
                        if (ctrl) {
                            reset(shift);
                            fprintf(stderr, "openql: %s\n", shift ? "power cycle (RAM cleared)" : "reset (RAM kept)");
                        } else open_menu = gui_ok;
                    }
                    break;
                }
                if (sc == SDL_SCANCODE_F9 && ctrl) {
                    if (down && !e.key.repeat) app_eject(shift ? 1 : 0);
                    break;
                }
                if (sc == SDL_SCANCODE_F10 && ctrl) {
                    if (down && !e.key.repeat) { paused = !paused; SDL_SetWindowTitle(win, paused ? "OpenQL · paused" : title); }
                    break;
                }
                if (sc == SDL_SCANCODE_F8 && ctrl) {
                    if (down && !e.key.repeat) { warp = !warp; SDL_SetWindowTitle(win, warp ? "OpenQL · as fast as it goes" : title); }
                    break;
                }
                if (sc == SDL_SCANCODE_V && ctrl && shift) {
                    if (down && !e.key.repeat) paste_start();
                    break;
                }
                if (e.key.repeat) break;   /* the QL's own keyboard repeats */
                int raw = amiga_raw(sc);
                if (raw >= 0) raw_key((uint8_t)(raw | (down ? 0 : 0x80)));
                break;
            }
            case SDL_MOUSEMOTION:
                if (machine == M_SUPERQL && pic_dst.w > 0 && pic_dst.h > 0) {
                    double k = dpi_scale();
                    int x = (int)((e.motion.x * k - pic_dst.x) * w / pic_dst.w), y = (int)((e.motion.y * k - pic_dst.y) * h / pic_dst.h);
                    aim(x < 0 ? 0 : x >= w ? w - 1 : x, y < 0 ? 0 : y >= h ? h - 1 : y);
                }
                break;
            case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP:
                if (machine == M_SUPERQL) {
                    Uint32 b = SDL_GetMouseState(NULL, NULL);
                    sql_mouse_buttons(&s, (uint8_t)((b & SDL_BUTTON_LMASK ? 1 : 0) | (b & SDL_BUTTON_RMASK ? 2 : 0)));
                }
                break;
            case SDL_DROPFILE:
                app_insert((SDL_GetModState() & KMOD_SHIFT) ? 1 : 0, e.drop.file);
                SDL_free(e.drop.file);
                break;
            case SDL_WINDOWEVENT:
                if (e.window.event == SDL_WINDOWEVENT_LEAVE) aim_x = aim_y = -1;
                break;
            default: break;
            }
        }
        if (open_menu) {
            /* the keys held when the menu opened would stay down: let them go */
            for (int k = 0; k < 0x68; k++) if (k < 0x60 || k > 0x67) raw_key((uint8_t)(k | 0x80));
            raw_key(0x60 | 0x80); raw_key(0x61 | 0x80); raw_key(0x63 | 0x80); raw_key(0x64 | 0x80); raw_key(0x65 | 0x80);
            int was_full = cfg.fullscreen;
            if (was_full) SDL_SetWindowFullscreen(win, 0);    /* the menu's window over a full screen would hide */
            present();
            status[0] = 0;
            int m = gui_menu(&cfg);
            if (use_launcher) settings_save(&cfg, settings_path());
            if (m == GUI_QUIT) return RUN_QUIT;
            if (m == GUI_SETTINGS) return RUN_SETTINGS;
            if (cfg.fullscreen || was_full) set_fullscreen(cfg.fullscreen);
            SDL_RaiseWindow(win);
            SDL_ShowCursor(machine == M_SUPERQL ? SDL_DISABLE : SDL_ENABLE);
            aim_x = aim_y = -1;
            next = SDL_GetPerformanceCounter();
            continue;
        }
        if (!paused) {
            int speed = warp ? SPEED_MAX : cfg.speed;
            int frames = speed == SPEED_2 ? 2 : speed == SPEED_4 ? 4 : 1;
            uint64_t start = SDL_GetPerformanceCounter();
            for (int f = 0; ; f++) {
                paste_frame();
                run_frame();
                if (halted()) break;
                if (cfg.sound && audio) sound_frame();
                else if (machine == M_SUPERQL) { int16_t junk[2 * 1024]; while (sql_audio_read(&s, junk, 1024) > 0) {} }
                if (speed == SPEED_MAX ? SDL_GetPerformanceCounter() - start > frame * 4 / 5 : f + 1 >= frames) break;
            }
            if (halted()) {
                fprintf(stderr, "openql: the CPU halted (double fault) at pc %08x; Ctrl+F12 to reset\n", pc());
                paused = 1;
            }
            render(pic);
            SDL_UpdateTexture(tex, NULL, pic, w * 4);
        }
        present();
        next += frame;
        uint64_t t = SDL_GetPerformanceCounter();
        if (next + freq / 4 < t) next = t;   /* fell behind (a stall): start again from now */
        if (next > t) SDL_Delay((Uint32)((next - t) * 1000 / freq));
    }
}

/* ---- main ------------------------------------------------------------------------- */

static void usage(void) {
    fprintf(stderr, "usage: openql [-L] [-M ql|superql] [-r ROM] [-m RAM] [-w WIN1 [-R]] [-f FLP [-f FLP]] [-D FOLDER]\n"
                    "              [-d MDV [-d MDV]] [-c CART] [-p PRINTER-FILE] [-s FILE|tcp:[HOST:]PORT] [-S FILE] [-F] [-q]\n"
                    "              [-l STATE] [-H SECONDS [-o FRAME.ppm] [-O STATE] [-i key:NAME@T | -i type:TEXT@T]...]\n");
}

static void reset(int clear_ram) {
    if (machine == M_QL) ql_reset(&q, clear_ram);
    else sql_reset(&s, clear_ram);
    mod_shift = mod_ctrl = mod_alt = 0;
}

int main(int argc, char **argv) {
    const char *ser_in = NULL, *state_in = NULL, *state_out = NULL;
    const char *rom = NULL, *cart = NULL, *prn = NULL, *ser = NULL, *dump = NULL;
    int ram = -1, full = 0, quiet = 0, nflp = 0, nmdv = 0, opt, force_launcher = 0, any = 0;
    double headless = 0;
    const char *inputs[256];               /* -i: read once the machine is known (its keyboard differs) */
    int ninputs = 0;
    settings_load(&cfg, settings_path());
    settings saved = cfg;
    settings line;                          /* what the command line asks for, over the defaults */
    settings_default(&line);
    line.scale = cfg.scale;
    line.sound = cfg.sound;
    while ((opt = getopt(argc, argv, "LM:r:m:w:Rf:D:d:c:p:s:S:FqH:o:i:l:O:h")) != -1) {
        any = 1;
        switch (opt) {
        case 'L': force_launcher = 1; break;
        case 'M':
            if (!strcmp(optarg, "ql")) line.machine = SET_QL;
            else if (!strcmp(optarg, "superql") || !strcmp(optarg, "super-ql")) line.machine = SET_SUPERQL;
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
        case 'w': snprintf(line.win1, SET_PATH, "%s", optarg); break;
        case 'R': line.win1_ro = 1; break;
        case 'f': if (nflp < 2) snprintf(line.flp[nflp++], SET_PATH, "%s", optarg); break;
        case 'D': snprintf(line.host_dir, SET_PATH, "%s", optarg); break;
        case 'd': if (nmdv < 2) snprintf(line.mdv[nmdv++], SET_PATH, "%s", optarg); break;
        case 'q': quiet = 1; break;
        case 'c': cart = optarg; break;
        case 'p': prn = optarg; break;
        case 's': ser = optarg; break;
        case 'S': ser_in = optarg; break;
        case 'F': full = 1; break;
        case 'H': headless = atof(optarg); break;
        case 'o': dump = optarg; break;
        case 'O': state_out = optarg; break;
        case 'l': state_in = optarg; break;
        case 'i': if (ninputs < 256) inputs[ninputs++] = optarg; break;
        default: usage(); return 2;
        }
    }
    if (optind < argc) { usage(); return 2; }
    use_launcher = force_launcher || (!any && cfg.launcher);
    if (!use_launcher && any) {                       /* the command line's machine */
        if (ram > 0) {   /* say so when the board will take a different size */
            int lo = line.machine == SET_QL ? 128 : 4, hi = line.machine == SET_QL ? 896 : (int)SQL_MAX_RAM_MB;
            if (ram < lo || ram > hi)
                fprintf(stderr, "openql: -m %d is outside %d to %d %s; using %d\n", ram, lo, hi,
                        line.machine == SET_QL ? "KB" : "MB", ram < lo ? lo : hi);
            if (line.machine == SET_QL) line.ram_ql_kb = ram; else line.ram_sql_mb = ram;
        }
        if (rom) snprintf(line.machine == SET_QL ? line.rom_ql : line.rom_sql, SET_PATH, "%s", rom);
        if (line.machine == SET_QL && (line.win1[0] || nflp)) fprintf(stderr, "openql: the QL has no disks; -w and -f are for the Super QL\n");
        if (line.machine == SET_SUPERQL && nmdv) fprintf(stderr, "openql: the Super QL has no microdrives; -d is for the QL\n");
        if (!rom_for(&line)) {
            fprintf(stderr, "openql: no ROM: give -r, or fetch one with tools/openql-fetch-roms %s\n",
                    line.machine == SET_QL ? "minerva" : "smsqe");
            return 1;
        }
        cfg = line;
    }
    cfg.fullscreen = full || (use_launcher && cfg.fullscreen);
    if (quiet) cfg.sound = 0;
    if (ser && !(ser1 = hostser_open(ser))) return 1;
    if (ser_in && hostser_input_file(&ser1, ser_in)) return 1;
    if (prn && !(printer_file = fopen(prn, "ab"))) perror(prn);
    keymap_init();
    machine = cfg.machine == SET_QL ? M_QL : M_SUPERQL;
    for (int i = 0; i < ninputs; i++) add_input(inputs[i]);

    if (headless > 0) {                    /* as fast as the host goes: tests and boot checks */
        if (machine_start(&cfg, cart)) return 1;
        const char *why = "";
        if (state_in && (machine == M_QL ? ql_load_state(&q, state_in, &why) : sql_load_state(&s, state_in, &why))) {
            fprintf(stderr, "openql: %s: %s\n", state_in, why);
            return 1;
        }
        for (long f = 0; f < (long)(headless * 50) && !halted(); f++) { inputs_at(f / 50.0); run_frame(); }
        if (halted()) fprintf(stderr, "openql: the CPU halted (double fault) at pc %08x\n", pc());
        render(pic);
        if (dump) save_ppm(dump, pic);
        if (state_out && (machine == M_QL ? ql_save_state(&q, state_out, &why) : sql_save_state(&s, state_out, &why)))
            fprintf(stderr, "openql: %s: %s\n", state_out, why);
        fprintf(stderr, "openql: pc %08x after %.1f s\n", pc(), headless);
        int rc = halted() ? 1 : 0;
        machine_down();
        free(pic);
        hostser_close(ser1);
        return rc;
    }

    /* the host's controls first: on macOS, libui wants to make the application itself */
    char why[300] = "";
    gui_ok = gui_init(why, sizeof why) == 0;
    if (!gui_ok) {
        fprintf(stderr, "openql: no launcher or menu: %s\n", why);
        if (use_launcher) { fprintf(stderr, "openql: give a machine on the command line (-M ql or -M superql)\n"); return 1; }
    }
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) { fprintf(stderr, "openql: %s\n", SDL_GetError()); return 1; }
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) == 0) {
        SDL_AudioSpec want = { 0 }, have;
        want.freq = HOST_RATE; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024;
        audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
        if (audio) SDL_PauseAudioDevice(audio, 0);
        else fprintf(stderr, "openql: no sound: %s\n", SDL_GetError());
    }
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");

    int rc = 0;
    if (!use_launcher) {
        if (machine_start(&cfg, cart)) { fprintf(stderr, "openql: %s\n", status); rc = 1; goto out; }
        const char *why = "";
        if (state_in && (machine == M_QL ? ql_load_state(&q, state_in, &why) : sql_load_state(&s, state_in, &why)))
            fprintf(stderr, "openql: %s: %s\n", state_in, why);
        if (run_window() == RUN_QUIT) goto out;
        use_launcher = 1;                           /* Settings, from the menu */
        settings keep = cfg;                        /* the launcher shows what was saved, with this run's picture choices */
        cfg = saved;
        cfg.scale = keep.scale; cfg.fullscreen = keep.fullscreen; cfg.speed = keep.speed;
    }
    for (;;) {
        if (win) SDL_HideWindow(win);
        if (!gui_launcher(&cfg)) break;
        if (run_window() == RUN_QUIT) break;
    }
    if (use_launcher) settings_save(&cfg, settings_path());
out:
    machine_down();
    hostser_close(ser1);
    if (audio) SDL_CloseAudioDevice(audio);
    if (astream) SDL_FreeAudioStream(astream);
    if (tex) SDL_DestroyTexture(tex);
    if (ren) SDL_DestroyRenderer(ren);
    if (win) SDL_DestroyWindow(win);
    SDL_Quit();
    if (gui_ok) gui_quit();
    free(pic);
    return rc;
}
