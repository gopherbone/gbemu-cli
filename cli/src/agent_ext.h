#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <Core/gb.h>

/* Accessors into GB_gameboy_t internals (struct layout is visible in Core/gb.h;
   accessed directly like Tester/main.c does). */

uint16_t ext_rom_bank(GB_gameboy_t *gb);
uint16_t ext_rom0_bank(GB_gameboy_t *gb);
uint16_t ext_wram_bank(GB_gameboy_t *gb);
uint16_t ext_vram_bank(GB_gameboy_t *gb);
uint16_t ext_cart_ram_bank(GB_gameboy_t *gb);
bool ext_ime(GB_gameboy_t *gb);
bool ext_halted(GB_gameboy_t *gb);
bool ext_stopped(GB_gameboy_t *gb);
bool ext_double_speed(GB_gameboy_t *gb);

unsigned ext_backtrace_size(GB_gameboy_t *gb);
/* i = 0 is the most recent call. Returns current call depth. */
void ext_backtrace_entry(GB_gameboy_t *gb, unsigned i, uint16_t *addr, uint16_t *bank, uint16_t *sp);
int ext_call_depth(GB_gameboy_t *gb);

/* A minimal DMG boot ROM: initializes registers to post-boot values and
   unmaps itself, jumping to $0100. Enables fully offline testing. */
extern const unsigned char builtin_dmg_boot[256];

/* Sandbox helpers */
void ext_finish_boot(GB_gameboy_t *gb);           /* skip boot ROM mapping forever */
void ext_set_ime(GB_gameboy_t *gb, bool on);
void ext_clear_halt(GB_gameboy_t *gb);            /* clear halted state */

/* Map of address -> current ROM/WRAM/VRAM bank for display purposes. */
int ext_effective_rom_bank(GB_gameboy_t *gb, uint16_t addr);
