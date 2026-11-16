# OpenQL: make (the openql program), make test (the boards' own cases),
# make qlrun (the QL's headless boot runner). Needs a C compiler and SDL2, and
# for openql's own windows git, meson, ninja (and GTK 3 on Linux) to build
# libui-ng (tools/build-libui).

CC      ?= cc
CFLAGS  ?= -O2 -g
# -ffp-contract=off: the FPU's double-double code (src/cpu/fpu_soft.c) needs
# every double operation rounded as written, never fused into one.
CFLAGS  += -std=gnu11 -Wall -Wno-unused-function -ffp-contract=off -D_FILE_OFFSET_BITS=64 -Isrc
LDLIBS  ?=
SDL_CFLAGS := $(shell pkg-config --cflags sdl2 2>/dev/null || sdl2-config --cflags)
SDL_LIBS   := $(shell pkg-config --libs sdl2 2>/dev/null || sdl2-config --libs)
PREFIX  ?= /usr/local
WINDRES ?= windres

CPU     = src/cpu/ac090.c
CPU_DEP = $(CPU) src/cpu/ac090.h src/cpu/ac090_mmu.c src/cpu/ac090_fpu.c src/cpu/fpu_special.h \
          src/cpu/fpu_soft.c src/cpu/fpu_soft_tables.h src/cpu/fpu_decimal.c
QL      = src/ql/ql.c src/ql/mdv.c
SQL     = src/superql/superql.c src/superql/fdc.c
HOST    = src/hostser.c src/hostdir.c src/settings.c
DEBUG   = src/debug.c src/cpu/dis68k.c
DEBUG_H = src/debug.h src/cpu/dis68k.h

LIBUI     = build/libui-ng
LIBUI_A   = $(LIBUI)/build/meson-out/libui.a
UNAME    := $(shell uname -s)
ifeq ($(OS),Windows_NT)
NETLIBS   = -lws2_32
GUI_LIBS  = -luser32 -lkernel32 -lgdi32 -lcomctl32 -luxtheme -lmsimg32 -lcomdlg32 -ld2d1 -ldwrite \
            -lole32 -loleaut32 -loleacc -luuid -lwindowscodecs -lstdc++
GUI_RES   = build/openql-res.o
else ifeq ($(UNAME),Darwin)
GUI_LIBS  = -framework Foundation -framework AppKit -lobjc
else
GUI_LIBS  = $(shell pkg-config --libs gtk+-3.0) -ldl
endif

all: openql

openql: src/openql.c src/gui.c $(QL) $(SQL) $(HOST) $(DEBUG) $(DEBUG_H) $(CPU_DEP) src/ql/ql.h src/superql/superql.h src/superql/fdc.h src/hostser.h src/hostdir.h src/settings.h src/gui.h src/state.h $(LIBUI_A) $(GUI_RES)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -I$(LIBUI) src/openql.c src/gui.c $(QL) $(SQL) $(HOST) $(DEBUG) $(CPU) $(GUI_RES) $(LIBUI_A) $(GUI_LIBS) $(SDL_LIBS) -lm $(NETLIBS) $(LDLIBS) -o $@

$(LIBUI_A):
	./tools/build-libui $(LIBUI)

# Windows: the manifest that asks for the host's own (Common Controls 6) look
build/openql-res.o: src/openql.rc src/openql.manifest
	mkdir -p build
	$(WINDRES) src/openql.rc -O coff -o $@

qlrun: src/ql/qlrun.c $(QL) $(CPU_DEP) src/ql/ql.h
	$(CC) $(CFLAGS) src/ql/qlrun.c $(QL) $(CPU) -lm -o $@

tests/ql_test: src/ql/ql_test.c $(QL) $(CPU_DEP) src/ql/ql.h
	$(CC) $(CFLAGS) src/ql/ql_test.c $(QL) $(CPU) -lm -o $@

tests/superql_test: src/superql/superql_test.c $(SQL) $(CPU_DEP) src/superql/superql.h src/superql/fdc.h
	$(CC) $(CFLAGS) src/superql/superql_test.c $(SQL) $(CPU) -lm -o $@

tests/settings_test: tests/settings_test.c src/settings.c src/settings.h
	$(CC) $(CFLAGS) tests/settings_test.c src/settings.c -o $@

tests/hostdir_test: tests/hostdir_test.c src/hostdir.c src/hostdir.h
	$(CC) $(CFLAGS) tests/hostdir_test.c src/hostdir.c -o $@

tests/fdc_test: tests/fdc_test.c src/superql/fdc.c src/superql/fdc.h
	$(CC) $(CFLAGS) tests/fdc_test.c src/superql/fdc.c -o $@

tests/debug_test: tests/debug_test.c $(DEBUG) $(DEBUG_H) $(CPU_DEP)
	$(CC) $(CFLAGS) tests/debug_test.c $(DEBUG) $(CPU) -lm -o $@

# The FPU's results, bit for bit: the host's own FPU (the x87 on x86 Linux,
# the integer one elsewhere), and the integer one built for this host too.
tests/fpu_test: tests/fpu_test.c $(CPU_DEP)
	$(CC) $(CFLAGS) tests/fpu_test.c $(CPU) -lm -o $@

tests/fpu_soft_test: tests/fpu_test.c $(CPU_DEP)
	$(CC) $(CFLAGS) -DAC090_FPU_SOFT=1 tests/fpu_test.c $(CPU) -lm -o $@

test: tests/ql_test tests/superql_test tests/fdc_test tests/settings_test tests/hostdir_test tests/debug_test tests/fpu_test tests/fpu_soft_test
	./tests/ql_test
	./tests/superql_test
	./tests/fdc_test
	./tests/settings_test
	cd tests && ./hostdir_test
	./tests/debug_test
	./tests/fpu_test
	./tests/fpu_soft_test
	python3 -I tests/test_fetch_roms.py

install: openql
	install -D -m 755 openql $(DESTDIR)$(PREFIX)/bin/openql
	install -D -m 755 tools/openql-fetch-roms $(DESTDIR)$(PREFIX)/bin/openql-fetch-roms
	install -D -m 644 roms.json $(DESTDIR)$(PREFIX)/share/openql/roms.json

clean:
	rm -f build/openql-res.o openql qlrun tests/ql_test tests/superql_test tests/fdc_test tests/settings_test tests/hostdir_test tests/debug_test tests/fpu_test tests/fpu_soft_test

.PHONY: all test install clean
