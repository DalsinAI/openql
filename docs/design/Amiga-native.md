# OpenQL on the Amiga: the QL's ROM on the Amiga's own 68k

Dale, 4 Nov 2026: OpenQL on the Amiga runs natively, with the QL's ROM on the Amiga's CPU, not through the C emulator. A 68k interpreting another 68k would make the Super QL crawl, and even a 7.5 MHz QL would be slow on a real 68040. The Amiga already has the CPU the ROM was written for. What it lacks is the QL's memory map and chips. This design gives it both: the 68040's MMU builds the map, and OpenQL plays the chips.

The QL-on-the-Amiga emulators of the 1990s worked the same way: they took the machine over and ran QDOS on its processor.

## What it needs

- **A 68040 or 68060 with its MMU.** This is the project's own build target ("68040-only builds"). Its page tables are what let an unmodified ROM see ROM at 0 and the I/O where it expects it. A 68030 MMU can do the same later with its own table format. A 68000 to 68020 without an MMU is out of scope.
- **AmigaOS 3.2 or our Open ROM** to start from. OpenQL is an ordinary program until the user starts the QL.
- **Fast RAM**: 1 MB for a QL, and up to 128 MB for a Super QL (what fits).
- **A display**: AGA or OCS/ECS planar for the QL modes, and RTG (OpenRTG, Picasso96 or CyberGraphX) for the Super QL's 16-bit modes.

## The two machines

| | QL (Minerva) | Super QL (SMSQ/E, Q40 build) |
| --- | --- | --- |
| ROM | `minerva-1.98j4.rom` at logical $0, 48 KB, read only | `Q40.rom` at logical $FE000000, and low ROM below $18000 at reset |
| RAM | logical $20000 to $FFFFF (128 KB to 896 KB) | logical $0 up to 128 MB |
| I/O OpenQL plays | ZX8302 and IPC at $18000 (keyboard, clock, interrupts, ser1), the ZX8301 mode register | the master chip $FF000000, the clock and NVRAM $FF020000, the multi-I/O card $FF400000 (UARTs, LPT, FDC, IDE), the DACs |
| Screen | 32 KB at $20000 or $28000, mode 4 and 8 | the QL screen at $20000, and video RAM at $FE800000 for 16-bit modes |

The board code that decides what each register does is the same C as OpenQL on the PC: `src/ql/ql.c` and `src/superql/superql.c`, with `fdc.c`. That code is split so its register logic (`*_io_read8`, `*_io_write8`, the IPC, the FDC, the clock) builds without AC090. The Amiga build links those parts with a native CPU layer in place of `ac090.c`.

## How it runs

### 1. Starting and taking over

OpenQL opens a small window under AmigaOS to choose the machine, ROM, RAM and disks. Pressing Start:

1. Allocates the QL's memory (MEMF_FAST, aligned to the page size). It loads the ROM into it, and reads each disk image whole into RAM, since dos.library can't be called once the machine is taken over.
2. Builds a 68040 page table (4 KB pages) for the QL's logical map:
   - **ROM pages:** mapped, write protected. A write to ROM faults and is dropped, as on the QL.
   - **RAM and screen pages:** mapped, cache mode copyback for RAM. Screen pages are write-through, so the display converter reads what the CPU wrote.
   - **I/O pages:** invalid. Every access faults into OpenQL (below).
   - **Everything else:** invalid, which gives an access fault as on the QL's open bus. Reads of nothing return $FF.
3. `Disable()`, saves the Amiga's state (VBR, TC, URP, SRP, ITT/DTT, CACR, INTENA, DMACON, the copper list) and the CIA timers, and installs its own.
4. Sets VBR to OpenQL's own vector table and enters the ROM's reset vector in supervisor mode, interrupts at level 7.

AmigaOS is frozen while the QL runs. Leaving with Ctrl + both Amiga keys + Esc (the Amiga's own reset chord, caught by the keyboard handler before it reaches the CIA reset line) writes the disk images back, restores the saved state, `Enable()`s and returns to the window.

### 2. The vector table: whose exceptions are whose

The QL's ROM thinks it owns the vectors at 0 (the QL's 68008 has no VBR). OpenQL's table sits in front of it.

