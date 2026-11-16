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
#define HDR 64
#define NAMELEN 36
#define QL_EPOCH 283996800L                 /* 1961-01-01 to 1970-01-01, in seconds */
#define MB (1024LL * 1024LL)

/* Offsets past 2 GB: long is 32 bits on Windows. */
#ifdef _WIN32
#define dseek(f, o) _fseeki64(f, (__int64)(o), SEEK_SET)
#define dsize(f) (_fseeki64(f, 0, SEEK_END) ? -1 : (int64_t)_ftelli64(f))
#else
#define dseek(f, o) fseeko(f, (off_t)(o), SEEK_SET)
#define dsize(f) (fseeko(f, 0, SEEK_END) ? -1 : (int64_t)ftello(f))
#endif

/* A disk's shape: the floppy's, or a hard disk's sized to the folder. */
typedef struct geom {
    unsigned spt, heads, cyls, sctg;
    uint32_t sectors;
} geom;

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
    geom g;
    unsigned ngrp;
    uint8_t *img;               /* the header and the map */
    size_t img_len;
    FILE *f;                    /* the image: files go straight in at their groups */
    unsigned next_group;        /* the next free group, handed out in order */
    int skipped;
    int failed;                 /* a write failed */
    uint64_t bytes;             /* what the files hold, to size a hard disk */
    uint32_t max_file;          /* the largest a file may be */
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
            if (!f || (uint64_t)size > b->max_file) { if (f) fclose(f); b->skipped++; free(n); continue; }
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
            b->bytes += n->len;
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

static unsigned gbytes(const build *b) { return b->g.sctg * SECT; }
static unsigned groups_for(const build *b, uint32_t len) { return (len + gbytes(b) - 1) / gbytes(b); }

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
    unsigned need = groups_for(b, len ? len : 1);
    if (b->next_group + need > b->ngrp) return -1;
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
        /* the groups were handed out in a row */
        if (dseek(b->f, (int64_t)n->first * gbytes(b)) || fwrite(n->data, 1, n->len, b->f) != n->len) b->failed = 1;
    }
}

