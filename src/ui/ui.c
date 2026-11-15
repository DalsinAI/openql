/* openql's own screens (ui.h): drawing, lists of rows, the file browser. */
#include "ui.h"
#include "font8x16.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#endif

uint32_t ui_px[UI_W * UI_H];

/* ---- drawing -------------------------------------------------------------- */

static inline void blend(uint32_t *d, uint32_t c) {
    uint32_t a = c >> 24;
    if (a == 255) { *d = c; return; }
    uint32_t o = *d, oa = o >> 24;
    uint32_t r = (((c >> 16) & 255) * a + ((o >> 16) & 255) * (255 - a)) / 255;
    uint32_t g = (((c >> 8) & 255) * a + ((o >> 8) & 255) * (255 - a)) / 255;
    uint32_t b = ((c & 255) * a + (o & 255) * (255 - a)) / 255;
    uint32_t na = a + oa * (255 - a) / 255;
    *d = na << 24 | r << 16 | g << 8 | b;
}

void ui_clear(uint32_t argb) {
    for (int i = 0; i < UI_W * UI_H; i++) ui_px[i] = argb;
}

void ui_rect(int x, int y, int w, int h, uint32_t argb) {
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > UI_W ? UI_W : x + w, y1 = y + h > UI_H ? UI_H : y + h;
    for (int j = y0; j < y1; j++)
        for (int i = x0; i < x1; i++) blend(&ui_px[j * UI_W + i], argb);
}

void ui_frame(int x, int y, int w, int h, uint32_t argb) {
    ui_rect(x, y, w, 1, argb); ui_rect(x, y + h - 1, w, 1, argb);
    ui_rect(x, y + 1, 1, h - 2, argb); ui_rect(x + w - 1, y + 1, 1, h - 2, argb);
}

int ui_text(int x, int y, const char *s, uint32_t argb, int scale) {
    for (; *s; s++) {
        unsigned ch = (unsigned char)*s;
        if (ch < 32 || ch > 126) ch = '?';
        const unsigned char *g = font8x16[ch - 32];
        for (int r = 0; r < 16; r++)
            for (int b = 0; b < 8; b++)
                if (g[r] & (0x80 >> b)) ui_rect(x + b * scale, y + r * scale, scale, scale, argb);
        x += 8 * scale;
    }
    return x;
}

int ui_text_w(const char *s, int scale) { return (int)strlen(s) * 8 * scale; }

void ui_text_fit(int x, int y, int w, const char *s, uint32_t argb) {
    int fit = w / 8;
    int n = (int)strlen(s);
    if (n <= fit) { ui_text(x, y, s, argb, 1); return; }
    if (fit < 4) return;
    char buf[512];
    snprintf(buf, sizeof buf, "...%s", s + n - (fit - 3));
    ui_text(x, y, buf, argb, 1);
}

/* ---- the mouse on the canvas ------------------------------------------------ */

static SDL_Rect ui_dst = { 0, 0, UI_W, UI_H };

void ui_set_dst(SDL_Rect dst) { ui_dst = dst; }

int ui_mouse(int wx, int wy, int *cx, int *cy) {
    if (ui_dst.w <= 0 || ui_dst.h <= 0) return 0;
    *cx = (wx - ui_dst.x) * UI_W / ui_dst.w;
    *cy = (wy - ui_dst.y) * UI_H / ui_dst.h;
    return *cx >= 0 && *cy >= 0 && *cx < UI_W && *cy < UI_H;
}

/* ---- lists of rows ------------------------------------------------------------ */

#define ROW_H 22

static int selectable(const ui_row *r) { return r->kind != ROW_HEADING && r->kind != ROW_INFO && !r->disabled; }

static void fix_sel(ui_list *l, const ui_row *rows, int n, int dir) {
    if (n <= 0) { l->sel = 0; return; }
    if (l->sel < 0) l->sel = 0;
    if (l->sel >= n) l->sel = n - 1;
    for (int tries = 0; tries < n && !selectable(&rows[l->sel]); tries++) {
        l->sel += dir;
        if (l->sel < 0) { l->sel = 0; dir = 1; }
        if (l->sel >= n) { l->sel = n - 1; dir = -1; }
    }
}

