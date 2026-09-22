/* sandbox.c — bare-CPU mode for validating standalone SM83 assembly.

   cpu.init  creates a boot-ROM-less machine backed by a flat 32KB ROM buffer
             (all zeros by default), post-boot register state, LCD on.
   cpu.load  pokes code anywhere (ROM writes go to the flat buffer directly).
   cpu.exec  runs with instruction + cycle accounting, optional PC bounds,
             HALT detection, and until-addr stopping.
*/
#include <string.h>
#include <stdlib.h>
#include <Core/gb.h>
#include <Core/memory.h>
#include <Core/random.h>
#include "ctx.h"
#include "agent_ext.h"



static void sandbox_default_state(void)
{
    GB_registers_t *r = GB_get_registers(CTX.gb);
    r->af = 0x01B0; r->bc = 0x0013; r->de = 0x00D8; r->hl = 0x014D;
    r->sp = 0xFFFE; r->pc = 0x0000;
    GB_write_memory(CTX.gb, 0xFFFF, 0x00); /* IE */
    GB_write_memory(CTX.gb, 0xFF40, 0x91); /* LCDC on, BG on */
    GB_write_memory(CTX.gb, 0xFF47, 0xE4); /* BGP */
    ext_set_ime(CTX.gb, false);
    ext_clear_halt(CTX.gb);
}

bool cmd_cpu_init(const jval_t *p)
{
    const char *model_name = j_str(p, "model", "dmg");
    const gbemu_model_t *mm = gbemu_find_model(model_name);
    if (!mm) { set_err("unknown model '%s'", model_name); return false; }
    uint64_t seed = (uint64_t)j_int(p, "seed", 9001);
    const char *fillhex = j_str(p, "fill", "00");
    unsigned fill = strtoul(fillhex, NULL, 16) & 0xFF;
    bool vectors_ret = j_bool(p, "vectors_ret", true);
    bool lcd_on = j_bool(p, "lcd", true);

    core_dispose();
    free_analysis_state();
    CTX.cycles_raw = 0;

    memset(CTX.sandbox_rom_storage, fill, sizeof(CTX.sandbox_rom_storage));
    if (vectors_ret) {
        /* make interrupt/RST vectors harmless: 0xC9 = RET */
        for (unsigned v = 0x40; v <= 0x60; v += 8) CTX.sandbox_rom_storage[v] = 0xC9;
    }

    GB_random_seed(seed);
    CTX.gb = GB_alloc();
    GB_init(CTX.gb, mm->model);
    core_setup_callbacks();
    GB_load_rom_from_buffer(CTX.gb, CTX.sandbox_rom_storage, sizeof(CTX.sandbox_rom_storage));
    ext_finish_boot(CTX.gb); /* never map the (unloaded) boot ROM */
    sandbox_default_state();
    if (!lcd_on) GB_write_memory(CTX.gb, 0xFF40, 0x00);

    CTX.loaded = true;
    CTX.sandbox = true;
    CTX.seed = seed;
    snprintf(CTX.rom_path, sizeof(CTX.rom_path), "(sandbox)");
    snprintf(CTX.title, sizeof(CTX.title), "SANDBOX");
    CTX.instr_count = CTX.frame_count = 0;

    jw_fmt(&CTX.out, "{\"sandbox\":true,\"model\":");
    jw_esc(&CTX.out, model_name);
    jw_fmt(&CTX.out, ",\"seed\":%llu,\"fill\":\"%02x\",\"vectors_ret\":%s,\"lcd\":%s}",
           (unsigned long long)seed, fill, vectors_ret ? "true" : "false",
           lcd_on ? "true" : "false");
    return true;
}

bool cmd_cpu_reset(const jval_t *p)
{
    if (!CTX.sandbox) { set_err("not in sandbox mode (cpu.init first)"); return false; }
    size_t sz;
    uint8_t *ram = GB_get_direct_access(CTX.gb, GB_DIRECT_ACCESS_RAM, &sz, NULL);
    memset(ram, 0, sz);
    uint8_t *vram = GB_get_direct_access(CTX.gb, GB_DIRECT_ACCESS_VRAM, &sz, NULL);
    memset(vram, 0, sz);
    uint8_t *oam = GB_get_direct_access(CTX.gb, GB_DIRECT_ACCESS_OAM, &sz, NULL);
    memset(oam, 0, sz);
    uint8_t *hram = GB_get_direct_access(CTX.gb, GB_DIRECT_ACCESS_HRAM, &sz, NULL);
    memset(hram, 0, sz);
    sandbox_default_state();
    if (j_bool(p, "clear_rom", false))
        memset(CTX.sandbox_rom_storage, 0, sizeof(CTX.sandbox_rom_storage));
    CTX.instr_count = 0;
    CTX.cycles_raw = 0;
    CTX.stop_pending = CTX.last_stop_valid = false;
    jw_raw(&CTX.out, "{\"reset\":true}");
    return true;
}

