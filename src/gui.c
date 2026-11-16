/* openql's launcher and menu, in the host's own controls (gui.h). */
#include "gui.h"
#include "debug.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ui.h>

static int inited;

int gui_init(char *why, size_t whysz) {
    if (inited) return 0;
    uiInitOptions o;
    memset(&o, 0, sizeof o);
    const char *err = uiInit(&o);
    if (err) {
        snprintf(why, whysz, "%s", err);
        uiFreeInitError(err);
        return -1;
    }
    uiMainSteps();
    inited = 1;
    return 0;
}


void gui_pump(void) {
    /* uiMainStep(0) does one waiting event (or nothing) and says only whether
     * openql is still running, so a few dozen take whatever is queued */
    for (int i = 0; inited && i < 64; i++) uiMainStep(0);
}

/* ---- the header: the QL's black, silver and white ------------------------- */

static void fill(uiDrawContext *c, double x, double y, double w, double h, double r, double g, double b) {
    uiDrawBrush br;
    memset(&br, 0, sizeof br);
    br.Type = uiDrawBrushTypeSolid;
    br.R = r; br.G = g; br.B = b; br.A = 1;
    uiDrawPath *p = uiDrawNewPath(uiDrawFillModeWinding);
    uiDrawPathAddRectangle(p, x, y, w, h);
    uiDrawPathEnd(p);
    uiDrawFill(c, p, &br);
    uiDrawFreePath(p);
}

static double text(uiDrawContext *c, const char *s, double x, double y, double size, int bold, double grey) {
    uiFontDescriptor f;
    uiLoadControlFont(&f);
    uiAttributedString *as = uiNewAttributedString(s);
    uiAttributedStringSetAttribute(as, uiNewSizeAttribute(size), 0, strlen(s));
    if (bold) uiAttributedStringSetAttribute(as, uiNewWeightAttribute(uiTextWeightBold), 0, strlen(s));
    uiAttributedStringSetAttribute(as, uiNewColorAttribute(grey, grey, grey, 1), 0, strlen(s));
    uiDrawTextLayoutParams p = { as, &f, 2000, uiDrawTextAlignLeft };
    uiDrawTextLayout *tl = uiDrawNewTextLayout(&p);
    double w, h;
    uiDrawTextLayoutExtents(tl, &w, &h);
    uiDrawText(c, tl, x, y - h / 2);
    uiDrawFreeTextLayout(tl);
    uiFreeAttributedString(as);
    uiFreeFontDescriptor(&f);
    return w;
}

/* As the QL's own manual and case have it: a black ground, a brushed
 * silver plate with black letters, white words beside it. */
static void header_draw(uiAreaHandler *h, uiArea *a, uiAreaDrawParams *p) {
    (void)h; (void)a;
    double W = p->AreaWidth, H = p->AreaHeight, mid = H / 2;
    fill(p->Context, 0, 0, W, H, 0.06, 0.06, 0.06);
    uiDrawBrush br;
    memset(&br, 0, sizeof br);
    uiDrawBrushGradientStop stops[3] = { { 0, 0.93, 0.93, 0.93, 1 }, { 0.5, 0.78, 0.78, 0.79, 1 }, { 1, 0.62, 0.62, 0.63, 1 } };
    br.Type = uiDrawBrushTypeLinearGradient;
    br.X0 = 0; br.Y0 = mid - 22; br.X1 = 0; br.Y1 = mid + 22;
    br.Stops = stops; br.NumStops = 3;
    uiDrawPath *plate = uiDrawNewPath(uiDrawFillModeWinding);
    uiDrawPathAddRectangle(plate, 24, mid - 22, 156, 44);
    uiDrawPathEnd(plate);
    uiDrawFill(p->Context, plate, &br);
    uiDrawFreePath(plate);
    text(p->Context, "OpenQL", 38, mid, 24, 1, 0.08);
    double x = 200;
    text(p->Context, "Sinclair QL  \xc2\xb7  Super QL", x, mid - 9, 15, 1, 1.0);
    text(p->Context, "Choose a machine, its disks and how it looks, then Start", x, mid + 12, 11, 0, 0.72);
}
static void no_mouse(uiAreaHandler *h, uiArea *a, uiAreaMouseEvent *e) { (void)h; (void)a; (void)e; }
static void no_cross(uiAreaHandler *h, uiArea *a, int left) { (void)h; (void)a; (void)left; }
static void no_drag(uiAreaHandler *h, uiArea *a) { (void)h; (void)a; }
static int no_key(uiAreaHandler *h, uiArea *a, uiAreaKeyEvent *e) { (void)h; (void)a; (void)e; return 0; }
static uiAreaHandler header_handler = { header_draw, no_mouse, no_cross, no_drag, no_key };

/* ---- small helpers ------------------------------------------------------------ */

static uiBox *hbox(void) { uiBox *b = uiNewHorizontalBox(); uiBoxSetPadded(b, 1); return b; }
static uiBox *vbox(void) { uiBox *b = uiNewVerticalBox(); uiBoxSetPadded(b, 1); return b; }

static uiGroup *group(const char *title, uiControl *child) {
    uiGroup *g = uiNewGroup(title);
    uiGroupSetMargined(g, 1);
    uiGroupSetChild(g, child);
    return g;
}

static uiCombobox *combo(const char *const *items) {
    uiCombobox *c = uiNewCombobox();
    for (; *items; items++) uiComboboxAppend(c, *items);
    return c;
}

static void show(uiControl *c, int on) { if (on) uiControlShow(c); else uiControlHide(c); }
static void enable(uiControl *c, int on) { if (on) uiControlEnable(c); else uiControlDisable(c); }

/* A disk's row in a grid: its name, where it comes from, Choose and Eject. */
typedef struct disk_row {
    uiLabel *name;
    uiEntry *path;
    uiButton *choose, *eject;
    char *target;           /* the settings' path */
    int folder;             /* a folder, not a file */
    int drive;              /* the menu's: 0 or 1; -1 for WIN1 */
} disk_row;

static void disk_row_add(uiGrid *g, int y, disk_row *r, const char *name) {
    r->name = uiNewLabel(name);
    r->path = uiNewEntry();
    uiEntrySetReadOnly(r->path, 1);
    r->choose = uiNewButton("Choose\xe2\x80\xa6");
    r->eject = uiNewButton("Eject");
    uiGridAppend(g, uiControl(r->name), 0, y, 1, 1, 0, uiAlignFill, 0, uiAlignCenter);
    uiGridAppend(g, uiControl(r->path), 1, y, 1, 1, 1, uiAlignFill, 0, uiAlignCenter);
    uiGridAppend(g, uiControl(r->choose), 2, y, 1, 1, 0, uiAlignFill, 0, uiAlignCenter);
    uiGridAppend(g, uiControl(r->eject), 3, y, 1, 1, 0, uiAlignFill, 0, uiAlignCenter);
}

static void disk_row_show(disk_row *r, int on) {
    show(uiControl(r->name), on); show(uiControl(r->path), on);
    show(uiControl(r->choose), on); show(uiControl(r->eject), on);
}

/* ---- the launcher ---------------------------------------------------------------- */

