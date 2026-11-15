# Sinclair QL board in C

A PAL Sinclair QL for the C AC090 core on its 68000 model: the 68008 at 7.5 MHz, the ZX8301 display chip, the ZX8302 peripheral chip and the 8049 IPC behind it. Written from the QL's documented registers. No other emulator's code is in it.

`openql -M ql` runs it in a window. Its Firmware/ROM (Minerva) is fetched on request by `tools/openql-fetch-roms` and is never in this repository.

## What the board has

| Part | What it does here |
| --- | --- |
| Map | ROM $00000-$0BFFF, the ROM port $0C000-$0FFFF (`-c CART`), I/O $18000-$1BFFF, 128 KB RAM at $20000, expansion RAM from $40000 up to 896 KB in all. 20 address lines, so the map repeats every 1 MB |
| 68008 | AC090's 68000 model through the interpreter, 7.5 MHz. Each bus access costs 4 more clocks (the 8-bit bus), 8 on the screen's 64 KB |
| ZX8301 | Mode 4 (512x256, black, red, green, white) and mode 8 (256x256, eight colours, flash), scr0 at $20000 or scr1 at $28000, blanking (`mc_stat`, $18063) |
| ZX8302 | The clock (seconds from 1961, from the host's clock, reset and stepped as on the QL), the frame interrupt at 50 Hz on level 2, interrupt masks and clears, ser1 output at the programmed baud rate with its transmit interrupt |
| IPC | The two-wire link as QDOS drives it: status, read keyboard (up to 7 keys with SHIFT/CTRL/ALT), KEYROW, sound (BEEP's pitch and duration; the window plays it as a square wave at the first pitch, without the sweeps, wrap or fuzz), random, test |
| Microdrives | mdv1_ to mdv8_ (`mdv.c`), worked by QDOS's own microdrive code through the ZX8302. Each cartridge is a loop of tape cells, one byte time (40 us) each, holding a byte or a gap; it moves under the head only while its drive is selected. The PLL hunts for a preamble (zeros, then two $FFs) after each reset and hands each byte after it to pc_trak1/2 with pc..rxrd; a write puts the byte in pc_tdata out at the next cell, and erase leaves gaps. A gap's start raises the gap interrupt, and enabling it in a gap raises it too. The loop has a splice at its end where nothing records: QDOS's FORMAT fails on a cartridge where every sector verifies. Images are QLAY's and the MiSTer QL core's (255 sectors of 686 bytes); a new or empty file is a blank cartridge, saved when the drive stops after a write; a read-only file has its tab out. FORMAT gives about 225 sectors. A drive with no cartridge: its motor runs, the gap stays on and a gap interrupt comes each 40 ms, so QDOS says "not found" and stops it |

Not yet: floppies or a hard disk, the network, ser1 and ser2 input, BEEP's sweeps and fuzz, and a faster CPU (Gold Card style). AC090's JIT is not used: it runs only as a 68020 and 68040, and a 68008's pace needs none.

## Files

| File | What it is |
| --- | --- |
| `ql.h`, `ql.c` | The board |
| `qlrun.c` | Boots a ROM headless, saves the screen each second as PPM, types keys (`-i key:f1@3 -i 'type:PRINT 1\n@6'`) |
| `ql_test.c` | The board's cases without a ROM (`make test`) |
| `../openql.c` | The window: the host's keys on the QL's keyboard (Backspace and Del are CTRL with an arrow, F6-F10 are SHIFT with F1-F5), and BEEP |

```sh
make qlrun
./qlrun -s 20 -m 640 -p shot -i key:f1@3 -i 'type:PRINT "HELLO", 6*7\n@8' ~/.local/share/openql/roms/minerva-1.98j4.rom
```

## Checked, 20 Oct 2026

Minerva 1.98j4 on `qlrun` and in AmiCradle, where this board was first written: the boot screen with the RAM (128 KB and 640 KB) and the host's date, F1 and F2, SuperBASIC running a typed FOR loop, `PRINT DATE$`, `MODE 8` with PAPER and INK, BEEP. It reaches SuperBASIC in about 4 emulated seconds after F1 (the empty `mdv1_boot` attempt).
