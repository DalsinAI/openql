/* DOS1_: a host folder as a QL drive (qldos.h). */
#include "qldos.h"
#include "qldos_rom.h"
#include "../hostdir.h"
#include <ctype.h>
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifndef _WIN32
#include <sys/statvfs.h>
#endif
#ifdef _WIN32
#include <direct.h>
#define mkdir1(p) _mkdir(p)
#else
#define mkdir1(p) mkdir(p, 0755)
#endif

#define ROM_AT    0xc000u
#define NAMELEN   36
#define HDR       64
#define MAX_FILE  (16u << 20)             /* far more than a QL's memory */
#define QL_EPOCH  283996800L
#define TRASH     ".openql-deleted"

/* QDOS's errors */
enum { ERR_NO = -6, ERR_NF = -7, ERR_EX = -8, ERR_EF = -10, ERR_DF = -11, ERR_BN = -12,
       ERR_BO = -5, ERR_OM = -3, ERR_BP = -15, ERR_RO = -20, ERR_NI = -19 };
/* a file channel's block (from ch_end, $18) and a physical definition */
enum { FS_ACCES = 0x1c, FS_FNAME = 0x32, PD_DRIVN = 0x14 };

typedef struct dos_chan {
    uint32_t blk;                 /* the channel's block in the QL's memory */
    char path[1100];              /* the host file */
    char name[NAMELEN + 1];       /* the QL's name */
    uint8_t *buf;                 /* the file's bytes (a directory: its entries) */
    uint32_t len, cap, pos;
    uint8_t type;                 /* 1: executable */
    uint32_t dspace;
    int dirty, read_only;
    struct dos_chan *next;
} dos_chan;

typedef struct dos {
    char folder[1024];            /* "" while no folder is attached */
    dos_chan *chans;
    int installed;
} dos;

/* ---- the QL's memory and registers ---- */

static uint8_t rb(ql *q, uint32_t a) { return (uint8_t)ac090_read(&q->cpu, a, 1); }
static void wb(ql *q, uint32_t a, uint8_t v) { ac090_write(&q->cpu, a, v, 1); }
static unsigned rw(ql *q, uint32_t a) { return (unsigned)rb(q, a) << 8 | rb(q, a + 1); }
static void ww(ql *q, uint32_t a, unsigned v) { wb(q, a, (uint8_t)(v >> 8)); wb(q, a + 1, (uint8_t)v); }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

/* A word-length name in the QL's memory. */
static int ql_string(ql *q, uint32_t a, char *out, size_t max) {
    unsigned n = rw(q, a);
    if (n >= max) return -1;
    for (unsigned i = 0; i < n; i++) out[i] = (char)rb(q, a + 2 + i);
    out[n] = 0;
    return (int)n;
}

/* ---- names ---- */

static void ql_char_name(char *out, const char *in, size_t max) {
    size_t i = 0;
    for (; in[i] && i + 1 < max; i++) out[i] = in[i] == '.' || in[i] == ' ' ? '_' : in[i];
    out[i] = 0;
}

static int same_name(const char *a, const char *b) {
    for (; *a && *b; a++, b++) if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) return 0;
    return !*a && !*b;
}

static int starts_with(const char *s, const char *prefix) {
    for (; *prefix; s++, prefix++) if (!*s || toupper((unsigned char)*s) != toupper((unsigned char)*prefix)) return 0;
    return 1;
}

static int skipped_entry(const char *n) {
    size_t l = strlen(n);
    return n[0] == '.' || (l > 11 && !strcmp(n + l - 11, ".openql-new"));
}

static int is_dir(const char *path) { struct stat st; return !stat(path, &st) && S_ISDIR(st.st_mode); }

/* The host file for a QL name: 1 found (path set), 0 not there. With
 * place, a name not there gets the path a new file of that name takes. */
