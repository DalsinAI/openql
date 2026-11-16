# OpenQL

A Sinclair QL emulator: one program, one window, two machines.

- **Sinclair QL**: a PAL QL, its 68008 at 7.5 MHz, the ZX8301 display, the ZX8302 and its IPC keyboard. It runs Minerva, the open replacement for QDOS, up to 896 KB of RAM.
- **Super QL**: a Q40/Q60-compatible board with a 68040 at 40 MHz and 128 MB, the most any QL-family machine had. It runs SMSQ/E with its pointer environment, a serial mouse, a WIN1 hard disk (a QXL.WIN file), two floppy drives, sound and 16-bit colour up to 1024 x 512.

The boards and the 68k core are our own C, written from the machines' documented registers and the SMSQ/E sources' hardware definitions. They began in AmiCradle; OpenQL is them on their own, without AmiCradle's manager.

## Download

Releases for Linux (x86-64 and ARM64), Windows and macOS (Apple silicon and Intel) are on the Releases page. OpenQL draws in a window of its own on the host (F11 or `-F` for full screen): no browser.

## Build

Needs a C compiler, SDL2 and Python 3 (for the ROM fetcher and the tests). The launcher and the F12 menu are the host's own windows and controls (GTK 3 on Linux, Win32 on Windows, Cocoa on macOS) through [libui-ng](https://github.com/libui-ng/libui-ng) (MIT), which `make` fetches and builds into `build/` the first time, with git, meson and ninja.