static const char *const machines[] = { "Super QL: 68040 at 40 MHz, SMSQ/E", "Sinclair QL: 68008 at 7.5 MHz, Minerva", NULL };
static const char *const ql_ram_names[] = { "128 KB (as it came)", "256 KB", "384 KB", "512 KB", "640 KB", "896 KB", NULL };
static const int ql_rams[] = { 128, 256, 384, 512, 640, 896 };
static const char *const sql_ram_names[] = { "16 MB", "32 MB", "64 MB", "128 MB", NULL };
static const int sql_rams[] = { 16, 32, 64, 128 };
static const char *const scales[] = { "4:3, as large as the window", "4:3, in whole pixels", "Fill the window", NULL };
static const char *const speeds[] = { "Real time", "2 \xc3\x97", "4 \xc3\x97", "As fast as it goes", NULL };
static const char *const displays[] = { "QL colours, 512 \xc3\x97 256 (mode 4)", "High colour, 512 \xc3\x97 256", "High colour, 1024 \xc3\x97 512", NULL };

static struct launcher {
    settings *cfg;
    uiWindow *win;
    uiCombobox *machine, *ram_ql, *ram_sql, *scale, *speed, *display;
    uiLabel *rom, *display_ql;
    uiButton *rom_choose, *rom_default;
    uiGroup *disks_sql, *disks_ql;
    disk_row win1, flp[2], host, host_ql, mdv[2];
    uiCheckbox *win1_ro, *full, *sound, *at_start;
    uiButton *qle, *fetch, *start, *quit;
    uiLabel *qle_note, *status;
    int result, active;
} L;

static void path_text(uiEntry *e, const char *p, const char *empty) { uiEntrySetText(e, p && p[0] ? p : empty); }

static void launcher_refresh(void) {
    settings *c = L.cfg;
    int sql = c->machine == SET_SUPERQL;
    uiComboboxSetSelected(L.machine, sql ? 0 : 1);
    show(uiControl(L.ram_sql), sql); show(uiControl(L.ram_ql), !sql);
    for (int i = 0; i < 4; i++) if (sql_rams[i] == c->ram_sql_mb) uiComboboxSetSelected(L.ram_sql, i);
    for (int i = 0; i < 6; i++) if (ql_rams[i] == c->ram_ql_kb) uiComboboxSetSelected(L.ram_ql, i);
    const char *own = sql ? c->rom_sql : c->rom_ql;
    char rom[SET_PATH + 64];
    if (own[0]) snprintf(rom, sizeof rom, "%s", own);
    else if (fetched_rom(c->machine)) snprintf(rom, sizeof rom, "%s (fetched)", sql ? "SMSQ/E 3.43" : "Minerva 1.98");
    else snprintf(rom, sizeof rom, "Not fetched yet: use Fetch the ROMs below");
    uiLabelSetText(L.rom, rom);
    enable(uiControl(L.rom_default), own[0] != 0);
    show(uiControl(L.disks_sql), sql); show(uiControl(L.disks_ql), !sql);
    path_text(L.win1.path, c->win1, "Empty");
    uiCheckboxSetChecked(L.win1_ro, c->win1_ro);
    enable(uiControl(L.win1_ro), c->win1[0] != 0);
    path_text(L.flp[0].path, c->flp[0], "Empty");
    path_text(L.flp[1].path, c->flp[1], "Empty");
    path_text(L.host.path, c->host_dir, "None");
    path_text(L.host_ql.path, c->host_dir, "None");
    path_text(L.mdv[0].path, c->mdv[0], "Empty");
    path_text(L.mdv[1].path, c->mdv[1], "Empty");
    int busy = app_fetch_busy();
    const char *qle = fetched_qle();
    if (busy) uiLabelSetText(L.qle_note, "Downloading\xe2\x80\xa6");
    else if (qle && !strcmp(c->win1, qle)) uiLabelSetText(L.qle_note, "QL/E is in WIN1_.");
    else if (qle) uiLabelSetText(L.qle_note, "Downloaded: the button puts it in WIN1_.");
    else uiLabelSetText(L.qle_note, "A QL hard disk full of software, from sinclairql.net (80 MB).");
    uiButtonSetText(L.qle, qle ? "Use QL/E" : "Get QL/E");
    enable(uiControl(L.qle), !busy);
    enable(uiControl(L.fetch), !busy);
    enable(uiControl(L.start), !busy);
    uiComboboxSetSelected(L.scale, c->scale);
    uiComboboxSetSelected(L.speed, c->speed);
    uiComboboxSetSelected(L.display, c->display);
    show(uiControl(L.display), sql); show(uiControl(L.display_ql), !sql);
    uiCheckboxSetChecked(L.full, c->fullscreen);
    uiCheckboxSetChecked(L.sound, c->sound);
    uiCheckboxSetChecked(L.at_start, c->launcher);
    int bad;
    const char *st = app_status(&bad);
    uiLabelSetText(L.status, st);
}

static void on_machine(uiCombobox *b, void *d) { (void)d; L.cfg->machine = uiComboboxSelected(b) == 1 ? SET_QL : SET_SUPERQL; launcher_refresh(); }
static void on_ram(uiCombobox *b, void *d) {
    (void)d;
    int i = uiComboboxSelected(b);
    if (b == L.ram_sql && i >= 0) L.cfg->ram_sql_mb = sql_rams[i];
    if (b == L.ram_ql && i >= 0) L.cfg->ram_ql_kb = ql_rams[i];
}
static void on_scale(uiCombobox *b, void *d) { (void)d; if (uiComboboxSelected(b) >= 0) L.cfg->scale = uiComboboxSelected(b); }
static void on_speed(uiCombobox *b, void *d) { (void)d; if (uiComboboxSelected(b) >= 0) L.cfg->speed = uiComboboxSelected(b); }
static void on_display(uiCombobox *b, void *d) { (void)d; if (uiComboboxSelected(b) >= 0) L.cfg->display = uiComboboxSelected(b); }
static void on_check(uiCheckbox *b, void *d) { *(int *)d = uiCheckboxChecked(b); }
static void on_full(uiCheckbox *b, void *d) { (void)d; L.cfg->fullscreen = uiCheckboxChecked(b); }

static void choose_into(uiWindow *parent, char *target, int folder) {
    char *p = folder ? uiOpenFolder(parent) : uiOpenFile(parent);
    if (!p) return;
    snprintf(target, SET_PATH, "%s", p);
    uiFreeText(p);
}

static void on_disk_choose(uiButton *b, void *d) { (void)b; disk_row *r = d; choose_into(L.win, r->target, r->folder); launcher_refresh(); }
static void on_disk_eject(uiButton *b, void *d) { (void)b; disk_row *r = d; r->target[0] = 0; launcher_refresh(); }
static void on_rom_choose(uiButton *b, void *d) {
    (void)b; (void)d;
    choose_into(L.win, L.cfg->machine == SET_QL ? L.cfg->rom_ql : L.cfg->rom_sql, 0);
    launcher_refresh();
}
static void on_rom_default(uiButton *b, void *d) {
    (void)b; (void)d;
    (L.cfg->machine == SET_QL ? L.cfg->rom_ql : L.cfg->rom_sql)[0] = 0;
    launcher_refresh();
}

static void use_qle(void) {
    settings *c = L.cfg;
    c->machine = SET_SUPERQL;
    c->ram_sql_mb = 128;
    snprintf(c->win1, SET_PATH, "%s", fetched_qle());
    c->win1_ro = 0;
}

static void on_qle(uiButton *b, void *d) {
    (void)b; (void)d;
    if (fetched_qle()) use_qle(); else app_fetch(1);
    launcher_refresh();
}
static void on_fetch(uiButton *b, void *d) { (void)b; (void)d; app_fetch(0); launcher_refresh(); }
static void on_start(uiButton *b, void *d) {
    (void)b; (void)d;
    settings_save(L.cfg, settings_path());
    if (app_start() == 0) L.result = 1;
    else launcher_refresh();
}
static void on_quit(uiButton *b, void *d) { (void)b; (void)d; L.result = 0; }
static int on_close(uiWindow *w, void *d) { (void)w; (void)d; L.result = 0; return 0; }