static int find_file(const char *folder, const char *prefix, const char *name, char *path, size_t psz, int place, int depth) {
    DIR *d = opendir(folder);
    if (!d) return 0;
    struct dirent *de;
    char sub[1100] = "", subprefix[NAMELEN + 2] = "";
    while ((de = readdir(d))) {
        if (skipped_entry(de->d_name)) continue;
        char part[300], full[NAMELEN * 2 + 4], host[1100];
        ql_char_name(part, de->d_name, sizeof part);
        if (strlen(prefix) + strlen(part) > NAMELEN) continue;
        snprintf(full, sizeof full, "%s%s", prefix, part);
        snprintf(host, sizeof host, "%s/%s", folder, de->d_name);
        if (is_dir(host)) {
            char under[sizeof full + 1];
            snprintf(under, sizeof under, "%s_", full);
            if (depth < 6 && strlen(under) <= NAMELEN && starts_with(name, under) && strlen(under) > strlen(subprefix)) {
                snprintf(sub, sizeof sub, "%s", host);
                snprintf(subprefix, sizeof subprefix, "%s", under);
            }
        } else if (same_name(full, name)) {
            snprintf(path, psz, "%s", host);
            closedir(d);
            return 1;
        }
    }
    closedir(d);
    if (sub[0]) {
        if (find_file(sub, subprefix, name, path, psz, 0, depth + 1)) return 1;
        if (place) { snprintf(path, psz, "%s/%s", sub, name + strlen(subprefix)); return 0; }
    }
    if (place) snprintf(path, psz, "%s/%s", folder, name + strlen(prefix));
    return 0;
}

/* ---- files ---- */

static int load_file(dos_chan *ch) {
    FILE *f = fopen(ch->path, "rb");
    if (!f) return ERR_NF;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || (unsigned long)n > MAX_FILE) { fclose(f); return ERR_OM; }
    ch->buf = malloc((size_t)n + 1);
    if (!ch->buf || fread(ch->buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); return ERR_NF; }
    fclose(f);
    ch->len = ch->cap = (uint32_t)n;
    if (n >= 8 && !memcmp(ch->buf + n - 8, "XTcc", 4)) {   /* an executable, with its data space */
        ch->type = 1;
        ch->dspace = get32(ch->buf + n - 4);
        ch->len -= 8;
    }
    return 0;
}

static int save_file(dos_chan *ch) {
    if (!ch->dirty) return 0;
    uint8_t *out = malloc(ch->len + 8);
    if (!out) return ERR_OM;
    memcpy(out, ch->buf, ch->len);
    size_t n = ch->len;
    if (ch->type == 1) { memcpy(out + n, "XTcc", 4); put32(out + n + 4, ch->dspace); n += 8; }
    int rc = hostdir_write_file(ch->path, out, n) ? ERR_DF : 0;
    free(out);
    if (!rc) ch->dirty = 0;
    return rc;
}

static int grow(dos_chan *ch, uint32_t need) {
    if (need > MAX_FILE) return ERR_DF;
    if (need <= ch->cap) return 0;
    uint32_t cap = ch->cap < 4096 ? 4096 : ch->cap;
    while (cap < need) cap *= 2;
    uint8_t *b = realloc(ch->buf, cap);
    if (!b) return ERR_OM;
    ch->buf = b;
    ch->cap = cap;
    return 0;
}

static int put_bytes(dos_chan *ch, const uint8_t *p, uint32_t n) {
    int rc = grow(ch, ch->pos + n);
    if (rc) return rc;
    if (ch->pos > ch->len) memset(ch->buf + ch->len, 0, ch->pos - ch->len);
    memcpy(ch->buf + ch->pos, p, n);
    ch->pos += n;
    if (ch->pos > ch->len) ch->len = ch->pos;
    ch->dirty = 1;
    return 0;
}

/* A file's 64-byte header, as a directory holds it (len with the header). */
static void entry(uint8_t *h, const char *name, uint32_t len, uint8_t type, uint32_t dspace, long mtime) {
    memset(h, 0, HDR);
    put32(h, len + HDR);
    h[5] = type;
    put32(h + 6, dspace);
    size_t nl = strlen(name);
    put16(h + 14, (unsigned)nl);
    memcpy(h + 16, name, nl);
    put32(h + 0x34, (uint32_t)(mtime + QL_EPOCH));
}