- **Traps (TRAP #0 to #15), line A, line F, illegal, CHK, TRAPV, divide by zero and trace** forward to the ROM's own vector. Each is a `move.l (rom_vector).l,-(sp); rts`-style jump, which costs a few cycles. QDOS's TRAP #1 to #4 system calls run at full speed.
- **Access error (vector 2)** is OpenQL's I/O path (next section). A fault that isn't an I/O page goes to the ROM's bus error handler, as the QL would see it.
- **Privilege violation** emulates `MOVE SR,<ea>` in user mode, which the 68000 allowed and the 68010+ do not, then returns. Everything else forwards to the ROM.
- **Amiga hardware interrupts (levels 1 to 6)** are OpenQL's:
  - Level 3 VERTB is the QL's 50 Hz frame. It runs the display conversion, then raises the QL's frame interrupt by entering the ROM's level 2 handler (QL) or the master chip's frame interrupt (Super QL).
  - Level 2 CIA-A takes the keyboard (Amiga raw codes into the same keymaps as the PC build, then the IPC or the set 2 queue).
  - Level 6 CIA-B is the Super QL's sample interrupt and the QL's IPC timing.
  - Paula's audio interrupt plays BEEP and the DACs.
  - An interrupt OpenQL makes for the QL is delivered only when the QL's own interrupt mask allows it: OpenQL keeps the QL's SR level and holds the request until then.

Minerva runs on the Super Gold Card's 68020 and SMSQ/E on the Q40's 68040, so both already handle the larger exception frames of the 68020 and later. No ROM patch is needed for frames.

### 3. I/O by page fault and trace

An access to an I/O page raises an access error. The 68040's frame gives the fault address, the size, read or write, and for a write the data. Writes are done on the spot: the frame's data goes to the board's `io_write8`, and the write is marked done in the frame's write-back status.

A read cannot be completed from the frame on a 68040. OpenQL uses map-and-trace instead:

1. Fill a private "I/O window" page with the register values `io_read8` gives for that page, at their offsets.
2. Map the faulting page to it for this one instruction, and set the trace bit in the stacked SR.
3. Return. The instruction re-runs and reads the window.
4. The trace exception unmaps the page again, restores the trace bit as it was, and lets any read side effect happen. A read side effect is, for example, taking a key from the queue or the IPC link's next bit.

The same path serves the QL's $18000 page and the Super QL's $FF000000 pages. Registers whose value changes with time, like the clock, the IPC link bit and the FDC FIFO, are computed at step 1, so the read sees the value of that moment.

This costs about one access fault and one trace exception per I/O access, a few microseconds on a 68040. QDOS polls the IPC a few hundred times a second, and SMSQ/E's floppy and IDE drivers move data a word at a time through a port. That is a few thousand faults a second during disk transfers. Measure it in the lab, and if IDE is too slow, take the data port's REP-style loops in the fault handler: recognise `move.w (a0),(a1)+` on the data port and complete the whole sector in one fault.

### 4. The display

- **QL mode 4 (512 x 256, 4 colours):** Amiga hires, 640 x 256 PAL, with 512 pixels used and the rest border, in two bitplanes. Each QL word holds 8 pixels as a green byte and a red byte. Two 256-entry tables turn each byte into a planar byte for plane 0 (green) and plane 1 (red). The palette maps the four combinations to black, red, green and white.
- **QL mode 8 (256 x 256, 8 colours + flash):** Amiga lores 320 x 256 in three bitplanes. The same tables split the G, R and B bits, and flash toggles a fourth plane's colours on the frame count.
- **Super QL 16-bit modes:** through RTG, by converting GGGGGRRRRRBBBBBx to the card's format each frame for the pages that changed. Without RTG, OpenQL says so and offers the QL modes only.
- **Only changed pages:** the 68040's page descriptors carry a modified bit. Each VERTB converts only the screen pages whose M bit is set, then clears it. A quiet SuperBASIC screen costs nothing.

### 5. Disks

The images are loaded into RAM at Start and written back on exit, because dos.library can't run while the machine is taken over.

- **QL:** microdrives come later (the ZX8302's microdrive interface, from images). Minerva has no floppy driver of its own, so a QL disk interface (Trump Card or Gold Card style) comes after that.
- **Super QL:** FLP1_ and FLP2_ use `fdc.c` as on the PC. WIN1 uses the IDE code, and a WIN larger than the RAM free can't be loaded.

A later version can hand disk requests back to AmigaOS without a full exit, by switching to the saved Amiga state for the length of a dos.library call and back.

### 6. Sound

Paula plays the QL's BEEP as a looped square wave sample at the IPC's pitch. The Super QL's DAC values are gathered at each sample interrupt into a buffer that Paula plays at about 20 kHz (the Q40's rate), on channels 0 and 1 for left and right.

## Layout in the repository

| Path | What it is |
| --- | --- |
| `src/ql/ql_io.c`, `src/superql/superql_io.c` | The register logic, split out of `ql.c` and `superql.c`, shared by the PC and Amiga builds |
| `src/amiga/openql.c` | The AmigaOS program: the window, loading, Start, exit, writing back |
| `src/amiga/takeover.s` | Saving and restoring the Amiga's state, the vector table, entering the ROM |
| `src/amiga/mmu040.c` | The page tables, map-and-trace, the modified bits |
| `src/amiga/fault.s` | The access error, trace and privilege violation handlers |
| `src/amiga/display.c` | The QL modes to bitplanes, and RTG for the 16-bit modes |
| `src/amiga/input.c`, `src/amiga/sound.c` | CIA-A keyboard, Paula |
| `build-amiga.sh` | Builds `OpenQL` with OpenNDK's 68040 GCC (soft float, -Os), as the stoves build our modules |

## Build and test

- **Build:** OpenNDK's 68k GCC on the dev machine (amigachromebuild), queued behind `~/WorkingSpace/build.lock` with the other heavy builds. Never on Daletop.
- **Cases:** the register logic keeps its host tests (`make test`). The Amiga layer gets cases that run on a lab instance: the page table's map (read ROM at 0, a write to ROM dropped, an I/O read through map-and-trace, a write through the fault frame), the vector forwarding, and the mode 4 conversion tables.
- **Lab:** a Cradle lab copy of an OS 3.2.3 instance on AC090's 68040 with its MMU. AC090 emulates the 68040's page tables and access error frames, so the takeover runs there first. After that, Dale's real hardware.

## Order of work

1. Split the register logic out of `ql.c` and `superql.c` (no change in behaviour, host tests unchanged).
2. The QL on a 68040: takeover and exit, the page table, map-and-trace, the vector table, the keyboard, mode 4 and 8 on AGA. The goal is Minerva's F1/F2 screen, then SuperBASIC typing.
3. BEEP, then QL microdrive images.
4. The Super QL: SMSQ/E Q40 at $FE000000, the master chip, the serial mouse from the Amiga mouse, FLP1_, WIN1 from RAM, the DACs, and RTG for the 16-bit modes.
5. The 68060 (its access fault frame differs: format 4) and the 68030 (its own table format), if wanted.

## Risks

- **AC090's MMU and access error frames** must match a real 68040 closely enough for map-and-trace. The lab finds out before real hardware does.
- **Cache coherence:** the QL code is loaded into RAM we own. CPUSHA after loading, and copyback only on RAM pages.
- **SMSQ/E's own use of the MMU:** SMSQ/E's Q40 build doesn't set up the 68040's MMU as far as known. If it does write TC, URP or SRP with MOVEC, nothing traps, because SMSQ/E runs in supervisor mode, and it would replace OpenQL's map. Check in the lab; if it does, OpenQL patches those MOVECs into TRAPs when it loads the ROM.
- **Timing loops** written for a 7.5 MHz 68008 run far faster on a 68040. QDOS times with the frame interrupt and the ZX8302 clock, so most software is fine, but some games may run too fast. A later option is a speed limiter (a CACR cache-off mode, or a periodic pause in VERTB).
