OpenQL: the Sinclair QL and the Super QL in one window.

- **Sinclair QL**: a PAL QL with a 68008 at 7.5 MHz, running Minerva, up to 896 KB.
- **Super QL**: a Q40/Q60-compatible board with a 68040 at 40 MHz and 128 MB, running SMSQ/E with its pointer environment, a serial mouse, WIN1 (a QXL.WIN file), FLP1_ and FLP2_ floppies (`-f`, or drop an image on the window), stereo sound through its DACs, and 16-bit colour up to 1024 x 512.
- **Sound on the QL**: BEEP, from the IPC's pitch.

No ROM is included. Fetch them on request with `openql-fetch-roms minerva` and `openql-fetch-roms smsqe` (Python 3), or give your own with `-r`.

| Download | For |
| --- | --- |
| `openql-linux-x86_64.tar.gz` | Linux x86-64 (needs SDL2: `sudo apt install libsdl2-2.0-0`) |
| `openql-linux-arm64.tar.gz` | Linux on ARM64, such as a Raspberry Pi 4 or 5 with a 64-bit OS (needs SDL2: `sudo apt install libsdl2-2.0-0`) |
| `openql-windows-x86_64.zip` | Windows 10 and 11, x86-64 (SDL2 included) |
| `openql-macos-arm64.tar.gz` | macOS on Apple silicon, natively (SDL2 included; the first time, right-click and Open) |
| `openql-macos-x86_64.tar.gz` | macOS on Intel (SDL2 included; the first time, right-click and Open) |

Not yet: the QL's own network (QLAN) wire.

New since v0.1.0:

- **A launcher**: `openql` on its own opens a window of the host's own controls (GTK, Win32 or Cocoa, in the system font, under a black and silver Sinclair header) to choose the machine, memory, disks, picture, speed and sound, and to **Get QL/E** (downloaded from sinclairql.net and put in WIN1_ with the settings it wants) or fetch the ROMs. It remembers the choices.
- **A menu on F12**, a host window too: swap floppies and cartridges (or make a blank one), saved states in four slots, type the clipboard in (also Ctrl+Shift+V), speed (2x, 4x, as fast as it goes; Ctrl+F8), the picture's scaling (4:3, whole pixels, fill), full screen, reset, back to the launcher.
- **A host folder as a disk**: `-D FOLDER` (or the launcher, or the menu) puts a folder on the host in the Super QL's FLP2_ as a QL disk, and writes what the QL saved back to it when the disk comes out.
- **Saved states** on both machines, also from the command line: `-l FILE` starts from one, `-O FILE` saves one after `-H`.

- **Serial input** on both machines: `-S FILE` sends a file in on ser1 (COM1), and `-s tcp:PORT` or `-s tcp:HOST:PORT` makes ser1 a null-modem cable over TCP, both ways, between two openqls or to a terminal program.
- **The Super QL's external interrupt register** shows the multi-I/O card's IRQ lines: the floppy controller (IRQ 6), the IDE disk (IRQ 14) and the serial ports (IRQ 3 and 4).
- Floppy: a seek's status is no longer mixed up with the reset's polling statuses, so SMSQ/E's FORMAT gets through its tracks.

- **Microdrives on the QL**: `-d FILE` puts a `.mdv` cartridge image in mdv1_ (a second `-d`: mdv2_), or drop one on the window. QDOS's own microdrive code reads, writes and FORMATs it through the ZX8302, at the tape's real speed. A new file is a blank cartridge.

- Native builds for Apple silicon and for Linux on ARM64. The Super QL's 68040 FPU no longer needs the x87's 80-bit long double: on ARM64 it works its 80-bit values out in integers, as AmiCradle's AC090 does on the Raspberry Pi 5. Arithmetic, conversions and packed decimal give the same bits as the x86 build (checked by `make test` on every build); the transcendentals are correctly rounded, where the x86 build's C library is out by an ulp now and then. The Windows and Intel macOS builds use the integer FPU too, so they give Linux's bits where their C libraries used to round FMOD, FINT, FSIN of a large angle and others their own way.
- AC090's interpreter runs plain instructions without its per-step checks: the same frames, faster.
- `openql -H` takes `-i key:NAME@T` and `-i type:TEXT@T`, so both machines can be driven without a window. CI now boots SMSQ/E and checks that SuperBASIC answers, as it does for Minerva.
- `-m` says when it is outside the machine's range, and refuses a value that is not a number.
- WIN1 takes a QXL.WIN made by QPC or Q-emuLator, which names no disk geometry: before, SMSQ/E read the wrong sectors past its first 2 MB. QL/E's QLE.WIN boots to its desktop.
- `-i type:` types for SMSQ/E's UK keyboard on the Super QL (`"`, `@`, `#`, `\\`, `|`, `~`).
- Fixes: a negative shift in the Super QL's sound (undefined in C); the picture buffer is freed on exit.