static int launcher_tick(void *d) {
    if (app_quit_asked()) { L.result = 0; return 1; }
    (void)d;
    if (!L.active) return 1;
    if (app_fetch_done()) {
        if (fetched_qle() && !L.cfg->win1[0]) use_qle();
        launcher_refresh();
    } else if (app_fetch_busy()) {
        int bad;
        uiLabelSetText(L.status, app_status(&bad));
    }
    return 1;
}

static void launcher_build(void) {
    L.win = uiNewWindow("OpenQL", 720, 600, 0);
    uiWindowSetMargined(L.win, 1);
    uiWindowOnClosing(L.win, on_close, NULL);
    uiBox *top = vbox();
    /* An area has no height of its own on any host: a label of empty lines beside
     * it gives the header four lines of the host's font. */
    uiBox *head = hbox();
    uiBoxSetPadded(head, 0);
    uiBoxAppend(head, uiControl(uiNewLabel(" \n \n \n ")), 0);
    uiBoxAppend(head, uiControl(uiNewArea(&header_handler)), 1);
    uiBoxAppend(top, uiControl(head), 0);
    uiBox *body = top;

    /* the machine */
    uiForm *mf = uiNewForm();
    uiFormSetPadded(mf, 1);
    L.machine = combo(machines);
    uiComboboxOnSelected(L.machine, on_machine, NULL);
    uiFormAppend(mf, "Machine", uiControl(L.machine), 0);
    uiBox *ram = hbox();
    L.ram_sql = combo(sql_ram_names);
    L.ram_ql = combo(ql_ram_names);
    uiComboboxOnSelected(L.ram_sql, on_ram, NULL);
    uiComboboxOnSelected(L.ram_ql, on_ram, NULL);
    uiBoxAppend(ram, uiControl(L.ram_sql), 1);
    uiBoxAppend(ram, uiControl(L.ram_ql), 1);
    uiFormAppend(mf, "Memory", uiControl(ram), 0);
    uiBox *rom = hbox();
    L.rom = uiNewLabel("");
    L.rom_choose = uiNewButton("Choose\xe2\x80\xa6");
    L.rom_default = uiNewButton("Default");
    uiButtonOnClicked(L.rom_choose, on_rom_choose, NULL);
    uiButtonOnClicked(L.rom_default, on_rom_default, NULL);
    uiBoxAppend(rom, uiControl(L.rom), 1);
    uiBoxAppend(rom, uiControl(L.rom_choose), 0);
    uiBoxAppend(rom, uiControl(L.rom_default), 0);
    uiFormAppend(mf, "ROM", uiControl(rom), 0);
    uiBoxAppend(body, uiControl(group("Machine", uiControl(mf))), 0);

    /* the Super QL's disks */
    uiBox *sd = vbox();
    uiBox *qle = hbox();
    L.qle = uiNewButton("Get QL/E");
    uiButtonOnClicked(L.qle, on_qle, NULL);
    L.qle_note = uiNewLabel("");
    uiBoxAppend(qle, uiControl(L.qle), 0);
    uiBoxAppend(qle, uiControl(L.qle_note), 1);
    uiBoxAppend(sd, uiControl(qle), 0);
    uiGrid *sg = uiNewGrid();
    uiGridSetPadded(sg, 1);
    disk_row_add(sg, 0, &L.win1, "WIN1_ hard disk");
    L.win1.target = L.cfg->win1;
    L.win1_ro = uiNewCheckbox("Read only");
    uiCheckboxOnToggled(L.win1_ro, on_check, &L.cfg->win1_ro);
    uiGridAppend(sg, uiControl(L.win1_ro), 1, 1, 1, 1, 1, uiAlignStart, 0, uiAlignCenter);
    disk_row_add(sg, 2, &L.flp[0], "FLP1_ floppy");
    disk_row_add(sg, 3, &L.flp[1], "FLP2_ floppy");
    disk_row_add(sg, 4, &L.host, "Host folder as WIN2_");
    L.flp[0].target = L.cfg->flp[0];
    L.flp[1].target = L.cfg->flp[1];
    L.host.target = L.cfg->host_dir;
    L.host.folder = 1;
    uiBoxAppend(sd, uiControl(sg), 0);
    L.disks_sql = group("Disks", uiControl(sd));
    uiBoxAppend(body, uiControl(L.disks_sql), 0);

    /* the QL's cartridges */
    uiGrid *qg = uiNewGrid();
    uiGridSetPadded(qg, 1);
    disk_row_add(qg, 0, &L.mdv[0], "mdv1_ cartridge");
    disk_row_add(qg, 1, &L.mdv[1], "mdv2_ cartridge");
    disk_row_add(qg, 2, &L.host_ql, "Host folder as DOS1_");
    L.mdv[0].target = L.cfg->mdv[0];
    L.mdv[1].target = L.cfg->mdv[1];
    L.host_ql.target = L.cfg->host_dir;
    L.host_ql.folder = 1;
    L.disks_ql = group("Drives", uiControl(qg));
    uiBoxAppend(body, uiControl(L.disks_ql), 0);
    disk_row *rows[] = { &L.win1, &L.flp[0], &L.flp[1], &L.host, &L.host_ql, &L.mdv[0], &L.mdv[1] };
    for (int i = 0; i < 7; i++) {
        uiButtonOnClicked(rows[i]->choose, on_disk_choose, rows[i]);
        uiButtonOnClicked(rows[i]->eject, on_disk_eject, rows[i]);
        rows[i]->drive = -1;
    }

    /* the picture and sound */
    uiForm *pf = uiNewForm();
    uiFormSetPadded(pf, 1);
    L.scale = combo(scales);
    uiComboboxOnSelected(L.scale, on_scale, NULL);
    uiFormAppend(pf, "Picture", uiControl(L.scale), 0);
    L.speed = combo(speeds);
    uiComboboxOnSelected(L.speed, on_speed, NULL);
    uiFormAppend(pf, "Speed", uiControl(L.speed), 0);
    uiBox *disp = hbox();
    L.display = combo(displays);
    uiComboboxOnSelected(L.display, on_display, NULL);
    L.display_ql = uiNewLabel("QL modes 4 and 8, as the QL has them");
    uiBoxAppend(disp, uiControl(L.display), 1);
    uiBoxAppend(disp, uiControl(L.display_ql), 1);
    uiFormAppend(pf, "Display", uiControl(disp), 0);
    uiBox *checks = hbox();
    L.full = uiNewCheckbox("Full screen");
    L.sound = uiNewCheckbox("Sound");
    L.at_start = uiNewCheckbox("Show this window at start");
    uiCheckboxOnToggled(L.full, on_full, NULL);
    uiCheckboxOnToggled(L.sound, on_check, &L.cfg->sound);
    uiCheckboxOnToggled(L.at_start, on_check, &L.cfg->launcher);
    uiBoxAppend(checks, uiControl(L.full), 0);
    uiBoxAppend(checks, uiControl(L.sound), 0);
    uiBoxAppend(checks, uiControl(L.at_start), 0);
    uiFormAppend(pf, "", uiControl(checks), 0);
    uiBoxAppend(body, uiControl(group("Picture and sound", uiControl(pf))), 0);

    /* the status and the buttons */
    uiBox *bar = hbox();
    L.status = uiNewLabel("");
    L.fetch = uiNewButton("Fetch the ROMs");
    L.quit = uiNewButton("Quit");
    L.start = uiNewButton("Start");
    uiButtonOnClicked(L.fetch, on_fetch, NULL);
    uiButtonOnClicked(L.quit, on_quit, NULL);
    uiButtonOnClicked(L.start, on_start, NULL);
    uiBoxAppend(bar, uiControl(L.status), 1);
    uiBoxAppend(bar, uiControl(L.fetch), 0);
    uiBoxAppend(bar, uiControl(L.quit), 0);
    uiBoxAppend(bar, uiControl(L.start), 0);
    uiBoxAppend(body, uiControl(bar), 0);

    uiWindowSetChild(L.win, uiControl(top));
    uiTimer(200, launcher_tick, NULL);
}