void ui_list_draw(ui_list *l, const ui_row *rows, int n, int x, int y, int w, int h, int label_w) {
    l->x = x; l->y = y; l->w = w; l->h = h;
    fix_sel(l, rows, n, 1);
    int vis = h / ROW_H;
    if (l->sel < l->top) l->top = l->sel;
    if (l->sel >= l->top + vis) l->top = l->sel - vis + 1;
    if (l->top > 0 && l->sel > 0 && rows[l->sel - 1].kind == ROW_HEADING && l->top == l->sel) l->top--;   /* keep its heading */
    if (l->top < 0) l->top = 0;
    for (int i = l->top; i < n && i < l->top + vis; i++) {
        const ui_row *r = &rows[i];
        int ry = y + (i - l->top) * ROW_H;
        if (r->kind == ROW_HEADING) {
            ui_text(x + 4, ry + 4, r->label, UI_ACCENT, 1);
            ui_rect(x + 4 + ui_text_w(r->label, 1) + 8, ry + 12, w - ui_text_w(r->label, 1) - 16, 1, 0x60e8ecf2u);
            continue;
        }
        int on = i == l->sel;
        if (on) ui_rect(x, ry, w, ROW_H - 2, UI_SEL);
        uint32_t fg = r->disabled ? UI_DIM : UI_TEXT;
        ui_text(x + 12, ry + 3, r->label, fg, 1);
        if (r->value[0]) {
            int vx = x + (label_w > 0 ? label_w : 12 + ui_text_w(r->label, 1) + 16);
            int vw = x + w - 12 - vx;
            char v[300];
            if (r->kind == ROW_CHOICE && on) snprintf(v, sizeof v, "< %s >", r->value);
            else snprintf(v, sizeof v, "%s", r->value);
            ui_text_fit(vx, ry + 3, vw, v, r->colour ? r->colour : (r->kind == ROW_INFO ? UI_DIM : fg));
        }
    }
    if (l->top > 0) ui_text(x + w - 16, y - 16, "^", UI_DIM, 1);
    if (l->top + vis < n) ui_text(x + w - 16, y + vis * ROW_H - 4, "v", UI_DIM, 1);
}

int ui_list_event(ui_list *l, const ui_row *rows, int n, const SDL_Event *e) {
    if (n <= 0) return e->type == SDL_KEYDOWN && e->key.keysym.sym == SDLK_ESCAPE ? UI_BACK : UI_NONE;
    if (e->type == SDL_KEYDOWN) {
        switch (e->key.keysym.sym) {
        case SDLK_UP: l->sel--; fix_sel(l, rows, n, -1); return UI_NONE;
        case SDLK_DOWN: l->sel++; fix_sel(l, rows, n, 1); return UI_NONE;
        case SDLK_PAGEUP: l->sel -= 8; fix_sel(l, rows, n, -1); return UI_NONE;
        case SDLK_PAGEDOWN: l->sel += 8; fix_sel(l, rows, n, 1); return UI_NONE;
        case SDLK_HOME: l->sel = 0; fix_sel(l, rows, n, 1); return UI_NONE;
        case SDLK_END: l->sel = n - 1; fix_sel(l, rows, n, -1); return UI_NONE;
        case SDLK_LEFT: return selectable(&rows[l->sel]) ? UI_LEFT : UI_NONE;
        case SDLK_RIGHT: return selectable(&rows[l->sel]) ? UI_RIGHT : UI_NONE;
        case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE:
            return selectable(&rows[l->sel]) ? (rows[l->sel].kind == ROW_CHOICE ? UI_RIGHT : UI_ACTIVATE) : UI_NONE;
        case SDLK_DELETE: case SDLK_BACKSPACE: return selectable(&rows[l->sel]) ? UI_CLEAR : UI_NONE;
        case SDLK_ESCAPE: return UI_BACK;
        default: return UI_NONE;
        }
    }
    if (e->type == SDL_MOUSEWHEEL) {
        l->sel += e->wheel.y > 0 ? -1 : 1;
        fix_sel(l, rows, n, e->wheel.y > 0 ? -1 : 1);
        return UI_NONE;
    }
    if (e->type == SDL_MOUSEBUTTONDOWN) {          /* a click selects and goes; the pointer alone moves nothing */
        int cx, cy, mx = e->button.x, my = e->button.y;
        if (!ui_mouse(mx, my, &cx, &cy) || cx < l->x || cx >= l->x + l->w || cy < l->y || cy >= l->y + l->h) return UI_NONE;
        int i = l->top + (cy - l->y) / ROW_H;
        if (i < 0 || i >= n || !selectable(&rows[i])) return UI_NONE;
        l->sel = i;
        if (rows[i].kind == ROW_CHOICE) return e->button.button == SDL_BUTTON_RIGHT ? UI_LEFT : UI_RIGHT;
        if (e->button.button == SDL_BUTTON_RIGHT) return rows[i].kind == ROW_FILE ? UI_CLEAR : UI_NONE;
        return UI_ACTIVATE;
    }
    return UI_NONE;
}

/* ---- the file browser ------------------------------------------------------------ */

typedef struct entry { char name[256]; int dir; long long size; } entry;