/* A directory's entries: every file whose name starts with prefix. */
static void list_dir(dos_chan *ch, const char *folder, const char *qlprefix, const char *want, int depth) {
    DIR *d = opendir(folder);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (skipped_entry(de->d_name)) continue;
        char part[300], full[NAMELEN * 2 + 4], host[1100];
        ql_char_name(part, de->d_name, sizeof part);
        if (strlen(qlprefix) + strlen(part) > NAMELEN) continue;
        snprintf(full, sizeof full, "%s%s", qlprefix, part);
        snprintf(host, sizeof host, "%s/%s", folder, de->d_name);
        struct stat st;
        if (stat(host, &st)) continue;
        if (S_ISDIR(st.st_mode)) {
            char under[sizeof full + 1];
            snprintf(under, sizeof under, "%s_", full);
            if (depth < 6 && strlen(under) <= NAMELEN && (starts_with(under, want) || starts_with(want, under)))
                list_dir(ch, host, under, want, depth + 1);
            continue;
        }
        if (!starts_with(full, want) || (uint64_t)st.st_size > MAX_FILE) continue;
        uint32_t len = (uint32_t)st.st_size, dspace = 0;
        uint8_t type = 0;
        if (len >= 8) {
            FILE *f = fopen(host, "rb");
            uint8_t tail[8];
            if (f && !fseek(f, -8, SEEK_END) && fread(tail, 1, 8, f) == 8 && !memcmp(tail, "XTcc", 4)) {
                type = 1; dspace = get32(tail + 4); len -= 8;
            }
            if (f) fclose(f);
        }
        uint8_t h[HDR];
        entry(h, full, len, type, dspace, (long)st.st_mtime);
        if (grow(ch, ch->len + HDR) == 0) { memcpy(ch->buf + ch->len, h, HDR); ch->len += HDR; }
    }
    closedir(d);
}

/* DELETE: the file goes to the folder's .openql-deleted, not away. */
static int trash(const char *folder, const char *path, const char *name) {
    char dir[1100], to[1200];
    snprintf(dir, sizeof dir, "%s/" TRASH, folder);
    mkdir1(dir);
    snprintf(to, sizeof to, "%s/%s", dir, name);
    remove(to);
    return rename(path, to) ? ERR_RO : 0;
}

/* ---- the driver's calls ---- */

static dos_chan *chan_of(dos *d, uint32_t blk) {
    for (dos_chan *c = d->chans; c; c = c->next) if (c->blk == blk) return c;
    return NULL;
}

static void chan_free(dos *d, dos_chan *ch) {
    for (dos_chan **pp = &d->chans; *pp; pp = &(*pp)->next)
        if (*pp == ch) { *pp = ch->next; break; }
    free(ch->buf);
    free(ch);
}

static int32_t dos_open(ql *q, dos *d) {
    ac090 *c = &q->cpu;
    uint32_t blk = c->a[0], phys = c->a[1];
    int key = (int8_t)rb(q, blk + FS_ACCES);
    if (!d->folder[0] || rb(q, phys + PD_DRIVN) != 1) return ERR_NF;   /* DOS1_ only, with a folder */
    char name[NAMELEN + 1];
    if (ql_string(q, blk + FS_FNAME, name, sizeof name) < 0) return ERR_BN;
    if (strchr(name, '/') || strchr(name, '\\') || strstr(name, "..")) return ERR_BN;
    dos_chan *ch = calloc(1, sizeof *ch);
    if (!ch) return ERR_OM;
    ch->blk = blk;
    snprintf(ch->name, sizeof ch->name, "%s", name);
    int found = name[0] ? find_file(d->folder, "", name, ch->path, sizeof ch->path, key == 2 || key == 3, 0) : 0;
    int rc = 0;
    switch (key) {
    case -1:                                       /* delete */
        rc = found ? trash(d->folder, ch->path, name) : ERR_NF;
        free(ch);
        return rc;
    case 0: case 1:                                /* old file: exclusive, shared */
        if (!found) rc = ERR_NF;
        else rc = load_file(ch);
        ch->read_only = key == 1;
        break;
    case 2: case 3:                                /* new file; new or overwrite */
        if (!name[0]) rc = ERR_BN;
        else if (found && key == 2) rc = ERR_EX;
        else ch->dirty = 1;                        /* there on close, even empty */
        break;
    case 4:                                        /* directory */
        ch->read_only = 1;
        list_dir(ch, d->folder, "", name, 0);
        break;
    default: rc = ERR_BP;
    }
    if (rc) { free(ch->buf); free(ch); return rc; }
    ch->next = d->chans;
    d->chans = ch;
    return 0;
}

