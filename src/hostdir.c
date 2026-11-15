/* A host folder as a QLWA disk image (hostdir.h). */
#include "hostdir.h"
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef _WIN32
#include <direct.h>
#define mkdir1(p) _mkdir(p)
#else
#define mkdir1(p) mkdir(p, 0755)
#endif

#define SECT 512
#define SPT 36
#define HEADS 2
#define CYLS 80
#define SECTORS (SPT * HEADS * CYLS)        /* 5760: 2.88 MB */
#define SCTG 3                              /* sectors a group, as SMSQ/E formats an ED disk */
#define NGRP (SECTORS / SCTG)
#define HDR 64
#define NAMELEN 36
#define QL_EPOCH 283996800L                 /* 1961-01-01 to 1970-01-01, in seconds */

/* QLWA's header */
enum {
    QWA_ID = 0x00, QWA_NAME = 0x04, QWA_UCHK = 0x1c, QWA_INTL = 0x20, QWA_SCTG = 0x22, QWA_SCTT = 0x24,
    QWA_TRKC = 0x26, QWA_CYLD = 0x28, QWA_NGRP = 0x2a, QWA_FGRP = 0x2c, QWA_SCTM = 0x2e, QWA_NMAP = 0x30,
    QWA_FREE = 0x32, QWA_ROOT = 0x34, QWA_RLEN = 0x36, QWA_GMAP = 0x40,
};
/* a file's header, and a directory's entries */
enum { H_FLEN = 0x00, H_TYPE = 0x05, H_DATA = 0x06, H_NAME = 0x0e, H_DATE = 0x34, H_FLID = 0x3a };

static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static unsigned get16(const uint8_t *p) { return (unsigned)p[0] << 8 | p[1]; }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static void say(char *why, size_t n, const char *s, const char *arg) { if (why) snprintf(why, n, s, arg ? arg : ""); }

/* ---- building --------------------------------------------------------------- */

typedef struct node {
    char qlname[NAMELEN + 1];   /* the whole name, with the folders' */
    char host[1024];
    int dir;
    uint8_t type;
    uint32_t dataspace;
    uint8_t *data;              /* a file's bytes, or a directory's entries (with its header) */
    uint32_t len;               /* its length, with the 64-byte header */
    long mtime;
    int first;                  /* its first group */
    struct node *kids, *next;
} node;

typedef struct build {
    uint8_t *img;
    unsigned next_group;        /* the next free group, handed out in order */
    int skipped;
} build;

static void ql_char_name(char *out, const char *in) {
    for (; *in; in++, out++) *out = *in == '.' || *in == ' ' ? '_' : *in;
    *out = 0;
}

static void free_tree(node *n) {
    while (n) {
        node *next = n->next;
        free_tree(n->kids);
        free(n->data);
        free(n);
        n = next;
    }
}

static node *scan(const char *folder, const char *prefix, int depth, build *b) {
    DIR *d = opendir(folder);
    if (!d) return NULL;
    node *head = NULL;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        node *n = calloc(1, sizeof *n);
        if (!n) break;
        snprintf(n->host, sizeof n->host, "%s/%s", folder, de->d_name);
        char part[300];
        ql_char_name(part, de->d_name);
        struct stat st;
        if (stat(n->host, &st) || strlen(prefix) + strlen(part) > NAMELEN) { b->skipped++; free(n); continue; }
        strcpy(n->qlname, prefix);              /* the lengths are checked above */
        strcat(n->qlname, part);
        n->mtime = (long)st.st_mtime;
        if (S_ISDIR(st.st_mode)) {
            if (depth >= 6 || strlen(n->qlname) + 1 > NAMELEN) { b->skipped++; free(n); continue; }
            n->dir = 1;
            n->type = 0xff;
            char sub[NAMELEN + 2];
            snprintf(sub, sizeof sub, "%s_", n->qlname);
            n->kids = scan(n->host, sub, depth + 1, b);
        } else {
            FILE *f = fopen(n->host, "rb");
            long size = st.st_size;
            if (!f || size > SECTORS * SECT) { if (f) fclose(f); b->skipped++; free(n); continue; }
            n->data = malloc((size_t)size + HDR);
            if (!n->data || fread(n->data + HDR, 1, (size_t)size, f) != (size_t)size) { fclose(f); free(n->data); free(n); b->skipped++; continue; }
            fclose(f);
            uint8_t *end = n->data + HDR + size;
            if (size >= 8 && !memcmp(end - 8, "XTcc", 4)) {   /* an executable, with its data space */
                n->type = 1;
                n->dataspace = get32(end - 4);
                size -= 8;
            }
            n->len = (uint32_t)size + HDR;
        }
        n->next = head;
        head = n;
    }
    closedir(d);
    return head;
}