int gui_launcher(settings *cfg) {
    L.cfg = cfg;
    if (!L.win) launcher_build();
    L.result = -1;
    L.active = 1;
    launcher_refresh();
    uiControlShow(uiControl(L.win));
    while (L.result < 0 && uiMainStep(1)) {}
    L.active = 0;
    uiControlHide(uiControl(L.win));
    gui_pump();
    return L.result > 0;
}

/* ---- the menu ---------------------------------------------------------------------- */

static struct menu {
    settings *cfg;
    uiWindow *win;
    disk_row disk[2], hostdir;
    uiButton *blank, *host_write;
    uiLabel *win1;
    uiCombobox *slot, *speed, *scale, *display;
    uiLabel *display_note;
    uiButton *save, *load, *paste, *reset, *power, *settings, *quit, *resume, *debugger;
    uiCheckbox *full;
    uiLabel *status;
    int result;
} M;

static void menu_refresh(void) {
    int ql = app_is_ql();
    char t[64];
    uiWindowSetTitle(M.win, ql ? "OpenQL: Sinclair QL" : "OpenQL: Super QL");
    for (int i = 0; i < 2; i++) {
        snprintf(t, sizeof t, ql ? "mdv%d_" : "FLP%d_", i + 1);
        uiLabelSetText(M.disk[i].name, t);
        const char *in = ql ? M.cfg->mdv[i] : M.cfg->flp[i];
        path_text(M.disk[i].path, app_drive_in(i) ? in : "", "Empty");
        enable(uiControl(M.disk[i].eject), app_drive_in(i));
        M.disk[i].target = ql ? M.cfg->mdv[i] : M.cfg->flp[i];
    }
    show(uiControl(M.blank), ql);
    /* the host folder: WIN2_ on the Super QL, DOS1_ on the QL */
    snprintf(t, sizeof t, "%s host folder", app_hostdir_drive());
    uiLabelSetText(M.hostdir.name, t);
    path_text(M.hostdir.path, app_hostdir_in() ? M.cfg->host_dir : "", "None");
    enable(uiControl(M.hostdir.eject), app_hostdir_in());
    show(uiControl(M.host_write), !ql);
    enable(uiControl(M.host_write), app_hostdir_in());
    show(uiControl(M.win1), !ql);
    char w[SET_PATH + 16];
    snprintf(w, sizeof w, "WIN1_: %s", M.cfg->win1[0] ? M.cfg->win1 : "none (choose one in Settings)");
    uiLabelSetText(M.win1, w);
    int slot = uiComboboxSelected(M.slot) + 1;
    enable(uiControl(M.load), app_state_exists(slot));
    uiComboboxSetSelected(M.speed, M.cfg->speed);
    uiComboboxSetSelected(M.scale, M.cfg->scale);
    uiComboboxSetSelected(M.display, M.cfg->display);
    show(uiControl(M.display), !ql);
    uiLabelSetText(M.display_note, ql ? "QL modes 4 and 8, as the QL has them" : "from the next reset");
    uiCheckboxSetChecked(M.full, M.cfg->fullscreen);
    int bad;
    uiLabelSetText(M.status, app_status(&bad));
}

static void m_choose(uiButton *b, void *d) {
    (void)b;
    disk_row *r = d;
    char *p = uiOpenFile(M.win);
    if (p) { app_insert(r->drive, p); uiFreeText(p); }
    menu_refresh();
}
static void m_eject(uiButton *b, void *d) { (void)b; disk_row *r = d; app_eject(r->drive); menu_refresh(); }
static void m_blank(uiButton *b, void *d) { (void)b; (void)d; app_blank_cartridge(); menu_refresh(); }
static void m_host(uiButton *b, void *d) {
    (void)b; (void)d;
    char *p = uiOpenFolder(M.win);
    if (p) { app_hostdir_insert(p); uiFreeText(p); }
    menu_refresh();
}
static void m_host_remove(uiButton *b, void *d) { (void)b; (void)d; app_hostdir_remove(); menu_refresh(); }
static void m_host_write(uiButton *b, void *d) { (void)b; (void)d; app_hostdir_write(); menu_refresh(); }
static void m_slot(uiCombobox *c, void *d) { (void)c; (void)d; menu_refresh(); }
static void m_save(uiButton *b, void *d) { (void)b; (void)d; app_state(1, uiComboboxSelected(M.slot) + 1); menu_refresh(); }
static void m_load(uiButton *b, void *d) {
    (void)b; (void)d;
    if (app_state(0, uiComboboxSelected(M.slot) + 1) == 0) M.result = GUI_RESUME;
    else menu_refresh();
}
static void m_paste(uiButton *b, void *d) { (void)b; (void)d; app_paste(); M.result = GUI_RESUME; }
static void m_speed(uiCombobox *c, void *d) { (void)d; if (uiComboboxSelected(c) >= 0) M.cfg->speed = uiComboboxSelected(c); }
static void m_scale(uiCombobox *c, void *d) { (void)d; if (uiComboboxSelected(c) >= 0) M.cfg->scale = uiComboboxSelected(c); app_redraw(); }
static void m_full(uiCheckbox *c, void *d) { (void)d; M.cfg->fullscreen = uiCheckboxChecked(c); }
static void m_display(uiCombobox *c, void *d) {
    (void)d;
    if (uiComboboxSelected(c) < 0) return;
    M.cfg->display = uiComboboxSelected(c);
    app_display(M.cfg->display);
    menu_refresh();
}
static void m_debugger(uiButton *b, void *d) { (void)b; (void)d; M.result = GUI_DEBUG; }
static void m_reset(uiButton *b, void *d) { (void)b; app_reset(d != NULL); M.result = GUI_RESUME; }
static void m_settings(uiButton *b, void *d) { (void)b; (void)d; M.result = GUI_SETTINGS; }
static void m_quit(uiButton *b, void *d) { (void)b; (void)d; M.result = GUI_QUIT; }
static void m_resume(uiButton *b, void *d) { (void)b; (void)d; M.result = GUI_RESUME; }
static int m_close(uiWindow *w, void *d) { (void)w; (void)d; M.result = GUI_RESUME; return 0; }