static int32_t dos_io(ql *q, dos *d) {
    ac090 *c = &q->cpu;
    dos_chan *ch = chan_of(d, c->a[0]);
    if (!ch) return ERR_NO;
    int op = (int)(c->d[0] & 0x7f);
    uint32_t d2 = op < 0x40 ? (uint32_t)(uint16_t)c->d[2] : c->d[2];
    uint32_t a1 = c->a[1];
    switch (op) {
    case 0x00: return ch->pos < ch->len ? 0 : ERR_EF;                 /* io.pend */
    case 0x01:                                                         /* io.fbyte */
        if (ch->pos >= ch->len) return ERR_EF;
        c->d[1] = (c->d[1] & ~0xffu) | ch->buf[ch->pos++];
        return 0;
    case 0x02: {                                                       /* io.fline */
        uint32_t n = 0;
        while (n < d2) {
            if (ch->pos >= ch->len) { c->d[1] = n; c->a[1] = a1 + n; return ERR_EF; }
            uint8_t b = ch->buf[ch->pos++];
            wb(q, a1 + n++, b);
            if (b == 10) { c->d[1] = n; c->a[1] = a1 + n; return 0; }
        }
        c->d[1] = n; c->a[1] = a1 + n;
        return ERR_BO;
    }
    case 0x03: case 0x48: {                                            /* io.fstrg, fs.load */
        uint32_t n = ch->pos < ch->len ? ch->len - ch->pos : 0;
        if (n > d2) n = d2;
        for (uint32_t i = 0; i < n; i++) wb(q, a1 + i, ch->buf[ch->pos + i]);
        ch->pos += n;
        c->d[1] = n; c->a[1] = a1 + n;
        return n < d2 ? ERR_EF : 0;
    }
    case 0x05: case 0x07: case 0x49: {                                 /* io.sbyte, io.sstrg, fs.save */
        if (ch->read_only) return ERR_RO;
        uint8_t one = (uint8_t)c->d[1];
        if (op == 0x05) return put_bytes(ch, &one, 1);
        uint8_t *tmp = malloc(d2 ? d2 : 1);
        if (!tmp) return ERR_OM;
        for (uint32_t i = 0; i < d2; i++) tmp[i] = rb(q, a1 + i);
        int rc = put_bytes(ch, tmp, d2);
        free(tmp);
        if (rc) return rc;
        c->d[1] = d2; c->a[1] = a1 + d2;
        return 0;
    }
    case 0x40: return 0;                                               /* fs.check */
    case 0x41: return save_file(ch);                                   /* fs.flush */
    case 0x42: case 0x43: {                                            /* fs.posab, fs.posre */
        int64_t to = op == 0x42 ? (int64_t)(int32_t)c->d[1] : (int64_t)ch->pos + (int32_t)c->d[1];
        if (to < 0) to = 0;
        if (to > ch->len) to = ch->len;
        ch->pos = (uint32_t)to;
        c->d[1] = ch->pos;
        return ch->pos < ch->len ? 0 : ERR_EF;
    }
    case 0x45: {                                                       /* fs.mdinf */
        const char *base = strrchr(d->folder, '/');
        const char *bs = strrchr(d->folder, '\\');
        if (bs > base) base = bs;
        base = base && base[1] ? base + 1 : d->folder;
        for (int i = 0; i < 10; i++) wb(q, a1 + i, (uint8_t)(i < (int)strlen(base) ? base[i] : ' '));
        c->a[1] = a1 + 10;
        /* free and good sectors of 512 bytes: as many as SuperBASIC's
         * DIR shows (it takes them as signed words) */
        unsigned long avail = 32767;
#ifndef _WIN32
        struct statvfs vs;
        if (!statvfs(d->folder, &vs) && (unsigned long long)vs.f_bavail * vs.f_frsize / 512 < avail)
            avail = (unsigned long)((unsigned long long)vs.f_bavail * vs.f_frsize / 512);
#endif
        c->d[1] = (uint32_t)avail << 16 | 32767u;
        return 0;
    }
    case 0x46:                                                         /* fs.heads */
        if (ch->read_only) return ERR_RO;
        ch->type = rb(q, a1 + 5);
        ch->dspace = (uint32_t)rw(q, a1 + 6) << 16 | rw(q, a1 + 8);
        ch->dirty = 1;
        c->d[1] = 14; c->a[1] = a1 + 14;
        return 0;
    case 0x47: {                                                       /* fs.headr */
        uint8_t h[HDR];
        entry(h, ch->name, ch->len, ch->type, ch->dspace, (long)time(NULL));
        put32(h, ch->len);                                             /* the length without the header, as the drivers give it */
        uint32_t n = d2 < HDR ? d2 : HDR;
        for (uint32_t i = 0; i < n; i++) wb(q, a1 + i, h[i]);
        c->d[1] = n; c->a[1] = a1 + n;
        return 0;
    }
    case 0x4a: {                                                       /* fs.renam: a1 the new name, with its drive */
        char nn[NAMELEN + 8], to[1100];
        if (ch->read_only && ch->dirty) return ERR_RO;
        if (ql_string(q, a1, nn, sizeof nn) < 6 || !starts_with(nn, "DOS1_")) return ERR_BN;
        const char *n = nn + 5;
        if (!n[0] || strlen(n) > NAMELEN) return ERR_BN;
        if (strchr(n, '/') || strchr(n, '\\') || strstr(n, "..")) return ERR_BN;
        if (find_file(d->folder, "", n, to, sizeof to, 1, 0)) return ERR_EX;
        if (!ch->dirty && rename(ch->path, to)) return ERR_RO;
        if (ch->dirty) remove(ch->path);
        snprintf(ch->path, sizeof ch->path, "%s", to);
        size_t l = strlen(n);
        memcpy(ch->name, n, l + 1);
        ww(q, ch->blk + FS_FNAME, (unsigned)l);
        for (size_t i = 0; i < l; i++) wb(q, ch->blk + FS_FNAME + 2 + (uint32_t)i, (uint8_t)n[i]);
        return 0;
    }
    case 0x4b:                                                         /* fs.trunc */
        if (ch->read_only) return ERR_RO;
        ch->len = ch->pos;
        ch->dirty = 1;
        return 0;
    default: return ERR_BP;
    }
}