static void header(uint8_t *h, const node *n) {
    memset(h, 0, HDR);
    put32(h + H_FLEN, n->len);
    h[H_TYPE] = n->type;
    put32(h + H_DATA, n->dataspace);
    size_t nl = strlen(n->qlname);
    put16(h + H_NAME, (unsigned)nl);
    memcpy(h + H_NAME + 2, n->qlname, nl);
    put32(h + H_DATE, (uint32_t)(n->mtime + QL_EPOCH));
    put16(h + H_FLID, (unsigned)n->first);
}

static unsigned groups_for(uint32_t len) { return (len + SCTG * SECT - 1) / (SCTG * SECT); }

/* A directory's entries, once its files have their first groups. */
static void make_dir_data(node *dir) {
    unsigned n = 0;
    for (node *k = dir->kids; k; k = k->next) n++;
    dir->len = HDR + n * HDR;
    dir->data = calloc(1, dir->len);
    unsigned i = 0;
    for (node *k = dir->kids; k; k = k->next, i++) header(dir->data + HDR + i * HDR, k);
}

/* Hand out groups: a file's groups follow each other, chained in the map. */
static int place(build *b, uint32_t len) {
    unsigned need = groups_for(len ? len : 1);
    if (b->next_group + need > NGRP) return -1;
    int first = (int)b->next_group;
    uint8_t *map = b->img + QWA_GMAP;
    for (unsigned g = 0; g < need; g++) put16(map + 2 * (first + g), g + 1 < need ? first + g + 1 : 0);
    b->next_group += need;
    return first;
}

/* Files first (their groups name them), then the directories that list them. */
static void place_tree(build *b, node **list) {
    for (node **pp = list; *pp; ) {
        node *n = *pp;
        if (n->dir) {
            place_tree(b, &n->kids);
            make_dir_data(n);
        }
        if ((n->first = place(b, n->len)) < 0) {   /* no room: left out */
            *pp = n->next;
            n->next = NULL;
            free_tree(n);
            b->skipped++;
            continue;
        }
        pp = &n->next;
    }
}

static void write_tree(build *b, node *list) {
    for (node *n = list; n; n = n->next) {
        if (n->dir) write_tree(b, n->kids);
        header(n->data, n);
        /* follow the map: the groups were handed out in a row */
        memcpy(b->img + (size_t)n->first * SCTG * SECT, n->data, n->len);
    }
}