struct ui_browser {
    char title[128];
    char dir[1024];             /* ends in '/'; "" on Windows: the drives */
    entry *e;
    int n, cap;
    ui_list list;
    ui_row *rows;
    char msg[160];
    int want_dir;               /* choosing a folder: "this folder" is the first row */
};

static int by_name(const void *a, const void *b) {
    const entry *x = a, *y = b;
    if (x->dir != y->dir) return y->dir - x->dir;
    if (!strcmp(x->name, "..")) return -1;
    if (!strcmp(y->name, "..")) return 1;
#ifdef _WIN32
    return _stricmp(x->name, y->name);
#else
    return strcasecmp(x->name, y->name);
#endif
}

static void add(ui_browser *b, const char *name, int dir, long long size) {
    if (b->n == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 64;
        b->e = realloc(b->e, (size_t)b->cap * sizeof *b->e);
        b->rows = realloc(b->rows, (size_t)b->cap * sizeof *b->rows);
    }
    snprintf(b->e[b->n].name, sizeof b->e[b->n].name, "%s", name);
    b->e[b->n].dir = dir;
    b->e[b->n].size = size;
    b->n++;
}

static void load(ui_browser *b) {
    b->n = 0;
    b->list.sel = b->list.top = 0;
    b->msg[0] = 0;
#ifdef _WIN32
    if (!b->dir[0]) {
        DWORD drives = GetLogicalDrives();
        for (int i = 0; i < 26; i++)
            if (drives & (1u << i)) { char d[4] = { (char)('A' + i), ':', 0, 0 }; add(b, d, 1, 0); }
        goto rows;
    }
#endif
    DIR *d = opendir(b->dir);
    if (!d) { snprintf(b->msg, sizeof b->msg, "cannot read this folder"); }
    else {
        struct dirent *de;
        while ((de = readdir(d))) {
            if (!strcmp(de->d_name, ".") || (de->d_name[0] == '.' && strcmp(de->d_name, ".."))) continue;
            char p[1400];
            snprintf(p, sizeof p, "%s%s", b->dir, de->d_name);
            struct stat st;
            if (stat(p, &st)) continue;
            add(b, de->d_name, S_ISDIR(st.st_mode) ? 1 : 0, (long long)st.st_size);
        }
        closedir(d);
        int has_up = 0;
        for (int i = 0; i < b->n; i++) has_up |= !strcmp(b->e[i].name, "..");
#ifdef _WIN32
        if (!has_up) add(b, "..", 1, 0);       /* up to the drives */
#else
        if (!has_up && strcmp(b->dir, "/")) add(b, "..", 1, 0);
#endif
    }
    qsort(b->e, (size_t)b->n, sizeof *b->e, by_name);
    if (b->want_dir && b->dir[0]) {             /* this folder, first */
        add(b, "", 1, 0);
        memmove(b->e + 1, b->e, (size_t)(b->n - 1) * sizeof *b->e);
        memset(&b->e[0], 0, sizeof b->e[0]);
        b->e[0].dir = 2;
    }
#ifdef _WIN32
rows:
#endif
    for (int i = 0; i < b->n; i++) {
        ui_row *r = &b->rows[i];
        memset(r, 0, sizeof *r);
        r->id = i;
        r->kind = ROW_ACTION;
        if (b->e[i].dir == 2) {
            snprintf(r->label, sizeof r->label, "[ use this folder ]");
            r->colour = UI_ACCENT;
            snprintf(r->value, sizeof r->value, "%s", "");
            continue;
        }
        snprintf(r->label, sizeof r->label, "%.60s%s", b->e[i].name, b->e[i].dir && strcmp(b->e[i].name, "..") ? "/" : "");
        if (b->e[i].dir) snprintf(r->value, sizeof r->value, "%s", strcmp(b->e[i].name, "..") ? "folder" : "up");
        else if (b->e[i].size >= 1 << 20) snprintf(r->value, sizeof r->value, "%.1f MB", b->e[i].size / 1048576.0);
        else snprintf(r->value, sizeof r->value, "%lld KB", (b->e[i].size + 1023) / 1024);
        r->colour = b->e[i].dir ? UI_DIM : 0;
    }
}

static void set_dir(ui_browser *b, const char *dir) {
    snprintf(b->dir, sizeof b->dir, "%s", dir);
    for (char *c = b->dir; *c; c++) if (*c == '\\') *c = '/';
    size_t n = strlen(b->dir);
    if (n && b->dir[n - 1] != '/' && n + 1 < sizeof b->dir) { b->dir[n] = '/'; b->dir[n + 1] = 0; }
    load(b);
}