```sh
sudo apt install build-essential libsdl2-dev libgtk-3-dev meson ninja-build git   # Debian, Ubuntu
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

Started with nothing on its command line, `openql` opens its launcher: choose the machine, its memory, its disks, the picture and the speed, then Start. **Get QL/E** downloads QL/E (QLE.WIN, a QL hard disk full of software, from sinclairql.net; it needs Python 3, like the ROM fetcher), puts it in the Super QL's WIN1_ and sets the machine up for it; **Fetch the ROMs** gets Minerva and SMSQ/E. The launcher keeps its choices in `openql.cfg` beside the ROMs folder, and can be told not to show at start (`-L` shows it anyway).

With options, openql starts that machine straight away:

```sh
./openql -L                         # the launcher, whatever the settings say
./openql -M superql                 # the Super QL with SMSQ/E
./openql -w QXL.WIN                 # ... with a QL hard disk as WIN1 (-R: read only)
./openql -w QLE.WIN                 # ... with QL/E, a QL hard disk full of software (sinclairql.net)
./openql -f games.img               # ... with a QL floppy image in FLP1_ (a second -f: FLP2_)
./openql -M ql                      # a QL with Minerva: F1 or F2, then SuperBASIC
./openql -M ql -m 128               # a stock 128 KB QL
./openql -M ql -d games.mdv         # ... with a microdrive cartridge in mdv1_ (a second -d: mdv2_)
./openql -g large -w QLE.WIN        # the Super QL in 16-bit colour at 1024 x 512
```

| Option | What it does |
| --- | --- |
| `-M ql\|superql` | The machine (the Super QL by default) |
| `-r ROM` | A Firmware/ROM of your own instead of the fetched one |
| `-m RAM` | KB on the QL (128 to 896, 640 by default), MB on the Super QL (up to 128) |
| `-w FILE`, `-R` | The Super QL's WIN1: a QXL.WIN file; `-R` read only |
| `-f FILE` | A floppy image in the Super QL's FLP1_, a second in FLP2_: raw, 720 KB, 1.44 MB or 2.88 MB. A read-only file is a write-protected disk; an empty file is a blank disk to FORMAT |
| `-D FOLDER` | A folder on the host as the Super QL's FLP2_: its files (up to 2.8 MB) are a QL disk, `game.bas` as `game_bas` and a folder as a directory. What the QL saves there is written back to the folder when the disk comes out or openql stops: changed files under their own names, new ones under the QL's. An executable's data space is kept as an `XTcc` trailer, as uQLx and sQLux keep it. Files deleted on the QL stay on the host |
| `-d FILE` | A microdrive cartridge in the QL's mdv1_, a second in mdv2_: a `.mdv` image of 255 sectors of 686 bytes (QLAY, MiSTer). A new or empty file is a blank cartridge to FORMAT; a read-only file has its write-protect tab out. Writes are saved to the file when the drive stops |
| `-c FILE` | A 16 KB cartridge in the QL's ROM port |
| `-s FILE` | ser1's (COM1's) output goes to a file |
| `-s tcp:PORT`, `-s tcp:HOST:PORT` | ser1 as a null-modem cable over TCP, both ways: openql listens on 127.0.0.1:PORT, or connects to HOST:PORT. Two openqls joined this way can `COPY` files between them, and a terminal program (`nc 127.0.0.1 PORT`) can talk to the QL |
| `-S FILE` | ser1's input: the file's bytes come in once, when a program opens ser1 |
| `-p FILE` | The Super QL's LPT1 output |
| `-g ql\|high\|large` | The display SMSQ/E starts in on the Super QL: `ql` (QL mode 4, as SMSQ/E comes), `high` (512 x 256 in 16-bit colour) or `large` (1024 x 512 in 16-bit colour). Set in SMSQ/E's own configuration ("Initial display mode", as MenuConfig sets it) in the ROM as openql loads it; the ROM file is not changed. The launcher and the F12 menu have it too |
| `-F`, `-q` | Full screen; no sound |
| `-l FILE` | Start from a saved state |
| `-H SECONDS`, `-o FILE.ppm`, `-O FILE` | No window: run that long as fast as the host goes, then save the screen (tests), or the machine's state |
| `-i key:NAME@T`, `-i type:TEXT@T` | With `-H`, press a key (`f1` to `f10`, `enter`, `esc`, `space`, `tab`, the arrows) or type text (`\n` for ENTER) at T seconds, on either machine |
| `-b ADDR` | With `-H`, stop when the CPU reaches ADDR (hex), print the registers and the code there, and end (status 3 if it never got there) |

In the window, **F12** opens openql's menu: put a disk or cartridge in or take it out (or a new blank cartridge on the QL, or a host folder as FLP2_ on the Super QL), save the machine's state in one of four slots and load it again, type the host's clipboard in, change the speed (real time, 2x, 4x, or as fast as it goes) and the picture (4:3 at the window's size, 4:3 in whole pixels, or filling the window), go full screen, choose the Super QL's display for the next reset, reset, open the debugger, or go back to the launcher. A saved state is the machine with its memory, for the same openql, ROM and memory size; the disks in the drives are not part of it.

Also: **F11** full screen, **Ctrl+Shift+V** types the clipboard in, **Ctrl+F8** as fast as it goes (again: back), **Ctrl+F12** the reset button, **Ctrl+Shift+F12** a power cycle, **Ctrl+F10** pause, **Ctrl+F11** the debugger, **Ctrl+F9** takes the disk out of FLP1_ or the cartridge out of mdv1_ (with Shift, FLP2_ or mdv2_). Drop a floppy or microdrive image on the window to put it in FLP1_ or mdv1_ (hold Shift for the second drive). The QL has no Backspace or Delete, so those keys send CTRL with left or right, as QL typists did. F6 to F10 are SHIFT with F1 to F5. On the Super QL the host's pointer over the window moves SMSQ/E's pointer.

### The debugger

**Ctrl+F11**, or **Debugger...** in the F12 menu, stops the CPU and opens the debugger beside the machine, a window of the host's own controls:

- **Registers**: D0-D7, A0-A7, PC, SR with its flags, USP, SSP and VBR, each editable while the CPU is stopped (hex).
- **Code**: the disassembly about the PC (68000 to 68040, the 68040's FPU and MMU instructions included), which it follows as the CPU stops; or type an address (hex, a register, or one plus or minus hex such as `a6+$20`) and Show. Click a line to choose it, double-click it to set or clear a breakpoint there.
- **Break**, **Continue**, **Step** (one instruction), **Step over** (a `bsr`, `jsr`, `trap` or `dbcc` loop runs whole), **Run to here** (the line chosen).
- **Memory**: 16 bytes a line, with their characters; **Write** puts bytes (hex) in RAM. Only RAM and ROM are shown: the hardware's registers show as `--`, since reading one could take a key from the keyboard's queue.

Addresses are the CPU's: with SMSQ/E's MMU on, the debugger reads through the 68040's tables as the CPU would. Closing the window lets the machine run on; its breakpoints stay, and one that is reached opens the debugger again. A breakpoint costs speed while it is set (each instruction is checked), none once they are cleared.

## What is not done yet

A host folder on the QL (which has only its microdrives) and as a hard disk (WIN2_); the QL's disks, and the QL's own network (QLAN) wire: two machines talk through ser1 over TCP instead. Displays larger than 1024 x 512: that is the most the Q40 and Q60 boards had, and the most SMSQ/E's Q40 build drives. The QL's BEEP plays its pitch only (no sweeps or fuzz). See `docs/` for each board.

## Layout

| Path | What it is |
| --- | --- |
| `src/openql.c` | The program: the window, keyboard, mouse and timing, the launcher and the menu |
| `src/gui.c` | openql's own windows (the launcher, the F12 menu and the debugger), made of the host's controls through libui-ng |
| `src/settings.c` | The launcher's settings file and openql's folders |
| `src/debug.c`, `src/cpu/dis68k.c` | The debugger's breakpoints, steps, registers and memory; the 68000 to 68040 disassembler |
| `src/state.h` | Saved states' header and the CPU's part |
| `src/hostser.c`, `src/hostdir.c` | ser1's host end (a file, TCP); a host folder as a QLWA disk |
| `src/ql/` | The QL board, its cases (`ql_test.c`) and `qlrun`, a headless boot runner that types keys |
| `src/superql/` | The Super QL board and its cases |
| `src/cpu/` | AC090's 68k core (interpreter) |
| `tools/openql-fetch-roms`, `roms.json` | The fetcher (ROMs and QL/E) and what it fetches |

Designed, tested and delivered by The AmiCradle Team. MIT licence.
