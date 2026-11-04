# AC090's integer core and FPU

The 68000-family core OpenQL runs: `ac090.c` (the integer core, with `ac090_mmu.c` and `ac090_fpu.c` included into it) and `ac090.h`. It is AmiCradle's AC090 core, interpreter only: the QL's 68008 model and the Super QL's 68040 model with its FPU. AmiCradle's JIT, magic functions and task cores are not here.

It comes from gfoscpuemulators' `m68k-ac090/native`, as DalsinAI/amicradle holds it at `machines/cpus/` (last taken from amicradle ab21d986, 4 Nov 2026, with the interpreter's plain-instruction runs). Its history is there. Take a newer core by copying `ac090.c` and `ac090.h` from there, then run `make test` and the boot checks.
