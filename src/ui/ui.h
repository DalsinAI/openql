/*
 * openql's own screens: the launcher, the menu (F12) and the file browser,
 * drawn in software on an 800 x 600 canvas that the window shows over (or
 * instead of) the machine's picture.
 */
#ifndef OPENQL_UI_H
#define OPENQL_UI_H

#include <SDL.h>
#include <stdint.h>

#define UI_W 800
#define UI_H 600

/* colours, ARGB */
#define UI_BG      0xf0101828u
#define UI_PANEL   0xf81c2636u
#define UI_SEL     0xff2f5d9au
#define UI_TEXT    0xffe8ecf2u
#define UI_DIM     0xff8a96a8u
#define UI_ACCENT  0xfff0c040u
#define UI_GOOD    0xff6cd08cu
#define UI_BAD     0xfff07a6au

extern uint32_t ui_px[UI_W * UI_H];

void ui_clear(uint32_t argb);
void ui_rect(int x, int y, int w, int h, uint32_t argb);
void ui_frame(int x, int y, int w, int h, uint32_t argb);         /* an outline */
int ui_text(int x, int y, const char *s, uint32_t argb, int scale);   /* returns the x after it */
int ui_text_w(const char *s, int scale);
void ui_text_fit(int x, int y, int w, const char *s, uint32_t argb);  /* cut short from the left with ... */

/* ---- a list of rows: label, value, and what a key does to them ---------- */

enum { ROW_ACTION, ROW_CHOICE, ROW_FILE, ROW_HEADING, ROW_INFO };

typedef struct ui_row {
    int id;                 /* the caller's */
    int kind;
    char label[64];
    char value[256];
    uint32_t colour;        /* the value's, or 0 */
    int disabled;
} ui_row;

enum { UI_NONE, UI_ACTIVATE, UI_LEFT, UI_RIGHT, UI_CLEAR, UI_BACK, UI_QUIT };

typedef struct ui_list {
    int sel, top;
    int x, y, w, h;         /* where it was drawn: for the mouse */
} ui_list;

void ui_list_draw(ui_list *l, const ui_row *rows, int n, int x, int y, int w, int h, int label_w);
/* An event for the list: what it asks of the selected row (l->sel), or UI_NONE. */
int ui_list_event(ui_list *l, const ui_row *rows, int n, const SDL_Event *e);

/* The window's point (x, y) on the canvas, where it is shown at dst. */
void ui_set_dst(SDL_Rect dst);
int ui_mouse(int wx, int wy, int *cx, int *cy);

/* ---- choosing a file -------------------------------------------------------- */

typedef struct ui_browser ui_browser;
ui_browser *ui_browser_open(const char *title, const char *start);   /* start: a file or a folder */
void ui_browser_draw(ui_browser *b);
/* 1: chosen (the path in out), -1: cancelled, 0: still choosing */
int ui_browser_event(ui_browser *b, const SDL_Event *e, char *out, size_t outsz);
void ui_browser_close(ui_browser *b);

#endif
