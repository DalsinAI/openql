# OpenQL: make (the openql program), make test (the boards' own cases),
# make qlrun (the QL's headless boot runner). Needs a C compiler and SDL2.

CC      ?= cc
CFLAGS  ?= -O2 -g
# -ffp-contract=off: the FPU's double-double code (src/cpu/fpu_soft.c) needs
# every double operation rounded as written, never fused into one.
CFLAGS  += -std=gnu11 -Wall -Wno-unused-function -ffp-contract=off -D_FILE_OFFSET_BITS=64 -Isrc
LDLIBS  ?=
SDL_CFLAGS := $(shell pkg-config --cflags sdl2 2>/dev/null || sdl2-config --cflags)
SDL_LIBS   := $(shell pkg-config --libs sdl2 2>/dev/null || sdl2-config --libs)
PREFIX  ?= /usr/local

CPU     = src/cpu/ac090.c
CPU_DEP = $(CPU) src/cpu/ac090.h src/cpu/ac090_mmu.c src/cpu/ac090_fpu.c src/cpu/fpu_special.h \
          src/cpu/fpu_soft.c src/cpu/fpu_soft_tables.h src/cpu/fpu_decimal.c
QL      = src/ql/ql.c
SQL     = src/superql/superql.c src/superql/fdc.c

all: openql

openql: src/openql.c $(QL) $(SQL) $(CPU_DEP) src/ql/ql.h src/superql/superql.h src/superql/fdc.h
	$(CC) $(CFLAGS) $(SDL_CFLAGS) src/openql.c $(QL) $(SQL) $(CPU) $(SDL_LIBS) -lm $(LDLIBS) -o $@

qlrun: src/ql/qlrun.c $(QL) $(CPU_DEP) src/ql/ql.h
	$(CC) $(CFLAGS) src/ql/qlrun.c $(QL) $(CPU) -lm -o $@

tests/ql_test: src/ql/ql_test.c $(QL) $(CPU_DEP) src/ql/ql.h
	$(CC) $(CFLAGS) src/ql/ql_test.c $(QL) $(CPU) -lm -o $@

tests/superql_test: src/superql/superql_test.c $(SQL) $(CPU_DEP) src/superql/superql.h src/superql/fdc.h
	$(CC) $(CFLAGS) src/superql/superql_test.c $(SQL) $(CPU) -lm -o $@

tests/fdc_test: tests/fdc_test.c src/superql/fdc.c src/superql/fdc.h
	$(CC) $(CFLAGS) tests/fdc_test.c src/superql/fdc.c -o $@

# The FPU's results, bit for bit: the host's own FPU (the x87 on x86, the
# integer one on ARM64), and the integer one built for this host too.
tests/fpu_test: tests/fpu_test.c $(CPU_DEP)
	$(CC) $(CFLAGS) tests/fpu_test.c $(CPU) -lm -o $@

tests/fpu_soft_test: tests/fpu_test.c $(CPU_DEP)
	$(CC) $(CFLAGS) -DAC090_FPU_SOFT=1 tests/fpu_test.c $(CPU) -lm -o $@

test: tests/ql_test tests/superql_test tests/fdc_test tests/fpu_test tests/fpu_soft_test
	./tests/ql_test
	./tests/superql_test
	./tests/fdc_test
	./tests/fpu_test
	./tests/fpu_soft_test
	python3 -I tests/test_fetch_roms.py

install: openql
	install -D -m 755 openql $(DESTDIR)$(PREFIX)/bin/openql
	install -D -m 755 tools/openql-fetch-roms $(DESTDIR)$(PREFIX)/bin/openql-fetch-roms
	install -D -m 644 roms.json $(DESTDIR)$(PREFIX)/share/openql/roms.json

clean:
	rm -f openql qlrun tests/ql_test tests/superql_test tests/fdc_test tests/fpu_test tests/fpu_soft_test

.PHONY: all test install clean
