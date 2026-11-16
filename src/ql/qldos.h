/*
 * DOS1_: a folder on the host as a drive of the QL's, read and written as
 * the QL goes. openql's own small driver (qldos.s) sits in the ROM port and
 * links itself in as QDOS's directory device "DOS"; its open, input/output
 * and close come here, through line A words the emulator takes.
 *
 * Names are the QL's, as hostdir.h gives them: a host file "game.bas" is
 * "game_bas", and "progs/game.bas" is "progs_game_bas". A new file keeps
 * the QL's name, in the folder its name starts with if there is one. An
 * executable's type and data space travel at the end of the host file as
 * "XTcc" and four bytes. A file is read whole when opened and written back
 * whole when it is closed or flushed: beside the old one, then renamed over
 * it, so a failed write never leaves half a file. DELETE moves the file
 * into the folder's .openql-deleted folder instead of removing it.
 */
#ifndef OPENQL_QLDOS_H
#define OPENQL_QLDOS_H

#include <stddef.h>
#include "ql.h"

/* The driver in the ROM port, before the QL starts: 0, or -1 when a
 * cartridge is there already. */
int ql_dos_install(ql *q);
int ql_dos_installed(const ql *q);

/* The folder behind DOS1_ (the driver must be in): 0, or -1 with why. */
int ql_dos_attach(ql *q, const char *folder, char *why, size_t whysz);
/* No folder: files still open are written back first. */
void ql_dos_detach(ql *q);
/* Every open file that changed, written back now. */
void ql_dos_flush(ql *q);

#endif
