/*
 * Saved states: a machine as it stood, in a file, for the same openql build
 * and the same ROM. The boards write their own struct and memory after a
 * header; the CPU's registers are the part of ac090 before its jmp_buf.
 * Disks are not in a state: the cartridges, floppies and WIN1 that are in
 * when a state is loaded stay in.
 */
#ifndef OPENQL_STATE_H
#define OPENQL_STATE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "cpu/ac090.h"

typedef struct state_header {
    char magic[8];              /* "OPENQLST" */
    uint32_t version;           /* STATE_VERSION */
    uint32_t machine;           /* 1 QL, 2 Super QL */
    uint32_t board_bytes;       /* sizeof the board's struct: the same build */
    uint32_t ram_bytes;
    uint32_t rom_hash;          /* the same ROM */
    uint32_t spare;
} state_header;

#define STATE_VERSION 1

static inline uint32_t state_hash(const uint8_t *p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

static inline int state_write_header(FILE *f, uint32_t machine, uint32_t board_bytes, uint32_t ram_bytes, uint32_t rom_hash) {
    state_header h;
    memset(&h, 0, sizeof h);
    memcpy(h.magic, "OPENQLST", 8);
    h.version = STATE_VERSION; h.machine = machine; h.board_bytes = board_bytes;
    h.ram_bytes = ram_bytes; h.rom_hash = rom_hash;
    return fwrite(&h, sizeof h, 1, f) == 1 ? 0 : -1;
}

/* 0 if the file's header is for this machine, build, RAM and ROM; else -1
 * with the reason in why. */
static inline int state_check_header(FILE *f, uint32_t machine, uint32_t board_bytes, uint32_t ram_bytes, uint32_t rom_hash, const char **why) {
    state_header h;
    if (fread(&h, sizeof h, 1, f) != 1 || memcmp(h.magic, "OPENQLST", 8)) { *why = "not an openql state"; return -1; }
    if (h.version != STATE_VERSION || h.board_bytes != board_bytes) { *why = "made by another openql version"; return -1; }
    if (h.machine != machine) { *why = "for the other machine"; return -1; }
    if (h.ram_bytes != ram_bytes) { *why = "made with another RAM size"; return -1; }
    if (h.rom_hash != rom_hash) { *why = "made with another ROM"; return -1; }
    return 0;
}

/* The CPU as it was: registers, caches' contents and clock; the bus, the
 * JIT's and the MMU's host-side caches stay this run's. */
static inline void state_cpu_take(ac090 *c, const ac090 *from) {
    memcpy(c, from, offsetof(ac090, abort));
    c->mmu_on = c->model == AC090_68040 && (c->tc & 0x8000);
    memset(c->atc_key, 0, sizeof c->atc_key);
    c->mmu_regs_valid = 0;
    c->mmu_in_fault = 0;
    c->pace_rem = from->pace_rem;
    c->bus_waited = from->bus_waited;
    c->recheck = 1;
    c->run_page = NULL;
}

#endif
