OpenQL: the Sinclair QL and the Super QL in one window.

- **Sinclair QL**: a PAL QL with a 68008 at 7.5 MHz, running Minerva, up to 896 KB.
- **Super QL**: a Q40/Q60-compatible board with a 68040 at 40 MHz and 128 MB, running SMSQ/E with its pointer environment, a serial mouse, WIN1 (a QXL.WIN file), FLP1_ and FLP2_ floppies (`-f`, or drop an image on the window), stereo sound through its DACs, and 16-bit colour up to 1024 x 512.
- **Sound on the QL**: BEEP, from the IPC's pitch.

No ROM is included. Fetch them on request with `openql-fetch-roms minerva` and `openql-fetch-roms smsqe` (Python 3), or give your own with `-r`.

| Download | For |
| --- | --- |
| `openql-linux-x86_64.tar.gz` | Linux x86-64 (needs SDL2: `sudo apt install libsdl2-2.0-0`) |
| `openql-windows-x86_64.zip` | Windows 10 and 11, x86-64 (SDL2 included) |
| `openql-macos-x86_64.tar.gz` | macOS on Intel, and Apple silicon through Rosetta (SDL2 included; the first time, right-click and Open) |

Not yet: the QL's own network (QLAN) wire.

New since v0.1.0:

- **Serial input** on both machines: `-S FILE` sends a file in on ser1 (COM1), and `-s tcp:PORT` or `-s tcp:HOST:PORT` makes ser1 a null-modem cable over TCP, both ways, between two openqls or to a terminal program.
- **The Super QL's external interrupt register** shows the multi-I/O card's IRQ lines: the floppy controller (IRQ 6), the IDE disk (IRQ 14) and the serial ports (IRQ 3 and 4).
- Floppy: a seek's status is no longer mixed up with the reset's polling statuses, so SMSQ/E's FORMAT gets through its tracks.

- **Microdrives on the QL**: `-d FILE` puts a `.mdv` cartridge image in mdv1_ (a second `-d`: mdv2_), or drop one on the window. QDOS's own microdrive code reads, writes and FORMATs it through the ZX8302, at the tape's real speed. A new file is a blank cartridge.

- AC090's interpreter runs plain instructions without its per-step checks: the same frames, faster.
- `openql -H` takes `-i key:NAME@T` and `-i type:TEXT@T`, so both machines can be driven without a window. CI now boots SMSQ/E and checks that SuperBASIC answers, as it does for Minerva.
- `-m` says when it is outside the machine's range, and refuses a value that is not a number.
- WIN1 takes a QXL.WIN made by QPC or Q-emuLator, which names no disk geometry: before, SMSQ/E read the wrong sectors past its first 2 MB. QL/E's QLE.WIN boots to its desktop.
- `-i type:` types for SMSQ/E's UK keyboard on the Super QL (`"`, `@`, `#`, `\\`, `|`, `~`).
- Fixes: a negative shift in the Super QL's sound (undefined in C); the picture buffer is freed on exit.
