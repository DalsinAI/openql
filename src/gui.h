/*
 * openql's own windows: the launcher and the machine's menu (F12), made of
 * the host's own controls through libui-ng (GTK on Linux, Win32 on Windows,
 * Cocoa on macOS), in the host's system font.
 *
 * gui.c asks openql.c for what it shows and does through the app_
 * functions below.
 */
#ifndef OPENQL_GUI_H
#define OPENQL_GUI_H

#include <stddef.h>
#include "settings.h"

int gui_init(char *why, size_t whysz);   /* 0, or -1 with the reason */
void gui_quit(void);

/* The launcher, until Start (1) or Quit (0); the settings are cfg's. */
int gui_launcher(settings *cfg);

/* The machine's menu, until it is closed. */
enum { GUI_RESUME, GUI_SETTINGS, GUI_QUIT };
int gui_menu(settings *cfg);

/* Let the windows' events through while openql's own loop runs. */
void gui_pump(void);

/* ---- openql.c's side ----------------------------------------------------- */

int app_start(void);                     /* the machine cfg describes: 0, or -1 (app_status says why) */
const char *app_status(int *bad);        /* the last word, and whether it is bad news */
void app_fetch(int qle);                 /* the ROMs (0) or QL/E (1), in the background */
int app_fetch_busy(void);
int app_fetch_done(void);                /* 1 once, when a fetch has finished */

int app_is_ql(void);                     /* the running machine */
int app_drive_in(int drive);             /* FLP or mdv 0/1 holds something */
void app_insert(int drive, const char *path);
void app_eject(int drive);
void app_blank_cartridge(void);
int app_hostdir_in(void);
void app_hostdir_insert(const char *folder);
void app_hostdir_write(void);
int app_state(int save, int slot);       /* 0, or -1 (app_status says why) */
int app_state_exists(int slot);
void app_paste(void);
void app_reset(int power);
void app_fullscreen(int on);
void app_redraw(void);                   /* show the machine's picture again */
int app_quit_asked(void);                /* SDL: the host asked openql to end (Ctrl-C, logging out) */
int app_menu_key(void);                  /* SDL: F12 or Esc on the machine's window, or 2: quit */

#endif