int hostdir_build(const char *folder, const char *image, int *skipped, char *why, size_t whysz) {
    struct stat st;
    if (stat(folder, &st) || !S_ISDIR(st.st_mode)) { say(why, whysz, "%s is not a folder", folder); return -1; }
    build b = { 0 };
    b.img = calloc(SECTORS, SECT);
    if (!b.img) { say(why, whysz, "no memory", NULL); return -1; }
    uint8_t *h = b.img;
    unsigned map_bytes = QWA_GMAP + 2 * NGRP, map_sectors = (map_bytes + SECT - 1) / SECT;
    unsigned map_groups = (map_sectors + SCTG - 1) / SCTG;
    put32(h + QWA_ID, 0x514c5741);                  /* QLWA */
    const char *base = strrchr(folder, '/');
    const char *bs = strrchr(folder, '\\');
    if (bs > base) base = bs;
    base = base && base[1] ? base + 1 : folder;
    char name[21];
    memset(name, ' ', 20);
    name[20] = 0;
    size_t nl = strlen(base) > 10 ? 10 : strlen(base);   /* SMSQ/E shows ten characters */
    for (size_t i = 0; i < nl; i++) name[i] = base[i] == ' ' ? '_' : base[i];
    put16(h + QWA_NAME, (unsigned)nl);
    memcpy(h + QWA_NAME + 2, name, 20);
    put32(h + QWA_UCHK, (uint32_t)time(NULL));
    put16(h + QWA_SCTG, SCTG);
    put16(h + QWA_SCTT, SPT);
    put16(h + QWA_TRKC, HEADS);
    put16(h + QWA_CYLD, CYLS);
    put16(h + QWA_NGRP, NGRP);
    put16(h + QWA_SCTM, map_sectors);
    put16(h + QWA_NMAP, 1);
    for (unsigned g = 0; g < map_groups; g++) put16(h + QWA_GMAP + 2 * g, g + 1 < map_groups ? g + 1 : 0);
    b.next_group = map_groups;

    node root = { 0 };
    root.dir = 1;
    root.kids = scan(folder, "", 0, &b);
    /* the root directory goes first after the map, as FORMAT puts it; its
     * size is known once the files that fit are known, so its groups are
     * taken for the most it could need and given back after */
    unsigned root_at = b.next_group;
    unsigned n_top = 0;
    for (node *k = root.kids; k; k = k->next) n_top++;
    b.next_group += groups_for(HDR + n_top * HDR);
    place_tree(&b, &root.kids);
    write_tree(&b, root.kids);
    make_dir_data(&root);
    unsigned rg = groups_for(root.len);
    for (unsigned g = 0; g < rg; g++) put16(h + QWA_GMAP + 2 * (root_at + g), g + 1 < rg ? root_at + g + 1 : 0);
    memcpy(b.img + (size_t)root_at * SCTG * SECT, root.data, root.len);
    put16(h + QWA_ROOT, root_at);
    put32(h + QWA_RLEN, root.len);
    /* the groups the root did not need, and those after the files, are free */
    unsigned first_free = 0, last = 0, nfree = 0, reserved = groups_for(HDR + n_top * HDR);
    for (unsigned g = root_at + rg; g < NGRP; g++) {
        if (g >= root_at + reserved && g < b.next_group) continue;   /* a file's */
        if (!first_free) first_free = g; else put16(h + QWA_GMAP + 2 * last, g);
        put16(h + QWA_GMAP + 2 * g, 0);
        last = g;
        nfree++;
    }
    put16(h + QWA_FREE, first_free);
    put16(h + QWA_FGRP, nfree);
    free(root.data);
    free_tree(root.kids);

    FILE *f = fopen(image, "wb");
    int ok = f && fwrite(b.img, SECT, SECTORS, f) == SECTORS;
    if (f && fclose(f)) ok = 0;
    free(b.img);
    if (!ok) { say(why, whysz, "cannot write %s", image); return -1; }
    if (skipped) *skipped = b.skipped;
    return 0;
}

/* ---- writing back ---------------------------------------------------------------- */

typedef struct reader {
    uint8_t *img;
    long sectors;
    unsigned sctg, ngrp;
    int written;
} reader;

/* A file's bytes, from its first group along the map. */
static uint8_t *read_file(reader *r, unsigned first, uint32_t len) {
    if (len > (uint32_t)r->sectors * SECT) return NULL;
    uint8_t *out = malloc(len ? len : 1);
    if (!out) return NULL;
    unsigned gsize = r->sctg * SECT, g = first;
    for (uint32_t at = 0, steps = 0; at < len; at += gsize, steps++) {
        if (g >= r->ngrp || (long)(g + 1) * r->sctg > r->sectors || steps > r->ngrp) { free(out); return NULL; }
        uint32_t n = len - at < gsize ? len - at : gsize;
        memcpy(out + at, r->img + (size_t)g * gsize, n);
        g = get16(r->img + QWA_GMAP + 2 * g);
    }
    return out;
}