static void menu_build(void) {
    M.win = uiNewWindow("OpenQL", 560, 420, 0);
    uiWindowSetMargined(M.win, 1);
    uiWindowOnClosing(M.win, m_close, NULL);
    uiBox *body = vbox();

    uiBox *dv = vbox();
    uiGrid *g = uiNewGrid();
    uiGridSetPadded(g, 1);
    for (int i = 0; i < 2; i++) {
        disk_row_add(g, i, &M.disk[i], i ? "FLP2_" : "FLP1_");
        M.disk[i].drive = i;
        uiButtonOnClicked(M.disk[i].choose, m_choose, &M.disk[i]);
        uiButtonOnClicked(M.disk[i].eject, m_eject, &M.disk[i]);
    }
    /* the host folder; on the Super QL a change starts SMSQ/E again */
    disk_row_add(g, 2, &M.hostdir, "Host folder");
    M.hostdir.drive = -1;
    uiButtonOnClicked(M.hostdir.choose, m_host, NULL);
    uiButtonOnClicked(M.hostdir.eject, m_host_remove, NULL);
    uiButtonSetText(M.hostdir.eject, "Remove");
    uiBoxAppend(dv, uiControl(g), 0);
    uiBox *more = hbox();
    M.blank = uiNewButton("New blank cartridge");
    M.host_write = uiNewButton("Write WIN2_ to the folder now");
    uiButtonOnClicked(M.blank, m_blank, NULL);
    uiButtonOnClicked(M.host_write, m_host_write, NULL);
    uiBoxAppend(more, uiControl(M.blank), 0);
    uiBoxAppend(more, uiControl(M.host_write), 0);
    uiBoxAppend(dv, uiControl(more), 0);
    M.win1 = uiNewLabel("");
    uiBoxAppend(dv, uiControl(M.win1), 0);
    uiBoxAppend(body, uiControl(group("Disks", uiControl(dv))), 0);

    uiBox *sv = hbox();
    static const char *const slots[] = { "Slot 1", "Slot 2", "Slot 3", "Slot 4", NULL };
    M.slot = combo(slots);
    uiComboboxSetSelected(M.slot, 0);
    uiComboboxOnSelected(M.slot, m_slot, NULL);
    M.save = uiNewButton("Save");
    M.load = uiNewButton("Load");
    uiButtonOnClicked(M.save, m_save, NULL);
    uiButtonOnClicked(M.load, m_load, NULL);
    uiBoxAppend(sv, uiControl(M.slot), 1);
    uiBoxAppend(sv, uiControl(M.save), 0);
    uiBoxAppend(sv, uiControl(M.load), 0);
    uiBoxAppend(body, uiControl(group("Saved state", uiControl(sv))), 0);

    uiForm *f = uiNewForm();
    uiFormSetPadded(f, 1);
    M.speed = combo(speeds);
    uiComboboxOnSelected(M.speed, m_speed, NULL);
    uiFormAppend(f, "Speed", uiControl(M.speed), 0);
    M.scale = combo(scales);
    uiComboboxOnSelected(M.scale, m_scale, NULL);
    uiFormAppend(f, "Picture", uiControl(M.scale), 0);
    uiBox *disp = hbox();
    M.display = combo(displays);
    uiComboboxOnSelected(M.display, m_display, NULL);
    M.display_note = uiNewLabel("");
    uiBoxAppend(disp, uiControl(M.display), 0);
    uiBoxAppend(disp, uiControl(M.display_note), 1);
    uiFormAppend(f, "Display", uiControl(disp), 0);
    M.full = uiNewCheckbox("Full screen");
    uiCheckboxOnToggled(M.full, m_full, NULL);
    uiFormAppend(f, "", uiControl(M.full), 0);
    uiBox *acts = hbox();
    M.paste = uiNewButton("Type the clipboard in");
    M.reset = uiNewButton("Reset");
    M.power = uiNewButton("Power off and on");
    uiButtonOnClicked(M.paste, m_paste, NULL);
    uiButtonOnClicked(M.reset, m_reset, NULL);
    uiButtonOnClicked(M.power, m_reset, (void *)1);
    uiBoxAppend(acts, uiControl(M.paste), 0);
    uiBoxAppend(acts, uiControl(M.reset), 0);
    uiBoxAppend(acts, uiControl(M.power), 0);
    uiFormAppend(f, "", uiControl(acts), 0);
    uiBoxAppend(body, uiControl(group("Machine", uiControl(f))), 0);

    uiBox *bar = hbox();
    M.status = uiNewLabel("");
    M.settings = uiNewButton("Settings\xe2\x80\xa6");
    M.debugger = uiNewButton("Debugger\xe2\x80\xa6");
    uiButtonOnClicked(M.debugger, m_debugger, NULL);
    M.quit = uiNewButton("Quit");
    M.resume = uiNewButton("Back to the machine");
    uiButtonOnClicked(M.settings, m_settings, NULL);
    uiButtonOnClicked(M.quit, m_quit, NULL);
    uiButtonOnClicked(M.resume, m_resume, NULL);
    uiBoxAppend(bar, uiControl(M.status), 1);
    uiBoxAppend(bar, uiControl(M.debugger), 0);
    uiBoxAppend(bar, uiControl(M.settings), 0);
    uiBoxAppend(bar, uiControl(M.quit), 0);
    uiBoxAppend(bar, uiControl(M.resume), 0);
    uiBoxAppend(body, uiControl(bar), 0);
    uiWindowSetChild(M.win, uiControl(body));
}

int gui_menu(settings *cfg) {
    M.cfg = cfg;
    if (!M.win) menu_build();
    M.result = -1;
    menu_refresh();
    uiControlShow(uiControl(M.win));
    while (M.result < 0) {
        gui_pump();
        int k = app_menu_key();
        if (k == 2) M.result = GUI_QUIT;
        else if (k == 1) M.result = GUI_RESUME;
        else if (M.result < 0) {
            app_redraw();
            uiMainStep(0);
        }
    }
    uiControlHide(uiControl(M.win));
    gui_pump();
    return M.result;
}

/* ---- the debugger ----------------------------------------------------------------- */

/* The registers, editable while the CPU is stopped; the code about the PC
 * (or wherever was asked) and memory, drawn in the host's fixed-width font
 * in two areas. Click a line of code to choose it, double-click it for a
 * breakpoint; once clicked, the arrow and page keys move through either. */

#if defined(_WIN32)
#define MONO "Consolas"
#elif defined(__APPLE__)
#define MONO "Menlo"
#else
#define MONO "Monospace"
#endif

enum { NREGS = 21, MAXROWS = 160 };
static const char *const reg_names[NREGS] = {
    "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7", "a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7",
    "pc", "sr", "usp", "ssp", "vbr",
};

static struct debugger {
    uiWindow *win;
    int open, building;
    uiButton *brk, *cont, *step, *over, *run_to, *bp, *bp_clear, *go_code, *go_mem, *poke;
    uiEntry *reg[NREGS];
    uiLabel *flags, *status, *bps, *count;
    uiEntry *code_at, *mem_at, *poke_bytes;
    uiCheckbox *follow;
    uiArea *code, *mem;
    uint32_t code_top, mem_top, chosen;   /* chosen: the line clicked (odd: none) */
    uint32_t lines_at[MAXROWS];           /* the code's lines' addresses, as last drawn */
    int nlines, mem_rows;
    double line_h;
    int was_stopped;
    uint64_t was_ins;
    unsigned ticks;
} D = { .chosen = 1 };

static uiFontDescriptor mono_font(void) {
    static char fam[] = MONO;
    uiFontDescriptor f;
    f.Family = fam;
    f.Size = 10;
    f.Weight = uiTextWeightNormal;
    f.Italic = uiTextItalicNormal;
    f.Stretch = uiTextStretchNormal;
    return f;
}

