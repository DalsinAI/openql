# OpenQL: make (the openql program), make test (the boards' own cases),
# make qlrun (the QL's headless boot runner). Needs a C compiler and SDL2.

CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=gnu11 -Wall -Wno-unused-function -D_FILE_OFFSET_BITS=64 -Isrc
LDLIBS  ?=
SDL_CFLAGS := $(shell pkg-config --cflags sdl2 2>/dev/null || sdl2-config --cflags)
SDL_LIBS   := $(shell pkg-config --libs sdl2 2>/dev/null || sdl2-config --libs)
PREFIX  ?= /usr/local

CPU     = src/cpu/ac090.c
CPU_DEP = $(CPU) src/cpu/ac090.h src/cpu/ac090_mmu.c src/cpu/ac090_fpu.c src/cpu/fpu_special.h
QL      = src/ql/ql.c src/ql/mdv.c
SQL     = src/superql/superql.c src/superql/fdc.c
HOST    = src/hostser.c src/settings.c src/ui/ui.c
ifeq ($(OS),Windows_NT)
NETLIBS = -lws2_32
endif

all: openql

openql: src/openql.c $(QL) $(SQL) $(HOST) $(CPU_DEP) src/ql/ql.h src/superql/superql.h src/superql/fdc.h src/hostser.h src/settings.h src/ui/ui.h src/ui/font8x16.h src/state.h
	$(CC) $(CFLAGS) $(SDL_CFLAGS) src/openql.c $(QL) $(SQL) $(HOST) $(CPU) $(SDL_LIBS) -lm $(NETLIBS) $(LDLIBS) -o $@

qlrun: src/ql/qlrun.c $(QL) $(CPU_DEP) src/ql/ql.h
	$(CC) $(CFLAGS) src/ql/qlrun.c $(QL) $(CPU) -lm -o $@

tests/ql_test: src/ql/ql_test.c $(QL) $(CPU_DEP) src/ql/ql.h
	$(CC) $(CFLAGS) src/ql/ql_test.c $(QL) $(CPU) -lm -o $@

tests/superql_test: src/superql/superql_test.c $(SQL) $(CPU_DEP) src/superql/superql.h src/superql/fdc.h
	$(CC) $(CFLAGS) src/superql/superql_test.c $(SQL) $(CPU) -lm -o $@

tests/settings_test: tests/settings_test.c src/settings.c src/settings.h
	$(CC) $(CFLAGS) tests/settings_test.c src/settings.c -o $@

tests/fdc_test: tests/fdc_test.c src/superql/fdc.c src/superql/fdc.h
	$(CC) $(CFLAGS) tests/fdc_test.c src/superql/fdc.c -o $@

test: tests/ql_test tests/superql_test tests/fdc_test tests/settings_test
	./tests/ql_test
	./tests/superql_test
	./tests/fdc_test
	./tests/settings_test
	python3 -I tests/test_fetch_roms.py

install: openql
	install -D -m 755 openql $(DESTDIR)$(PREFIX)/bin/openql
	install -D -m 755 tools/openql-fetch-roms $(DESTDIR)$(PREFIX)/bin/openql-fetch-roms
	install -D -m 644 roms.json $(DESTDIR)$(PREFIX)/share/openql/roms.json

clean:
	rm -f openql qlrun tests/ql_test tests/superql_test tests/fdc_test tests/settings_test

.PHONY: all test install clean
