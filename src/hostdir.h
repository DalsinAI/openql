/*
 * A folder on the host as a QL disk: its files and folders are written into
 * a QLWA disk image (the format of SMSQ/E's hard disks, which its floppy
 * driver reads too), which goes in a drive; when the disk comes out again,
 * the files the QL made or changed are written back to the folder.
 *
 * Names: a host file "game.bas" is the QL's "game_bas", one in a folder
 * "progs/game.bas" is "progs_game_bas" in the directory progs. A new file
 * keeps the QL's name. An executable's data space travels at the end of
 * the host file as "XTcc" and four bytes, as uQLx and sQLux keep it. Files
 * the QL deletes or renames are left as they were on the host.
 */
#ifndef OPENQL_HOSTDIR_H
#define OPENQL_HOSTDIR_H

#include <stddef.h>

/* The image for a 2.88 MB floppy: 80 cylinders, 2 heads, 36 sectors. 0, or
 * -1 with the reason in why. *skipped counts the files that did not fit. */
int hostdir_build(const char *folder, const char *image, int *skipped, char *why, size_t whysz);

/* The image's files back into the folder: how many were written, or -1. */
int hostdir_sync(const char *image, const char *folder, char *why, size_t whysz);

#endif
