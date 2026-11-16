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
| Video | QL modes 4 and 8 from the QL screen at $20000; 16-bit colour (GGGGGRRRRRBBBBBx) at 512 x 256 or 1024 x 512 from video RAM. The picture is 1024 x 512, with smaller modes doubled. SMSQ/E starts in the mode its configuration names (the SMSQ block's OSPD item, "Initial display mode": 1 QL mode 4, 2 small 16 bit, 3 large 16 bit); `sql_set_display` sets it in the loaded ROM image |
| Keyboard | PC set 2 scan codes, as SMSQ/E's driver reads them. The window's keys are turned into them |
| Mouse | A Microsoft serial mouse on COM2, as Q40 owners fitted one. It answers 'M' when SMSQ/E's driver raises DTR and RTS, then sends a 3-byte packet each frame there is movement or a button change. The host's pointer over the window becomes its movement (a QL-mode pixel is two of the picture's), and its left and right buttons are the mouse's. SMSQ/E's pointer environment is built in, so nothing needs loading |
| Clock | MK48T08 style: BCD time from the host's clock plus an offset that SMSQ/E sets; 2 KB of NVRAM, one byte every 4 |
| Multi-I/O | COM1-COM4 as 16550s with receive FIFOs and interrupts (COM1's output goes to stdout or `-s FILE`, COM2 has the mouse), LPT1 (each strobed byte goes to `-p FILE`), the floppy controller (below), IDE |
| IDE | The primary channel's master (WIN1_) and slave, PIO, LBA and CHS; both take the task file, the one bit 4 of the head register selects answers and takes the command. SMSQ/E's Q40 driver gives every WINn_ the master until `WIN_DRIVE` says otherwise, so with a slave fitted (the host folder, `-D`) openql finds the driver's linkage once it is linked ("WIN" at $42, a hard disk at $4C) and sets WIN2_'s target ($231) to 1 and partition ($251) to 0, as `WIN_DRIVE 2,1` does, unless they were set already. A QXL.WIN file is a whole QL hard disk: SMSQ/E takes a disk with no partition table as one QLWA partition, so it is WIN1. Each data word has the disk's first byte high, as SMSQ/E's Q40 driver reads a CHS disk (it swaps bytes only for a QXL.WIN inside FAT32). SMSQ/E's driver takes the disk's CHS geometry from the QLWA header; a QXL.WIN made by QPC or Q-emuLator has none there, so the drive shows its own (63 sectors, 16 heads) in sector 0 as read and keeps the file's zeros, and a disk made on a Q40 keeps the geometry it names. QL/E's 80 MB QLE.WIN boots to its desktop this way |

| Floppies | An 82077-style controller at $3F0 (`fdc.c`), without DMA as the Q40 has none: the CPU moves each byte through the FIFO while the status register says so. FLP1_ and FLP2_ take raw QL floppy images (80 tracks, two sides, 512-byte sectors: 720 KB DD, 1.44 MB HD, 2.88 MB ED). Reads, writes, multi-track, FORMAT (an empty file becomes a disk), READ ID, the disk change line, write protection (a read-only file). A transfer ends at the track's last sector as the chip does without TC (ST0 abnormal end, ST1 end of cylinder). Its IRQ 6 shows in the master chip's external interrupt register (bit 3) while DOR's DMA/IRQ gate is on; a command other than SENSE INTERRUPT drops the reset's four polling statuses |
| External interrupts | The external interrupt register ($FF000004) shows the multi-I/O card's IRQ lines: IRQ 3 (COM2, COM4) bit 0, IRQ 4 (COM1, COM3) bit 1, the floppy controller's IRQ 6 bit 3, the IDE disk's INTRQ (IRQ 14, unless nIEN; reading the status drops it) bit 6. Bit 0 of the enable register ($FF000010) lets them raise the master chip's external interrupt, on level 2. SMSQ/E's own drivers poll and leave it off |
| Sound | The two 8-bit DACs at $FF008000 (left) and $FF008004 (right). The sample interrupt, level 6, at 20 kHz or 40 kHz (`$FF00002C`) while it is enabled (`$FF000014`), acknowledged at `$FF000028`. Each sample takes the DACs as they are, and the window plays them at 48 kHz |

Serial input: `-S FILE` or `-s tcp:...` (README) feed COM1 once a channel is open (receive interrupt on, DTR and OUT2), so nothing is lost while SMSQ/E sets the port up.

Not yet: AC090's JIT. The mouse follows the host's pointer by movement, so the two can drift apart.

## Files

| File | What it is |
| --- | --- |
| `superql.h`, `superql.c` | The board |
| `fdc.h`, `fdc.c` | The floppy controller (`tests/fdc_test.c` has its cases) |
| `superql_test.c` | The board's cases with a small hand-assembled ROM (`make test`) |

```sh
make test
# SMSQ/E boots and SuperBASIC answers, with no window (as CI checks it):
./openql -H 12 -i 'type:PRINT 6*7\n@8' -o sb.ppm
python3 -I tests/boot_check.py sb.ppm ffffff 60 516,2,600,24   # the 42 at the top of #1
```