/* text at x, y (its top) in the fixed-width font; its height, and its width in *w */
static double mono_text(uiDrawContext *c, const char *t, double x, double y, double r, double g, double b, double *w) {
    uiFontDescriptor f = mono_font();
    uiAttributedString *as = uiNewAttributedString(t);
    if (t[0]) uiAttributedStringSetAttribute(as, uiNewColorAttribute(r, g, b, 1), 0, strlen(t));
    uiDrawTextLayoutParams p = { as, &f, 4000, uiDrawTextAlignLeft };
    uiDrawTextLayout *tl = uiDrawNewTextLayout(&p);
    double tw, th;
    uiDrawTextLayoutExtents(tl, &tw, &th);
    if (c) uiDrawText(c, tl, x, y);
    uiDrawFreeTextLayout(tl);
    uiFreeAttributedString(as);
    if (w) *w = tw;
    return th;
}

static double line_height(void) {
    if (D.line_h <= 0) {
        double h = mono_text(NULL, "Mg$0", 0, 0, 0, 0, 0, NULL);
        D.line_h = h > 4 ? h + 2 : 16;
    }
    return D.line_h;
}

static void code_draw(uiAreaHandler *h, uiArea *a, uiAreaDrawParams *p) {
    (void)h; (void)a;
    ac090 *c = app_cpu();
    fill(p->Context, 0, 0, p->AreaWidth, p->AreaHeight, 1, 1, 1);
    D.nlines = 0;
    if (!c) return;
    double lh = line_height(), mark = lh * 0.9;
    int rows = (int)(p->AreaHeight / lh);
    if (rows < 1) rows = 1;
    if (rows > MAXROWS) rows = MAXROWS;
    uint32_t at = D.code_top;
    for (int i = 0; i < rows; i++) {
        char t[200], hex[48] = "", line[300];
        int len = dbg_disasm(c, at, t, sizeof t);
        for (int k = 0; k < len && k < 10; k += 2) {
            uint8_t hi, lo;
            size_t n = strlen(hex);
            if (dbg_peek(c, at + (uint32_t)k, &hi) && dbg_peek(c, at + (uint32_t)k + 1, &lo)) snprintf(hex + n, sizeof hex - n, "%02x%02x ", hi, lo);
            else snprintf(hex + n, sizeof hex - n, "---- ");
        }
        if (len > 10) strncat(hex, "+", sizeof hex - strlen(hex) - 1);
        double y = i * lh;
        int here = at == c->pc && !c->halted;
        if (here) fill(p->Context, 0, y, p->AreaWidth, lh, 0.78, 0.78, 0.79);     /* the QL's silver */
        else if (at == D.chosen) fill(p->Context, 0, y, p->AreaWidth, lh, 0.91, 0.91, 0.92);
        if (dbg_bp_at(c, at)) mono_text(p->Context, "\xe2\x97\x8f", 4, y + 1, 0.80, 0.06, 0.06, NULL);
        if (here) mono_text(p->Context, "\xe2\x96\xb6", 4 + mark, y + 1, 0, 0, 0, NULL);
        snprintf(line, sizeof line, "%08x  %-22s%s", at, hex, t);
        mono_text(p->Context, line, 4 + 2 * mark, y + 1, 0.05, 0.05, 0.05, NULL);
        D.lines_at[i] = at;
        at += (uint32_t)len;
    }
    D.nlines = rows;
}

static void mem_draw(uiAreaHandler *h, uiArea *a, uiAreaDrawParams *p) {
    (void)h; (void)a;
    ac090 *c = app_cpu();
    fill(p->Context, 0, 0, p->AreaWidth, p->AreaHeight, 1, 1, 1);
    if (!c) return;
    double lh = line_height();
    int rows = (int)(p->AreaHeight / lh);
    if (rows < 1) rows = 1;
    D.mem_rows = rows;
    for (int i = 0; i < rows; i++) {
        uint32_t at = D.mem_top + 16u * (uint32_t)i;
        char line[120], asc[17];
        int n = snprintf(line, sizeof line, "%08x ", at);
        for (int k = 0; k < 16; k++) {
            uint8_t v;
            if (dbg_peek(c, at + (uint32_t)k, &v)) {
                n += snprintf(line + n, sizeof line - (size_t)n, "%s%02x", k == 8 ? "  " : " ", v);
                asc[k] = v >= 32 && v < 127 ? (char)v : '.';
            } else {
                n += snprintf(line + n, sizeof line - (size_t)n, "%s--", k == 8 ? "  " : " ");
                asc[k] = ' ';
            }
        }
        asc[16] = 0;
        snprintf(line + n, sizeof line - (size_t)n, "  %s", asc);
        mono_text(p->Context, line, 4, i * lh + 1, 0.05, 0.05, 0.05, NULL);
    }
}

static void debug_refresh(int all);

static void follow_off(void) { uiCheckboxSetChecked(D.follow, 0); }

static void code_mouse(uiAreaHandler *h, uiArea *a, uiAreaMouseEvent *e) {
    (void)h; (void)a;
    ac090 *c = app_cpu();
    if (!c || e->Down != 1) return;
    int row = (int)(e->Y / line_height());
    if (row < 0 || row >= D.nlines) return;
    D.chosen = D.lines_at[row];
    char t[16];
    snprintf(t, sizeof t, "%08x", D.chosen);
    uiEntrySetText(D.code_at, t);
    if (e->Count >= 2) dbg_bp_toggle(c, D.chosen);
    debug_refresh(0);
}

static int code_key(uiAreaHandler *h, uiArea *a, uiAreaKeyEvent *e) {
    (void)h; (void)a;
    ac090 *c = app_cpu();
    if (!c || e->Up) return 0;
    char t[200];
    int page = D.nlines > 2 ? D.nlines - 2 : 1;
    switch (e->ExtKey) {
    case uiExtKeyUp: D.code_top = dbg_back(c, D.code_top, 1); break;
    case uiExtKeyDown: D.code_top += (uint32_t)dbg_disasm(c, D.code_top, t, sizeof t); break;
    case uiExtKeyPageUp: D.code_top = dbg_back(c, D.code_top, page); break;
    case uiExtKeyPageDown: for (int i = 0; i < page; i++) D.code_top += (uint32_t)dbg_disasm(c, D.code_top, t, sizeof t); break;
    default: return 0;
    }
    follow_off();
    uiAreaQueueRedrawAll(D.code);
    return 1;
}

static int mem_key(uiAreaHandler *h, uiArea *a, uiAreaKeyEvent *e) {
    (void)h; (void)a;
    if (e->Up) return 0;
    uint32_t page = 16u * (uint32_t)(D.mem_rows > 2 ? D.mem_rows - 2 : 1);
    switch (e->ExtKey) {
    case uiExtKeyUp: D.mem_top -= 16; break;
    case uiExtKeyDown: D.mem_top += 16; break;
    case uiExtKeyPageUp: D.mem_top -= page; break;
    case uiExtKeyPageDown: D.mem_top += page; break;
    default: return 0;
    }
    char t[16];
    snprintf(t, sizeof t, "%08x", D.mem_top);
    uiEntrySetText(D.mem_at, t);
    uiAreaQueueRedrawAll(D.mem);
    return 1;
}

static uiAreaHandler code_handler = { code_draw, code_mouse, no_cross, no_drag, code_key };
static uiAreaHandler mem_handler = { mem_draw, no_mouse, no_cross, no_drag, mem_key };

/* an address typed in an entry: 0, or -1 with the status saying so */
static int entry_addr(uiEntry *e, uint32_t *v) {
    ac090 *c = app_cpu();
    char *t = uiEntryText(e);
    int rc = c ? dbg_parse(c, t, v) : -1;
    if (rc) {
        char m[160];
        snprintf(m, sizeof m, "\"%.60s\" is not an address: hex, a register, or one with +/- hex", t);
        uiLabelSetText(D.status, m);
    }
    uiFreeText(t);
    return rc;
}