static int same_file(const char *path, const uint8_t *p, size_t n) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    int same = 1;
    uint8_t buf[4096];
    size_t at = 0, got;
    while (same && (got = fread(buf, 1, sizeof buf, f)) > 0) {
        if (at + got > n || memcmp(buf, p + at, got)) same = 0;
        at += got;
    }
    fclose(f);
    return same && at == n;
}

/* The host name for a QL name in a folder: the one it came from (a host
 * name that turns into it), else the QL's own. */
static void host_name(const char *folder, const char *part, char *out, size_t outsz) {
    DIR *d = opendir(folder);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d))) {
            char q[300];
            ql_char_name(q, de->d_name);
            if (!strcmp(q, part)) { snprintf(out, outsz, "%s/%s", folder, de->d_name); closedir(d); return; }
        }
        closedir(d);
    }
    snprintf(out, outsz, "%s/%s", folder, part);
}

static void sync_dir(reader *r, const uint8_t *dir, uint32_t len, const char *prefix, const char *folder, int depth) {
    if (depth > 8) return;
    for (uint32_t at = HDR; at + HDR <= len; at += HDR) {
        const uint8_t *e = dir + at;
        uint32_t flen = get32(e + H_FLEN);
        unsigned nl = get16(e + H_NAME);
        if (!nl || nl > NAMELEN || flen < HDR) continue;          /* a deleted entry */
        char name[NAMELEN + 1];
        memcpy(name, e + H_NAME + 2, nl);
        name[nl] = 0;
        size_t pl = strlen(prefix);
        if (strncmp(name, prefix, pl) || !name[pl]) continue;
        const char *part = name + pl;
        if (strchr(part, '/') || strchr(part, '\\') || !strcmp(part, "..") || !strcmp(part, ".")) continue;
        char path[1400];
        host_name(folder, part, path, sizeof path);
        uint8_t *data = read_file(r, get16(e + H_FLID), flen);
        if (!data) continue;
        if (e[H_TYPE] == 0xff) {
            mkdir1(path);
            char sub[NAMELEN + 2];
            snprintf(sub, sizeof sub, "%s_", name);
            sync_dir(r, data, flen, sub, path, depth + 1);
        } else {
            size_t n = flen - HDR;
            if (e[H_TYPE] == 1) {                                   /* keep the data space with it */
                uint8_t *more = realloc(data, flen + 8);
                if (!more) { free(data); continue; }
                data = more;
                memcpy(data + flen, "XTcc", 4);
                put32(data + flen + 4, get32(e + H_DATA));
                n += 8;
            }
            if (!same_file(path, data + HDR, n)) {
                FILE *f = fopen(path, "wb");
                if (f) {
                    if (fwrite(data + HDR, 1, n, f) == n) r->written++;
                    fclose(f);
                }
            }
        }
        free(data);
    }
}

int hostdir_sync(const char *image, const char *folder, char *why, size_t whysz) {
    FILE *f = fopen(image, "rb");
    if (!f) { say(why, whysz, "cannot read %s", image); return -1; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    reader r = { 0 };
    r.sectors = size / SECT;
    r.img = malloc((size_t)size + 1);
    int ok = r.img && size >= SECT && fread(r.img, 1, (size_t)size, f) == (size_t)size;
    fclose(f);
    if (!ok || get32(r.img + QWA_ID) != 0x514c5741) { free(r.img); say(why, whysz, "%s is not a QLWA disk", image); return -1; }
    r.sctg = get16(r.img + QWA_SCTG);
    r.ngrp = get16(r.img + QWA_NGRP);
    if (!r.sctg || (size_t)QWA_GMAP + 2 * r.ngrp > (size_t)size) { free(r.img); say(why, whysz, "%s's map is broken", image); return -1; }
    uint32_t rlen = get32(r.img + QWA_RLEN);
    uint8_t *root = read_file(&r, get16(r.img + QWA_ROOT), rlen);
    if (!root) { free(r.img); say(why, whysz, "%s's directory is broken", image); return -1; }
    sync_dir(&r, root, rlen, "", folder, 0);
    free(root);
    free(r.img);
    return r.written;
}
