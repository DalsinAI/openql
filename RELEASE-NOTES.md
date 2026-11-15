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

Not yet: the QL's microdrives, the network, serial input, the floppy controller's interrupt.

New since v0.1.0:

- Native builds for Apple silicon and for Linux on ARM64. The Super QL's 68040 FPU no longer needs the x87's 80-bit long double: on ARM64 it works its 80-bit values out in integers, as AmiCradle's AC090 does on the Raspberry Pi 5. Arithmetic, conversions and packed decimal give the same bits as the x86 build (checked by `make test` on every build); the transcendentals are correctly rounded, where the x86 build's C library is out by an ulp now and then. The Windows and Intel macOS builds use the integer FPU too, so they give Linux's bits where their C libraries used to round FMOD, FINT, FSIN of a large angle and others their own way.
- AC090's interpreter runs plain instructions without its per-step checks: the same frames, faster.
- `openql -H` takes `-i key:NAME@T` and `-i type:TEXT@T`, so both machines can be driven without a window. CI now boots SMSQ/E and checks that SuperBASIC answers, as it does for Minerva.
- `-m` says when it is outside the machine's range, and refuses a value that is not a number.
- WIN1 takes a QXL.WIN made by QPC or Q-emuLator, which names no disk geometry: before, SMSQ/E read the wrong sectors past its first 2 MB. QL/E's QLE.WIN boots to its desktop.
- `-i type:` types for SMSQ/E's UK keyboard on the Super QL (`"`, `@`, `#`, `\\`, `|`, `~`).
- Fixes: a negative shift in the Super QL's sound (undefined in C); the picture buffer is freed on exit.
