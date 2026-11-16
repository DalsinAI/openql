/* openql's launcher and menu, in the host's own controls (gui.h). */
#include "gui.h"
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

static struct launcher {
    settings *cfg;
    uiWindow *win;
    uiCombobox *machine, *ram_ql, *ram_sql, *scale, *speed;
    uiLabel *rom;
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
    uiCombobox *slot, *speed, *scale;
    uiButton *save, *load, *paste, *reset, *power, *settings, *quit, *resume;
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
    M.quit = uiNewButton("Quit");
    M.resume = uiNewButton("Back to the machine");
    uiButtonOnClicked(M.settings, m_settings, NULL);
    uiButtonOnClicked(M.quit, m_quit, NULL);
    uiButtonOnClicked(M.resume, m_resume, NULL);
    uiBoxAppend(bar, uiControl(M.status), 1);
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

void gui_quit(void) {
    if (!inited) return;
    /* libui wants every window gone before it ends */
    if (L.win) uiControlDestroy(uiControl(L.win));
    if (M.win) uiControlDestroy(uiControl(M.win));
    L.win = NULL; M.win = NULL;
    uiUninit();
    inited = 0;
}
