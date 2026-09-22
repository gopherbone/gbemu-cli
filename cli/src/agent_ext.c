#include "agent_ext.h"
#include <Core/gb.h>

uint16_t ext_rom_bank(GB_gameboy_t *gb) { return gb->mbc_rom_bank; }
uint16_t ext_rom0_bank(GB_gameboy_t *gb) { return gb->mbc_rom0_bank; }
uint16_t ext_wram_bank(GB_gameboy_t *gb) { return gb->cgb_ram_bank ? gb->cgb_ram_bank : 1; }
uint16_t ext_vram_bank(GB_gameboy_t *gb) { return gb->cgb_vram_bank; }
uint16_t ext_cart_ram_bank(GB_gameboy_t *gb) { return gb->mbc_ram_bank; }
bool ext_ime(GB_gameboy_t *gb) { return gb->ime; }
bool ext_halted(GB_gameboy_t *gb) { return gb->halted; }
bool ext_stopped(GB_gameboy_t *gb) { return gb->stopped; }
bool ext_double_speed(GB_gameboy_t *gb) { return gb->cgb_double_speed; }

unsigned ext_backtrace_size(GB_gameboy_t *gb) { return gb->backtrace_size; }
int ext_call_depth(GB_gameboy_t *gb) { return gb->debug_call_depth; }

void ext_backtrace_entry(GB_gameboy_t *gb, unsigned i, uint16_t *addr, uint16_t *bank, uint16_t *sp)
{
    /* i = 0 → most recent */
    unsigned idx = gb->backtrace_size - 1 - i;
    *addr = gb->backtrace_returns[idx].addr;
    *bank = gb->backtrace_returns[idx].bank;
    *sp = gb->backtrace_sps[idx];
}

void ext_finish_boot(GB_gameboy_t *gb) { gb->boot_rom_finished = true; }
void ext_set_ime(GB_gameboy_t *gb, bool on) { gb->ime = on; gb->ime_toggle = false; }
void ext_clear_halt(GB_gameboy_t *gb) { gb->halted = false; gb->stopped = false; }

int ext_effective_rom_bank(GB_gameboy_t *gb, uint16_t addr)
{
    if (addr >= 0x4000 && addr < 0x8000)
        return gb->mbc_rom_bank;
    if (addr < 0x4000)
        return gb->mbc_rom0_bank & (gb->rom_size / 0x4000 - 1);
    return -1;
}

/*
 * Minimal DMG boot ROM (256 bytes). Code:
 *   ld sp,$FFFE
 *   ld bc,$01B0 / push bc / pop af   ; AF = $01B0
 *   ld bc,$0013 / ld de,$00D8 / ld hl,$014D
 *   ld a,1 / ldh ($50),a             ; unmap boot ROM
 *   jp $0100                          ; fetched from cartridge after unmap
 * NOTE: bytes $0015-$0017 of the *cartridge* must contain `jp $0100`
 * (C3 00 01); the test ROM generator guarantees this.
 */
const unsigned char builtin_dmg_boot[256] = {
    0x31, 0xFE, 0xFF,       /* 00: ld sp,$FFFE   */
    0x01, 0xB0, 0x01,       /* 03: ld bc,$01B0   */
    0xC5,                   /* 06: push bc       */
    0xF1,                   /* 07: pop af        */
    0x01, 0x13, 0x00,       /* 08: ld bc,$0013   */
    0x11, 0xD8, 0x00,       /* 0B: ld de,$00D8   */
    0x21, 0x4D, 0x01,       /* 0E: ld hl,$014D   */
    0x3E, 0x01,             /* 11: ld a,$01      */
    0xE0, 0x50,             /* 13: ldh ($50),a   */
    0xC3, 0x00, 0x01,       /* 15: jp $0100      */
};
