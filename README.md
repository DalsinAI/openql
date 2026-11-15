# OpenQL

A Sinclair QL emulator: one program, one window, two machines.

- **Sinclair QL**: a PAL QL, its 68008 at 7.5 MHz, the ZX8301 display, the ZX8302 and its IPC keyboard. It runs Minerva, the open replacement for QDOS, up to 896 KB of RAM.
- **Super QL**: a Q40/Q60-compatible board with a 68040 at 40 MHz and 128 MB, the most any QL-family machine had. It runs SMSQ/E with its pointer environment, a serial mouse, a WIN1 hard disk (a QXL.WIN file), two floppy drives, sound and 16-bit colour up to 1024 x 512.

The boards and the 68k core are our own C, written from the machines' documented registers and the SMSQ/E sources' hardware definitions. They began in AmiCradle; OpenQL is them on their own, without AmiCradle's manager.

## Download

Releases for Linux, Windows and macOS are on the Releases page. OpenQL draws in a window of its own on the host (F11 or `-F` for full screen): no browser.

## Build

Needs a C compiler, SDL2 and Python 3 (for the ROM fetcher and the tests).

```sh
sudo apt install build-essential libsdl2-dev   # Debian, Ubuntu
make
make test
```

## ROMs

No Firmware/ROM is in this repository or shipped with OpenQL. `tools/openql-fetch-roms` downloads each from its own project on request and checks it against `roms.json`:

```sh
tools/openql-fetch-roms minerva     # Minerva 1.98j4 for the QL (GPL-2.0)
tools/openql-fetch-roms smsqe       # SMSQ/E 3.43's Q40/Q60 build for the Super QL
```

They go to `~/.local/share/openql/roms` (`%APPDATA%\openql\roms` on Windows, or `$OPENQL_ROMS`), where `openql` looks for them.

## Run

```sh
./openql                            # the Super QL with SMSQ/E
./openql -w QXL.WIN                 # ... with a QL hard disk as WIN1 (-R: read only)
./openql -f games.img               # ... with a QL floppy image in FLP1_ (a second -f: FLP2_)
./openql -M ql                      # a QL with Minerva: F1 or F2, then SuperBASIC
./openql -M ql -m 128               # a stock 128 KB QL
```

| Option | What it does |
| --- | --- |
| `-M ql\|superql` | The machine (the Super QL by default) |
| `-r ROM` | A Firmware/ROM of your own instead of the fetched one |
| `-m RAM` | KB on the QL (128 to 896, 640 by default), MB on the Super QL (up to 128) |
| `-w FILE`, `-R` | The Super QL's WIN1: a QXL.WIN file; `-R` read only |
| `-f FILE` | A floppy image in the Super QL's FLP1_, a second in FLP2_: raw, 720 KB, 1.44 MB or 2.88 MB. A read-only file is a write-protected disk; an empty file is a blank disk to FORMAT |
| `-c FILE` | A 16 KB cartridge in the QL's ROM port |
| `-s FILE`, `-p FILE` | ser1 (COM1) and the Super QL's LPT1 output |
| `-F`, `-q` | Full screen; no sound |
| `-H SECONDS`, `-o FILE.ppm` | No window: run that long as fast as the host goes, then save the screen (tests) |
| `-i key:NAME@T`, `-i type:TEXT@T` | With `-H`, press a key (`f1` to `f10`, `enter`, `esc`, `space`, `tab`, the arrows) or type text (`\n` for ENTER) at T seconds, on either machine |

In the window: **F11** full screen, **Ctrl+F12** the reset button, **Ctrl+Shift+F12** a power cycle, **Ctrl+F10** pause, **Ctrl+F9** takes the disk out of FLP1_ (with Shift, FLP2_). Drop a floppy image on the window to put it in FLP1_ (hold Shift for FLP2_). The QL has no Backspace or Delete, so those keys send CTRL with left or right, as QL typists did. F6 to F10 are SHIFT with F1 to F5. On the Super QL the host's pointer over the window moves SMSQ/E's pointer.

## What is not done yet

The QL's microdrives and disks, the Super QL floppy controller's interrupt, the network, and serial input from the host. The QL's BEEP plays its pitch only (no sweeps or fuzz). See `docs/` for each board.

## Layout

| Path | What it is |
| --- | --- |
| `src/openql.c` | The program: the window, keyboard, mouse and timing |
| `src/ql/` | The QL board, its cases (`ql_test.c`) and `qlrun`, a headless boot runner that types keys |
| `src/superql/` | The Super QL board and its cases |
| `src/cpu/` | AC090's 68k core (interpreter) |
| `tools/openql-fetch-roms`, `roms.json` | The ROM fetcher and what it fetches |

Designed, tested and delivered by The AmiCradle Team. MIT licence.