static void go_up(ui_browser *b) {
    size_t n = strlen(b->dir);
    if (n <= 1) return;
    char was[256] = "";
    b->dir[n - 1] = 0;                       /* drop the trailing '/' */
    char *slash = strrchr(b->dir, '/');
    if (slash) { snprintf(was, sizeof was, "%s", slash + 1); slash[1] = 0; }
    else b->dir[0] = 0;                      /* Windows: "C:" up to the drives */
#ifndef _WIN32
    if (!b->dir[0]) strcpy(b->dir, "/");
#endif
    load(b);
    for (int i = 0; i < b->n; i++) if (!strcmp(b->e[i].name, was)) b->list.sel = i;   /* where we came from */
}

ui_browser *ui_browser_open(const char *title, const char *start, int want_dir) {
    ui_browser *b = calloc(1, sizeof *b);
    if (!b) return NULL;
    b->want_dir = want_dir;
    snprintf(b->title, sizeof b->title, "%s", title);
    char dir[1024] = "", pick[256] = "";
    if (start && *start) {
        snprintf(dir, sizeof dir, "%s", start);
        for (char *c = dir; *c; c++) if (*c == '\\') *c = '/';
        struct stat st;
        if (stat(dir, &st) || !S_ISDIR(st.st_mode)) {   /* a file: its folder, with it selected */
            char *slash = strrchr(dir, '/');
            if (slash) { snprintf(pick, sizeof pick, "%s", slash + 1); slash[1] = 0; }
            else dir[0] = 0;
        }
    }
    struct stat st;
    if (!dir[0] || stat(dir, &st) || !S_ISDIR(st.st_mode)) {
        const char *home = getenv("HOME");
#ifdef _WIN32
        if (!home) home = getenv("USERPROFILE");
#endif
        snprintf(dir, sizeof dir, "%s", home ? home : ".");
    }
    set_dir(b, dir);
    for (int i = 0; i < b->n; i++) if (!strcmp(b->e[i].name, pick)) b->list.sel = i;
    return b;
}

void ui_browser_draw(ui_browser *b) {
    ui_rect(40, 30, UI_W - 80, UI_H - 60, UI_PANEL);
    ui_frame(40, 30, UI_W - 80, UI_H - 60, 0x60e8ecf2u);
    ui_text(60, 44, b->title, UI_ACCENT, 1);
    ui_text_fit(60, 66, UI_W - 120, b->dir[0] ? b->dir : "the drives", UI_DIM);
    ui_rect(60, 88, UI_W - 120, 1, 0x40e8ecf2u);
    if (b->msg[0]) ui_text(60, 100, b->msg, UI_BAD, 1);
    ui_list_draw(&b->list, b->rows, b->n, 50, 96, UI_W - 100, UI_H - 96 - 70, 0);
    ui_text(60, UI_H - 58, b->want_dir ? "Enter opens a folder   Backspace up a folder   Esc cancel"
                                       : "Enter choose   Backspace up a folder   Esc cancel   a letter: jump", UI_DIM, 1);
}

int ui_browser_event(ui_browser *b, const SDL_Event *e, char *out, size_t outsz) {
    if (e->type == SDL_TEXTINPUT && e->text.text[0] && b->n) {   /* jump to the next name with that letter */
        char c = e->text.text[0];
        for (int k = 1; k <= b->n; k++) {
            int i = (b->list.sel + k) % b->n;
            char f = b->e[i].name[0];
            if ((f | 32) == (c | 32)) { b->list.sel = i; break; }
        }
        return 0;
    }
    int a = ui_list_event(&b->list, b->rows, b->n, e);
    if (a == UI_BACK) return -1;
    if (a == UI_CLEAR && e->type == SDL_KEYDOWN && e->key.keysym.sym == SDLK_BACKSPACE) { go_up(b); return 0; }
    if (a == UI_LEFT) { go_up(b); return 0; }
    if (a != UI_ACTIVATE && a != UI_RIGHT) return 0;
    if (b->list.sel < 0 || b->list.sel >= b->n) return 0;
    entry *en = &b->e[b->list.sel];
    if (en->dir == 2) {                          /* this folder */
        if (a != UI_ACTIVATE) return 0;
        snprintf(out, outsz, "%s", b->dir);
        size_t n = strlen(out);
        if (n > 1 && out[n - 1] == '/' && !(n == 3 && out[1] == ':')) out[n - 1] = 0;
        return 1;
    }
    if (!strcmp(en->name, "..")) { go_up(b); return 0; }
    char p[1400];
    snprintf(p, sizeof p, "%s%s", b->dir, en->name);
    if (en->dir) { set_dir(b, p); return 0; }
    if (a == UI_RIGHT || b->want_dir) return 0;   /* right arrow only opens folders; a folder is wanted */
    snprintf(out, outsz, "%s", p);
    return 1;
}

void ui_browser_close(ui_browser *b) {
    if (!b) return;
    free(b->e);
    free(b->rows);
    free(b);
}
