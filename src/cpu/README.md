# AC090's integer core and FPU

The 68000-family core OpenQL runs: `ac090.c` (the integer core, with `ac090_mmu.c` and `ac090_fpu.c` included into it) and `ac090.h`. It is AmiCradle's AC090 core, interpreter only: the QL's 68008 model and the Super QL's 68040 model with its FPU. AmiCradle's JIT, magic functions and task cores are not here.

It came from DalsinAI/amicradle at c35de227 (4 Nov 2026), `machines/cpus/m68k-ac090/native`. Its history is there.
