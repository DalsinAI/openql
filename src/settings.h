/*
 * openql's settings, kept between runs in openql.cfg in its data folder
 * (~/.local/share/openql, $XDG_DATA_HOME/openql, %APPDATA%\openql), and
 * the folders beside it: roms (or $OPENQL_ROMS), disks (or $OPENQL_DISKS),
 * states.
 */
#ifndef OPENQL_SETTINGS_H
#define OPENQL_SETTINGS_H

#define SET_PATH 1024

enum { SET_QL, SET_SUPERQL };
enum { SCALE_43, SCALE_INTEGER, SCALE_STRETCH, SCALE_MODES };
enum { SPEED_1, SPEED_2, SPEED_4, SPEED_MAX, SPEEDS };

typedef struct settings {
    int machine;                    /* SET_QL or SET_SUPERQL */
    int ram_ql_kb, ram_sql_mb;
    char rom_ql[SET_PATH], rom_sql[SET_PATH];   /* empty: the fetched one */
    char win1[SET_PATH];            /* the Super QL's WIN1_ */
    int win1_ro;
    char flp[2][SET_PATH];          /* the Super QL's FLP1_, FLP2_ */
    char mdv[2][SET_PATH];          /* the QL's mdv1_, mdv2_ */
    char host_dir[SET_PATH];        /* a host folder as a drive: WIN2_ (hostdir.h) or DOS1_ (ql/qldos.h) */
    int scale, fullscreen, speed, sound;
    int launcher;                   /* show the launcher when openql starts with nothing to do */
    int seen;                       /* 0 until a settings file has been read or written */
} settings;

void settings_default(settings *s);
int settings_load(settings *s, const char *path);     /* 0, or -1 (none yet: defaults) */
int settings_save(const settings *s, const char *path);

/* The folders and files: each returns a static buffer of its own. */
const char *data_dir(void);
const char *rom_dir(void);
const char *disk_dir(void);
const char *state_dir(void);           /* made if it is not there */
const char *settings_path(void);
const char *fetched_rom(int machine);  /* the fetched ROM's path, or NULL: none */
const char *fetched_qle(void);         /* QLE.WIN in the disks folder, or NULL */
int make_dirs(const char *path);       /* mkdir -p */
int file_exists(const char *path);

const char *speed_name(int speed);
const char *scale_name(int scale);

#endif