static void dos_close(ql *q, dos *d) {
    dos_chan *ch = chan_of(d, q->cpu.a[0]);
    if (!ch) return;
    save_file(ch);
    chan_free(d, ch);
}

/* The line A words in the driver: $A0D1 open, $A0D2 i/o, $A0D3 close. */
static int dos_guard(ac090 *c, int vector, uint32_t pc) {
    ql *q = c->bus.opaque;
    dos *d = q->dos;
    (void)vector;
    if (!d || pc < ROM_AT || pc >= ROM_AT + sizeof qldos_rom) return 0;
    unsigned op = (unsigned)q->rom[pc] << 8 | q->rom[pc + 1];
    switch (op) {
    case 0xa0d1: c->d[0] = (uint32_t)dos_open(q, d); break;
    case 0xa0d2: c->d[0] = (uint32_t)dos_io(q, d); break;
    case 0xa0d3: dos_close(q, d); c->d[0] = 0; break;
    default: return 0;
    }
    c->pc = pc + 2;
    return 1;
}

static void dos_free(ql *q) {
    dos *d = q->dos;
    if (!d) return;
    while (d->chans) { save_file(d->chans); chan_free(d, d->chans); }
    free(d);
    q->dos = NULL;
    q->dos_free = NULL;
}

int ql_dos_install(ql *q) {
    for (unsigned i = 0; i < 0x4000; i++) if (q->rom[ROM_AT + i] != 0xff) return -1;   /* a cartridge */
    dos *d = calloc(1, sizeof *d);
    if (!d) return -1;
    memcpy(q->rom + ROM_AT, qldos_rom, sizeof qldos_rom);
    d->installed = 1;
    q->dos = d;
    q->dos_free = dos_free;
    q->cpu.guard_hook = dos_guard;
    return 0;
}

int ql_dos_installed(const ql *q) { return q->dos != NULL; }

int ql_dos_attach(ql *q, const char *folder, char *why, size_t whysz) {
    dos *d = q->dos;
    if (!d) { snprintf(why, whysz, "a cartridge is in the ROM port"); return -1; }
    struct stat st;
    if (stat(folder, &st) || !S_ISDIR(st.st_mode)) { snprintf(why, whysz, "%s is not a folder", folder); return -1; }
    ql_dos_flush(q);
    snprintf(d->folder, sizeof d->folder, "%s", folder);
    return 0;
}

void ql_dos_flush(ql *q) {
    dos *d = q->dos;
    if (!d) return;
    for (dos_chan *ch = d->chans; ch; ch = ch->next) save_file(ch);
}

void ql_dos_detach(ql *q) {
    dos *d = q->dos;
    if (!d) return;
    ql_dos_flush(q);
    d->folder[0] = 0;
}