bool cmd_cpu_load(const jval_t *p)
{
    if (!CTX.sandbox) { set_err("not in sandbox mode (cpu.init first)"); return false; }
    uint16_t addr;
    if (!parse_u16(j_get(p, "addr"), &addr)) { set_err("bad 'addr'"); return false; }
    const char *hex = j_str(p, "data", NULL);
    if (!hex) { set_err("missing 'data'"); return false; }
    size_t hexlen = strlen(hex);
    size_t n = 0;
    size_t cap = hexlen / 2 + 2;
    uint8_t *buf = malloc(cap);
    if (!buf) { set_err("oom"); return false; }
    for (size_t i = 0; i + 1 < hexlen; i += 2) {
        buf[n++] = (uint8_t)strtoul((char[]){hex[i], hex[i + 1], 0}, NULL, 16);
    }
    if ((uint32_t)addr + n > 0x10000) { free(buf); set_err("write wraps past $FFFF"); return false; }
    size_t romsz; uint16_t bk;
    uint8_t *rom = GB_get_direct_access(CTX.gb, GB_DIRECT_ACCESS_ROM, &romsz, &bk);
    CTX.probing = true;
    for (size_t i = 0; i < n; i++) {
        uint16_t a = addr + (uint16_t)i;
        if (a < 0x8000 && (size_t)a < romsz) rom[a] = buf[i];
        else GB_write_memory(CTX.gb, a, buf[i]);
    }
    CTX.probing = false;
    free(buf);
    jw_fmt(&CTX.out, "{\"written\":%zu,\"addr\":\"%04x\"}", n, addr);
    return true;
}

/* Returns a static reason string; NULL = still running limits ok */
static const char *sandbox_run(uint64_t max_steps, uint64_t max_cycles_raw,
                               bool bound_armed, uint16_t bound_lo, uint16_t bound_hi,
                               bool until_armed, uint16_t until_addr,
                               bool detect_halt, uint64_t *steps_out, uint64_t *cycles_out)
{
    GB_registers_t *r = GB_get_registers(CTX.gb);
    uint64_t steps = 0;
    uint64_t cyc_start = CTX.cycles_raw;
    install_analysis_hooks();
    while (steps < max_steps) {
        uint16_t pc = r->pc;
        if (until_armed && pc == until_addr && steps > 0) { *steps_out = steps; *cycles_out = CTX.cycles_raw - cyc_start; return "until_addr"; }
        if (bound_armed && (pc < bound_lo || pc > bound_hi)) { *steps_out = steps; *cycles_out = CTX.cycles_raw - cyc_start; return "left_bounds"; }
        unsigned c = GB_run(CTX.gb);
        CTX.cycles_raw += c;
        CTX.instr_count++;
        steps++;
        if (CTX.cycles_raw - cyc_start >= max_cycles_raw) { *steps_out = steps; *cycles_out = CTX.cycles_raw - cyc_start; return "cycle_limit"; }
        if (detect_halt && ext_halted(CTX.gb)) { *steps_out = steps; *cycles_out = CTX.cycles_raw - cyc_start; return "halted"; }
        if (CTX.stop_pending) { *steps_out = steps; *cycles_out = CTX.cycles_raw - cyc_start; return CTX.stop_reason[0] ? CTX.stop_reason : "stopped"; }
    }
    *steps_out = steps;
    *cycles_out = CTX.cycles_raw - cyc_start;
    return "steps_completed";
}

