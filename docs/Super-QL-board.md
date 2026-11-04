# Super QL board in C

A Q40/Q60-compatible board for the C AC090 core on its 68040 model. Dale asked for it on 20 Oct 2026: "lets do a super ql, as much memory as any of the machines hobbyists ever built". Peter Graf's Q60 took 128 MB, the most any QL-family machine has had, and one Q60 was fitted with a 68040. This board is that machine: SMSQ/E's own Q40 memory map, a 68040 at 40 MHz and up to 128 MB.

It is written from the Q40 definitions in the SMSQ/E sources (`keys/q40`, `keys/q40_multiIO`) and SMSQ/E's Q40 drivers. No other emulator's code is in it.

`openql` (or `openql -M superql`) runs it in a window. SMSQ/E is its Firmware/ROM: `tools/openql-fetch-roms smsqe` fetches it on request, and it is never in this repository.

## What the board has

| Part | What it does here |
| --- | --- |
| Map | RAM from $0 (4 to 128 MB), ROM at $FE000000 (up to 8 MB), video RAM at $FE800000, the master chip at $FF000000, the clock's NVRAM at $FF020000, ISA I/O at $FF400000 (port << 2, +1 for bytes) |
| Low ROM | At reset, reads below $18000 come from the ROM and writes go to RAM. A write to $FF018000 switches to RAM, which is then write protected below $18000. A write to $FF010000 switches back |
| 68040 | AC090's 68040 model with its FPU, through the interpreter for now |
| Master chip | The interrupt register (keyboard, serial, 50 Hz frame, external), its enables and acknowledges, the display mode, the LED. Everything is on level 2 |
| Video | QL modes 4 and 8 from the QL screen at $20000; 16-bit colour (GGGGGRRRRRBBBBBx) at 512 x 256 or 1024 x 512 from video RAM. The picture is 1024 x 512, with smaller modes doubled |
| Keyboard | PC set 2 scan codes, as SMSQ/E's driver reads them. The window's keys are turned into them |
| Mouse | A Microsoft serial mouse on COM2, as Q40 owners fitted one. It answers 'M' when SMSQ/E's driver raises DTR and RTS, then sends a 3-byte packet each frame there is movement or a button change. The host's pointer over the window becomes its movement (a QL-mode pixel is two of the picture's), and its left and right buttons are the mouse's. SMSQ/E's pointer environment is built in, so nothing needs loading |
| Clock | MK48T08 style: BCD time from the host's clock plus an offset that SMSQ/E sets; 2 KB of NVRAM, one byte every 4 |
| Multi-I/O | COM1-COM4 as 16550s with receive FIFOs and interrupts (COM1's output goes to stdout or `-s FILE`, COM2 has the mouse), LPT1 (each strobed byte goes to `-p FILE`), a floppy controller that reports no drive, IDE |
| IDE | One disk on the primary channel, PIO, LBA and CHS. A QXL.WIN file is a whole QL hard disk: SMSQ/E takes a disk with no partition table as one QLWA partition, so it is WIN1. Each data word has the disk's first byte high, as SMSQ/E's Q40 driver reads a CHS disk (it swaps bytes only for a QXL.WIN inside FAT32) |

Not yet: floppies, sound (the two DACs are accepted, not played), serial input from the host, the network, and AC090's JIT. The mouse follows the host's pointer by movement, so the two can drift apart.

## Files

| File | What it is |
| --- | --- |
| `superql.h`, `superql.c` | The board |
| `superql_test.c` | The board's cases with a small hand-assembled ROM (`make test`) |

```sh
make test
```
