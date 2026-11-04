OpenQL 0.1.0: the Sinclair QL and the Super QL in one window.

- **Sinclair QL**: a PAL QL with a 68008 at 7.5 MHz, running Minerva, up to 896 KB.
- **Super QL**: a Q40/Q60-compatible board with a 68040 at 40 MHz and 128 MB, running SMSQ/E with its pointer environment, a serial mouse, WIN1 (a QXL.WIN file) and 16-bit colour up to 1024 x 512.

No ROM is included. Fetch them on request with `openql-fetch-roms minerva` and `openql-fetch-roms smsqe` (Python 3), or give your own with `-r`.

| Download | For |
| --- | --- |
| `openql-linux-x86_64.tar.gz` | Linux x86-64 (needs SDL2: `sudo apt install libsdl2-2.0-0`) |
| `openql-windows-x86_64.zip` | Windows 10 and 11, x86-64 (SDL2 included) |
| `openql-macos-x86_64.tar.gz` | macOS on Intel, and Apple silicon through Rosetta (SDL2 included; the first time, right-click and Open) |

Not yet: floppies, microdrives, sound, the network, serial input.