bool cmd_cpu_exec(const jval_t *p)
{
    if (!CTX.sandbox) { set_err("not in sandbox mode (cpu.init first)"); return false; }

    /* optional inline load: write 'data' at 'at', set pc unless overridden */
    if (j_has(p, "data")) {
        const char *hex = j_str(p, "data", NULL);
        uint16_t at = 0;
        if (!hex || !parse_u16(j_get(p, "at"), &at)) { set_err("'data' requires hex string and 'at'"); return false; }
        size_t hexlen = strlen(hex);
        size_t cap = hexlen / 2 + 2, n = 0;
        uint8_t *buf = malloc(cap);
        if (!buf) { set_err("oom"); return false; }
        for (size_t i = 0; i + 1 < hexlen; i += 2)
            buf[n++] = (uint8_t)strtoul((char[]){hex[i], hex[i + 1], 0}, NULL, 16);
        if ((uint32_t)at + n > 0x10000) { free(buf); set_err("write wraps past $FFFF"); return false; }
        size_t romsz;
        uint8_t *rom = GB_get_direct_access(CTX.gb, GB_DIRECT_ACCESS_ROM, &romsz, NULL);
        CTX.probing = true;
        for (size_t i = 0; i < n; i++) {
            uint16_t a = at + (uint16_t)i;
            if (a < 0x8000 && (size_t)a < romsz) rom[a] = buf[i];
            else GB_write_memory(CTX.gb, a, buf[i]);
        }
        CTX.probing = false;
        free(buf);
        if (!j_has(p, "pc")) GB_get_registers(CTX.gb)->pc = at;
    }

    /* optional register setup */
    GB_registers_t *r = GB_get_registers(CTX.gb);
    const char *r16[6] = {"pc", "sp", "af", "bc", "de", "hl"};
    uint16_t *rptr[6] = {&r->pc, &r->sp, &r->af, &r->bc, &r->de, &r->hl};
    for (int i = 0; i < 6; i++) {
        const jval_t *v = j_get(p, r16[i]);
        if (!v) continue;
        uint16_t x;
        if (!parse_u16(v, &x)) { set_err("bad '%s'", r16[i]); return false; }
        *rptr[i] = (i == 2) ? x & 0xFFF0 : x;
    }
    if (j_has(p, "ime")) ext_set_ime(CTX.gb, j_bool(p, "ime", false));
    if (j_has(p, "ie")) {
        uint16_t ie;
        if (parse_u16(j_get(p, "ie"), &ie)) GB_write_memory(CTX.gb, 0xFFFF, (uint8_t)ie);
    }
    if (j_has(p, "a")) { uint16_t x; parse_u16(j_get(p, "a"), &x); r->af = (uint16_t)((x << 8) | (r->af & 0xF0)); }
    if (j_has(p, "b")) { uint16_t x; parse_u16(j_get(p, "b"), &x); r->bc = (uint16_t)((x << 8) | (r->bc & 0xFF)); }
    if (j_has(p, "c")) { uint16_t x; parse_u16(j_get(p, "c"), &x); r->bc = (uint16_t)((r->bc & 0xFF00) | x); }
    if (j_has(p, "d")) { uint16_t x; parse_u16(j_get(p, "d"), &x); r->de = (uint16_t)((x << 8) | (r->de & 0xFF)); }
    if (j_has(p, "e")) { uint16_t x; parse_u16(j_get(p, "e"), &x); r->de = (uint16_t)((r->de & 0xFF00) | x); }
    if (j_has(p, "h")) { uint16_t x; parse_u16(j_get(p, "h"), &x); r->hl = (uint16_t)((x << 8) | (r->hl & 0xFF)); }
    if (j_has(p, "l")) { uint16_t x; parse_u16(j_get(p, "l"), &x); r->hl = (uint16_t)((r->hl & 0xFF00) | x); }

    uint64_t max_steps = (uint64_t)j_int(p, "steps", 256);
    if (max_steps > 100000000) { set_err("steps cap 100M"); return false; }
    uint64_t max_cycles = (uint64_t)j_int(p, "max_cycles8", max_steps * 64);

    bool bound_armed = false;
    uint16_t bound_lo = 0, bound_hi = 0;
    const jval_t *bnd = j_arr(p, "bound");
    if (bnd) {
        if (bnd->n == 2) {
            if (!parse_u16(bnd->items[0], &bound_lo) || !parse_u16(bnd->items[1], &bound_hi)) {
                set_err("bad 'bound' (want [lo,hi])"); return false;
            }
            bound_armed = true;
        }
        /* empty array = explicitly unbounded */
    }
    else if (j_has(p, "data") && j_bool(p, "bound", true)) {
        /* implicit: bound to written region */
        uint16_t at;
        parse_u16(j_get(p, "at"), &at);
        size_t n0 = strlen(j_str(p, "data", "")) / 2;
        if (n0) { bound_lo = at; bound_hi = (uint16_t)(at + n0 - 1); bound_armed = true; }
    }
    bool until_armed = false;
    uint16_t until_addr = 0;
    if (j_has(p, "until_addr")) {
        if (!parse_u16(j_get(p, "until_addr"), &until_addr)) { set_err("bad 'until_addr'"); return false; }
        until_armed = true;
    }
    bool detect_halt = j_bool(p, "detect_halt", true);

    /* issuing an explicit exec always starts from a runnable CPU state:
     * a previous exec may have left the CPU in HALT (kept by detect_halt) */
    ext_clear_halt(CTX.gb);

    CTX.stop_pending = false;
    CTX.stop_reason[0] = 0;
    uint64_t steps = 0, cycles = 0;
    const char *reason = sandbox_run(max_steps, max_cycles, bound_armed, bound_lo, bound_hi,
                                     until_armed, until_addr, detect_halt, &steps, &cycles);

    jw_t *w = &CTX.out;
    jw_fmt(w, "{\"steps\":%llu,\"cycles8\":%llu,\"cycles_t\":%llu,",
           (unsigned long long)steps, (unsigned long long)cycles, (unsigned long long)(cycles / 2));
    jw_raw(w, "\"reason\":");
    jw_esc(w, reason);
    jw_fmt(w, ",\"pc\":\"%04x\",\"sp\":\"%04x\",\"halted\":%s,", r->pc, r->sp,
           ext_halted(CTX.gb) ? "true" : "false");
    jw_fmt(w, "\"cycles_raw_total\":%llu,", (unsigned long long)CTX.cycles_raw);
    jw_raw(w, "\"regs\":{");
    const char *rn[6] = {"af", "bc", "de", "hl", "sp", "pc"};
    uint16_t rv[6] = {r->af, r->bc, r->de, r->hl, r->sp, r->pc};
    for (int i = 0; i < 6; i++) jw_fmt(w, "%s\"%s\":\"%04x\"", i ? "," : "", rn[i], rv[i]);
    jw_raw(w, "}}");
    return true;
}