/* the line chosen, else the code address typed */
static int target(uint32_t *v) {
    if (!(D.chosen & 1)) { *v = D.chosen; return 0; }
    return entry_addr(D.code_at, v);
}

static void d_break(uiButton *b, void *d) { (void)b; (void)d; if (app_cpu()) dbg_break(app_cpu()); debug_refresh(1); }
static void d_cont(uiButton *b, void *d) { (void)b; (void)d; if (app_cpu()) dbg_continue(app_cpu()); debug_refresh(1); }
static void d_step(uiButton *b, void *d) { (void)b; (void)d; if (app_cpu()) dbg_step(app_cpu()); }
static void d_over(uiButton *b, void *d) { (void)b; (void)d; if (app_cpu()) dbg_step_over(app_cpu()); }
static void d_run_to(uiButton *b, void *d) {
    (void)b; (void)d;
    uint32_t a;
    if (app_cpu() && target(&a) == 0) { dbg_run_to(app_cpu(), a); debug_refresh(1); }
}
static void d_bp(uiButton *b, void *d) {
    (void)b; (void)d;
    uint32_t a;
    if (app_cpu() && target(&a) == 0 && dbg_bp_toggle(app_cpu(), a) < 0) uiLabelSetText(D.status, "No more breakpoints: clear some first");
    debug_refresh(0);
}
static void d_bp_clear(uiButton *b, void *d) { (void)b; (void)d; if (app_cpu()) dbg_bp_clear(app_cpu()); debug_refresh(0); }
static void d_go_code(uiButton *b, void *d) {
    (void)b; (void)d;
    uint32_t a;
    if (entry_addr(D.code_at, &a) == 0) { D.code_top = a & ~1u; D.chosen = 1; follow_off(); uiAreaQueueRedrawAll(D.code); }
}
static void d_go_mem(uiButton *b, void *d) {
    (void)b; (void)d;
    uint32_t a;
    if (entry_addr(D.mem_at, &a) == 0) { D.mem_top = a & ~15u; uiAreaQueueRedrawAll(D.mem); }
}
static void d_follow(uiCheckbox *c, void *d) { (void)c; (void)d; debug_refresh(0); }

/* bytes in hex ("4e 71", "4e71") written from the memory address on */
static void d_poke(uiButton *b, void *d) {
    (void)b; (void)d;
    ac090 *c = app_cpu();
    uint32_t a;
    if (!c || entry_addr(D.mem_at, &a)) return;
    char *t = uiEntryText(D.poke_bytes), m[120];
    int n = 0, bad = 0, nib = -1;
    for (const char *p = t; *p && !bad; p++) {
        int v = *p >= '0' && *p <= '9' ? *p - '0' : *p >= 'a' && *p <= 'f' ? *p - 'a' + 10 : *p >= 'A' && *p <= 'F' ? *p - 'A' + 10 : -1;
        if (v < 0) { if (*p == ' ' || *p == ',' || *p == '$') { if (nib >= 0) bad = 1; continue; } bad = 1; break; }
        if (nib < 0) { nib = v; continue; }
        if (!dbg_poke(c, a + (uint32_t)n, (uint8_t)(nib << 4 | v))) { bad = 2; break; }
        n++;
        nib = -1;
    }
    if (nib >= 0 && !bad) bad = 1;
    uiFreeText(t);
    if (bad == 2) snprintf(m, sizeof m, "%d bytes written; $%08x is not RAM", n, a + (uint32_t)n);
    else if (bad) snprintf(m, sizeof m, "Bytes in hex, two digits each: 4e 71");
    else snprintf(m, sizeof m, "%d byte%s written at $%08x", n, n == 1 ? "" : "s", a);
    uiLabelSetText(D.status, m);
    uiAreaQueueRedrawAll(D.mem);
    uiAreaQueueRedrawAll(D.code);
}

static void d_reg(uiEntry *e, void *d) {
    const char *name = d;
    ac090 *c = app_cpu();
    if (!c || !dbg_stopped(c) || D.building) return;
    char *t = uiEntryText(e), *p = t, *end;
    while (*p == ' ' || *p == '$') p++;
    unsigned long v = strtoul(p, &end, 16);
    int ok = end != p && *end == 0;
    uiFreeText(t);
    if (!ok) return;
    dbg_set_reg(c, name, (uint32_t)v);
    char fl[40];
    dbg_sr_text(ac090_get_sr(c), fl, sizeof fl);
    uiLabelSetText(D.flags, fl);
    if (!strcmp(name, "pc") || !strcmp(name, "sr")) uiAreaQueueRedrawAll(D.code);
}

static int d_close(uiWindow *w, void *d) {
    (void)w; (void)d;
    uiControlHide(uiControl(D.win));
    D.open = 0;
    if (app_cpu()) dbg_release(app_cpu());
    return 0;
}