static int build_image(const char *folder, const char *image, int hard, int *skipped, char *why, size_t whysz) {
    struct stat st;
    if (stat(folder, &st) || !S_ISDIR(st.st_mode)) { say(why, whysz, "%s is not a folder", folder); return -1; }
    build b = { 0 };
    node root = { 0 };
    root.dir = 1;
    b.max_file = hard ? (uint32_t)(HOSTDIR_HD_MAX_MB * MB / 2) : 80u * 2 * 36 * SECT;
    root.kids = scan(folder, "", 0, &b);
    if (hard) {
        /* room for what is there twice over and 32 MB more, at least 64 MB:
         * whole cylinders of 16 heads and 63 sectors, the drive's geometry */
        uint64_t want = b.bytes * 2 + 32 * MB;
        if (want < HOSTDIR_HD_MIN_MB * MB) want = HOSTDIR_HD_MIN_MB * MB;
        if (want > HOSTDIR_HD_MAX_MB * MB) want = HOSTDIR_HD_MAX_MB * MB;
        b.g.spt = 63; b.g.heads = 16;
        b.g.cyls = (unsigned)((want / SECT + 1007) / 1008);
        b.g.sectors = b.g.cyls * 1008u;
        b.g.sctg = 4;                                   /* a group number is a word */
        while (b.g.sectors / b.g.sctg > 65535) b.g.sctg *= 2;
    } else {
        b.g = (geom){ 36, 2, 80, 3, 80 * 2 * 36 };      /* a 2.88 MB floppy, as SMSQ/E formats one */
    }
    b.ngrp = b.g.sectors / b.g.sctg;
    unsigned map_bytes = QWA_GMAP + 2 * b.ngrp, map_sectors = (map_bytes + SECT - 1) / SECT;
    unsigned map_groups = (map_sectors + b.g.sctg - 1) / b.g.sctg;
    b.img_len = (size_t)map_groups * gbytes(&b);
    b.img = calloc(1, b.img_len);
    b.f = fopen(image, "wb");
    if (!b.img || !b.f) {
        free(b.img); if (b.f) fclose(b.f); free_tree(root.kids);
        say(why, whysz, b.f ? "no memory" : "cannot write %s", image);
        return -1;
    }
    uint8_t *h = b.img;
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
    put16(h + QWA_SCTG, b.g.sctg);
    put16(h + QWA_SCTT, b.g.spt);
    put16(h + QWA_TRKC, b.g.heads);
    put16(h + QWA_CYLD, b.g.cyls);
    put16(h + QWA_NGRP, b.ngrp);
    put16(h + QWA_SCTM, map_sectors);
    put16(h + QWA_NMAP, 1);
    for (unsigned g = 0; g < map_groups; g++) put16(h + QWA_GMAP + 2 * g, g + 1 < map_groups ? g + 1 : 0);
    b.next_group = map_groups;

    /* the root directory goes first after the map, as FORMAT puts it; its
     * size is known once the files that fit are known, so its groups are
     * taken for the most it could need and given back after */
    unsigned root_at = b.next_group;
    unsigned n_top = 0;
    for (node *k = root.kids; k; k = k->next) n_top++;
    unsigned reserved = groups_for(&b, HDR + n_top * HDR);
    b.next_group += reserved;
    place_tree(&b, &root.kids);
    write_tree(&b, root.kids);
    make_dir_data(&root);
    unsigned rg = groups_for(&b, root.len);
    for (unsigned g = 0; g < rg; g++) put16(h + QWA_GMAP + 2 * (root_at + g), g + 1 < rg ? root_at + g + 1 : 0);
    if (dseek(b.f, (int64_t)root_at * gbytes(&b)) || fwrite(root.data, 1, root.len, b.f) != root.len) b.failed = 1;
    put16(h + QWA_ROOT, root_at);
    put32(h + QWA_RLEN, root.len);
    /* the groups the root did not need, and those after the files, are free */
    unsigned first_free = 0, last = 0, nfree = 0;
    for (unsigned g = root_at + rg; g < b.ngrp; g++) {
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

    /* the header and map, then the disk's last byte: the file is the whole disk
     * (sparse where the host's file system allows) */
    if (dseek(b.f, 0) || fwrite(b.img, 1, b.img_len, b.f) != b.img_len) b.failed = 1;
    if (dseek(b.f, (int64_t)b.g.sectors * SECT - 1) || fputc(0, b.f) == EOF) b.failed = 1;
    if (fclose(b.f)) b.failed = 1;
    free(b.img);
    if (b.failed) { remove(image); say(why, whysz, "cannot write %s", image); return -1; }
    if (skipped) *skipped = b.skipped;
    return 0;
}

int hostdir_build(const char *folder, const char *image, int *skipped, char *why, size_t whysz) {
    return build_image(folder, image, 0, skipped, why, whysz);
}

int hostdir_build_hd(const char *folder, const char *image, int *skipped, char *why, size_t whysz) {
    return build_image(folder, image, 1, skipped, why, whysz);
}

/* ---- writing back ---------------------------------------------------------------- */

typedef struct reader {
    FILE *f;
    int64_t sectors;
    unsigned sctg, ngrp;
    uint8_t *map;               /* the header and the map */
    time_t since;               /* host files changed after this were changed on the host too */
    int written, kept;
} reader;

/* A file's bytes, from its first group along the map. */
static uint8_t *read_file(reader *r, unsigned first, uint32_t len) {
    if ((int64_t)len > r->sectors * SECT) return NULL;
    uint8_t *out = malloc(len ? len : 1);
    if (!out) return NULL;
    unsigned gsize = r->sctg * SECT, g = first;
    for (uint32_t at = 0, steps = 0; at < len; at += gsize, steps++) {
        if (g >= r->ngrp || (int64_t)(g + 1) * r->sctg > r->sectors || steps > r->ngrp) { free(out); return NULL; }
        uint32_t n = len - at < gsize ? len - at : gsize;
        if (dseek(r->f, (int64_t)g * gsize) || fread(out + at, 1, n, r->f) != n) { free(out); return NULL; }
        g = get16(r->map + QWA_GMAP + 2 * g);
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

/* A whole file written beside the old one and then put in its place, so a
 * failed write (a full disk, openql stopped) never leaves half a file. */
int hostdir_write_file(const char *path, const void *p, size_t n) {
    char tmp[1500];
    snprintf(tmp, sizeof tmp, "%s.openql-new", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    int ok = fwrite(p, 1, n, f) == n;
    if (fclose(f)) ok = 0;
#ifdef _WIN32
    if (ok) remove(path);                       /* Windows will not rename over a file */
#endif
    if (!ok || rename(tmp, path)) { remove(tmp); return -1; }
    return 0;
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
                /* changed on the host since the disk was made, and by the
                 * QL too: the host's stays, and the QL's goes beside it */
                struct stat st;
                char alt[1500];
                const char *to = path;
                if (r->since && !stat(path, &st) && st.st_mtime > r->since) {
                    snprintf(alt, sizeof alt, "%s.from-ql", path);
                    to = alt;
                    r->kept++;
                }
                if (!same_file(to, data + HDR, n) && hostdir_write_file(to, data + HDR, n) == 0) r->written++;
            }
        }
        free(data);
    }
}

int hostdir_sync(const char *image, const char *folder, time_t since, int *kept, char *why, size_t whysz) {
    if (kept) *kept = 0;
    reader r = { 0 };
    r.since = since;
    r.f = fopen(image, "rb");
    if (!r.f) { say(why, whysz, "cannot read %s", image); return -1; }
    int64_t size = dsize(r.f);
    r.sectors = size / SECT;
    uint8_t head[SECT];
    if (size < SECT || dseek(r.f, 0) || fread(head, 1, SECT, r.f) != SECT || get32(head + QWA_ID) != 0x514c5741) {
        fclose(r.f); say(why, whysz, "%s is not a QLWA disk", image); return -1;
    }
    r.sctg = get16(head + QWA_SCTG);
    r.ngrp = get16(head + QWA_NGRP);
    size_t map_len = (size_t)QWA_GMAP + 2 * r.ngrp;
    if (!r.sctg || (int64_t)map_len > size || !(r.map = malloc(map_len)) ||
        dseek(r.f, 0) || fread(r.map, 1, map_len, r.f) != map_len) {
        free(r.map); fclose(r.f); say(why, whysz, "%s's map is broken", image); return -1;
    }
    uint32_t rlen = get32(r.map + QWA_RLEN);
    uint8_t *root = read_file(&r, get16(r.map + QWA_ROOT), rlen);
    if (!root) { free(r.map); fclose(r.f); say(why, whysz, "%s's directory is broken", image); return -1; }
    sync_dir(&r, root, rlen, "", folder, 0);
    free(root);
    free(r.map);
    fclose(r.f);
    if (kept) *kept = r.kept;
    return r.written;
}