static void debug_build(void) {
    D.building = 1;
    D.win = uiNewWindow("OpenQL: Debugger", 1000, 720, 0);
    uiWindowSetMargined(D.win, 1);
    uiWindowOnClosing(D.win, d_close, NULL);
    uiBox *top = vbox();

    uiBox *bar = hbox();
    D.brk = uiNewButton("Break");
    D.cont = uiNewButton("Continue");
    D.step = uiNewButton("Step");
    D.over = uiNewButton("Step over");
    uiButtonOnClicked(D.brk, d_break, NULL);
    uiButtonOnClicked(D.cont, d_cont, NULL);
    uiButtonOnClicked(D.step, d_step, NULL);
    uiButtonOnClicked(D.over, d_over, NULL);
    D.status = uiNewLabel("");
    D.count = uiNewLabel("");
    uiBoxAppend(bar, uiControl(D.brk), 0);
    uiBoxAppend(bar, uiControl(D.cont), 0);
    uiBoxAppend(bar, uiControl(D.step), 0);
    uiBoxAppend(bar, uiControl(D.over), 0);
    uiBoxAppend(bar, uiControl(D.status), 1);
    uiBoxAppend(bar, uiControl(D.count), 0);
    uiBoxAppend(top, uiControl(bar), 0);

    uiBox *body = hbox();
    uiBox *left = vbox();
    uiGrid *g = uiNewGrid();
    uiGridSetPadded(g, 1);
    for (int i = 0; i < NREGS; i++) {
        char up[8];
        snprintf(up, sizeof up, "%s", reg_names[i]);
        for (char *q = up; *q; q++) *q = (char)(*q >= 'a' && *q <= 'z' ? *q - 32 : *q);
        D.reg[i] = uiNewEntry();
        uiEntryOnChanged(D.reg[i], d_reg, (void *)reg_names[i]);
        int col = i < 16 ? (i >= 8) * 2 : (i - 16) % 2 * 2, row = i < 16 ? i % 8 : 8 + (i - 16) / 2;
        uiGridAppend(g, uiControl(uiNewLabel(up)), col, row, 1, 1, 0, uiAlignEnd, 0, uiAlignCenter);
        uiGridAppend(g, uiControl(D.reg[i]), col + 1, row, 1, 1, 1, uiAlignFill, 0, uiAlignCenter);
    }
    D.flags = uiNewLabel("");
    uiGridAppend(g, uiControl(D.flags), 2, 10, 2, 1, 1, uiAlignStart, 0, uiAlignCenter);
    uiBoxAppend(left, uiControl(group("Registers", uiControl(g))), 0);
    uiBox *bv = vbox();
    D.bps = uiNewLabel("");
    D.bp_clear = uiNewButton("Clear them all");
    uiButtonOnClicked(D.bp_clear, d_bp_clear, NULL);
    uiBoxAppend(bv, uiControl(D.bps), 1);
    uiBoxAppend(bv, uiControl(D.bp_clear), 0);
    uiBoxAppend(left, uiControl(group("Breakpoints", uiControl(bv))), 1);
    uiBoxAppend(body, uiControl(left), 0);

    uiBox *right = vbox();
    uiBox *cv = vbox();
    uiBox *crow = hbox();
    D.code_at = uiNewEntry();
    D.go_code = uiNewButton("Show");
    D.follow = uiNewCheckbox("Follow the PC");
    uiCheckboxSetChecked(D.follow, 1);
    D.bp = uiNewButton("Breakpoint");
    D.run_to = uiNewButton("Run to here");
    uiButtonOnClicked(D.go_code, d_go_code, NULL);
    uiCheckboxOnToggled(D.follow, d_follow, NULL);
    uiButtonOnClicked(D.bp, d_bp, NULL);
    uiButtonOnClicked(D.run_to, d_run_to, NULL);
    uiBoxAppend(crow, uiControl(uiNewLabel("Address")), 0);
    uiBoxAppend(crow, uiControl(D.code_at), 1);
    uiBoxAppend(crow, uiControl(D.go_code), 0);
    uiBoxAppend(crow, uiControl(D.follow), 0);
    uiBoxAppend(crow, uiControl(D.bp), 0);
    uiBoxAppend(crow, uiControl(D.run_to), 0);
    uiBoxAppend(cv, uiControl(crow), 0);
    D.code = uiNewArea(&code_handler);
    uiBoxAppend(cv, uiControl(D.code), 1);
    uiBoxAppend(right, uiControl(group("Code", uiControl(cv))), 1);

    uiBox *mv = vbox();
    uiBox *mrow = hbox();
    D.mem_at = uiNewEntry();
    D.go_mem = uiNewButton("Show");
    D.poke_bytes = uiNewEntry();
    D.poke = uiNewButton("Write");
    uiButtonOnClicked(D.go_mem, d_go_mem, NULL);
    uiButtonOnClicked(D.poke, d_poke, NULL);
    uiBoxAppend(mrow, uiControl(uiNewLabel("Address")), 0);
    uiBoxAppend(mrow, uiControl(D.mem_at), 1);
    uiBoxAppend(mrow, uiControl(D.go_mem), 0);
    uiBoxAppend(mrow, uiControl(uiNewLabel("Bytes")), 0);
    uiBoxAppend(mrow, uiControl(D.poke_bytes), 1);
    uiBoxAppend(mrow, uiControl(D.poke), 0);
    uiBoxAppend(mv, uiControl(mrow), 0);
    D.mem = uiNewArea(&mem_handler);
    uiBoxAppend(mv, uiControl(D.mem), 1);
    uiBoxAppend(right, uiControl(group("Memory (RAM and ROM; the hardware's registers show as --)", uiControl(mv))), 1);
    uiBoxAppend(body, uiControl(right), 1);
    uiBoxAppend(top, uiControl(body), 1);
    uiBoxAppend(top, uiControl(uiNewLabel("Click a line of code to choose it, double-click it for a breakpoint. Once clicked, "
                                           "\xe2\x86\x91 \xe2\x86\x93 Page Up and Page Down move through the code or memory. "
                                           "An address: hex, a register (a6), or one plus or minus hex (a7+$10).")), 0);
    uiWindowSetChild(D.win, uiControl(top));
    D.building = 0;
}

static void debug_refresh(int all) {
    ac090 *c = app_cpu();
    if (!D.win || !c) return;
    int stopped = dbg_stopped(c);
    D.building = 1;
    enable(uiControl(D.brk), !stopped && !c->halted);
    enable(uiControl(D.cont), stopped);
    enable(uiControl(D.step), stopped);
    enable(uiControl(D.over), stopped);
    enable(uiControl(D.run_to), stopped);
    char t[160];
    if (all || !stopped) {
        for (int i = 0; i < NREGS; i++) {
            uint32_t v = 0;
            dbg_get_reg(c, reg_names[i], &v);
            snprintf(t, sizeof t, i == 17 ? "%04x" : "%08x", v);
            uiEntrySetText(D.reg[i], t);
            uiEntrySetReadOnly(D.reg[i], !stopped);
        }
        if (c->model == AC090_68000) uiEntrySetReadOnly(D.reg[20], 1);   /* the 68008 has no VBR */
    }
    dbg_sr_text(ac090_get_sr(c), t, sizeof t);
    uiLabelSetText(D.flags, t);
    if (stopped || c->halted) snprintf(t, sizeof t, "%s, at $%08x", dbg_why(c), c->pc);
    else snprintf(t, sizeof t, "Running");
    uiLabelSetText(D.status, t);
    snprintf(t, sizeof t, "%s, %llu instructions", app_is_ql() ? "68008" : "68040", (unsigned long long)c->instructions);
    uiLabelSetText(D.count, t);
    char list[400] = "";
    for (int i = 0; i < c->dbg_nbp; i++) {
        size_t n = strlen(list);
        snprintf(list + n, sizeof list - n, "%s$%08x", i ? (i % 2 ? "   " : "\n") : "", c->dbg_bp[i]);
    }
    uiLabelSetText(D.bps, c->dbg_nbp ? list : "None. Double-click a line of code.");
    enable(uiControl(D.bp_clear), c->dbg_nbp > 0);
    if (uiCheckboxChecked(D.follow)) {
        int in = 0;
        for (int i = 0; i + 2 < D.nlines; i++) if (D.lines_at[i] == c->pc) in = 1;
        if (!in) D.code_top = dbg_back(c, c->pc, 4);
    }
    D.building = 0;
    uiAreaQueueRedrawAll(D.code);
    uiAreaQueueRedrawAll(D.mem);
}

void gui_debug_open(void) {
    if (!inited) return;
    ac090 *c = app_cpu();
    if (!D.win) {
        debug_build();
        if (c) {
            D.code_top = dbg_back(c, c->pc, 4);
            D.mem_top = c->a[7] & ~15u;
            char t[16];
            snprintf(t, sizeof t, "%08x", D.mem_top);
            uiEntrySetText(D.mem_at, t);
        }
    }
    D.open = 1;
    D.was_stopped = -1;
    uiControlShow(uiControl(D.win));
    debug_refresh(1);
}

int gui_debug_is_open(void) { return D.open; }

void gui_debug_tick(void) {
    if (!D.open) return;
    gui_pump();
    ac090 *c = app_cpu();
    if (!c || !D.open) return;
    int stopped = dbg_stopped(c);
    if (stopped != D.was_stopped || (stopped && c->instructions != D.was_ins)) debug_refresh(1);
    else if (!stopped && ++D.ticks % 10 == 0) debug_refresh(0);   /* running: five times a second */
    D.was_stopped = stopped;
    D.was_ins = c->instructions;
}

void gui_quit(void) {
    if (!inited) return;
    /* libui wants every window gone before it ends */
    if (L.win) uiControlDestroy(uiControl(L.win));
    if (M.win) uiControlDestroy(uiControl(M.win));
    if (D.win) uiControlDestroy(uiControl(D.win));
    L.win = NULL; M.win = NULL; D.win = NULL;
    uiUninit();
    inited = 0;
}
