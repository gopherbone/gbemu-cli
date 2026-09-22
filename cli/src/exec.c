#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <Core/gb.h>
#include <Core/display.h>
#include <Core/debugger.h>
#include <Core/memory.h>
#include "ctx.h"
#include "agent_ext.h"

/* ---------------- hook arming ---------------- */


bool need_exec_cb(void)
{
    /* mem-predicate bps need the exec cb to latch the executing instruction's
       pc (the pc register is already past the instruction by access time) */
    for (int i = 0; i < MAX_BREAKPOINTS; i++)
        if (CTX.bps[i].used && CTX.bps[i].enabled && CTX.bps[i].kind == BP_MEM) return true;
    return CTX.trace.active || CTX.cov_active;
}

bool need_mem_cbs(void)
{
    if (CTX.trace.active && CTX.trace.with_mem) return true;
    for (int i = 0; i < MAX_WATCHPOINTS; i++)
        if (CTX.wps[i].used && CTX.wps[i].enabled) return true;
    /* mem-predicate breakpoints intercept reads/writes through the mem cbs */
    for (int i = 0; i < MAX_BREAKPOINTS; i++)
        if (CTX.bps[i].used && CTX.bps[i].enabled && CTX.bps[i].kind == BP_MEM) return true;
    return false;
}

static void exec_cb(GB_gameboy_t *gb, uint16_t pc, uint8_t opcode);
static uint8_t read_cb(GB_gameboy_t *gb, uint16_t addr, uint8_t data);
static bool write_cb(GB_gameboy_t *gb, uint16_t addr, uint8_t data);

void install_analysis_hooks(void)
{
    GB_set_execution_callback(CTX.gb, need_exec_cb() ? exec_cb : NULL);
    GB_set_read_memory_callback(CTX.gb, need_mem_cbs() ? read_cb : NULL);
    GB_set_write_memory_callback(CTX.gb, need_mem_cbs() ? write_cb : NULL);
}

/* ---------------- trace ring ---------------- */

static void trace_push(uint16_t pc, uint8_t opcode)
{
    trace_t *t = &CTX.trace;
    if (t->pc_filter && (pc < t->pc_lo || pc > t->pc_hi)) return;
    if (t->rom_only && pc >= 0x8000) return;

    trace_entry_t *e = &t->ring[t->head];
    memset(e, 0, sizeof(*e));
    e->pc = pc;
    e->bank = (uint16_t)ext_effective_rom_bank(CTX.gb, pc);
    e->opcode = opcode;
    if (t->with_regs) {
        GB_registers_t *r = GB_get_registers(CTX.gb);
        e->regs[0] = (uint8_t)(r->af >> 8); e->regs[1] = (uint8_t)r->af;
        e->regs[2] = (uint8_t)(r->bc >> 8); e->regs[3] = (uint8_t)r->bc;
        e->regs[4] = (uint8_t)(r->de >> 8); e->regs[5] = (uint8_t)r->de;
        e->regs[6] = (uint8_t)(r->hl >> 8); e->regs[7] = (uint8_t)r->hl;
        e->has_regs = true;
    }
    e->n_mems = 0;
    /* attach memory accesses of the *previous* instruction */
    t->head = (t->head + 1) % t->cap;
    if (t->count == t->cap) t->dropped++;
    else t->count++;
    t->total++;
}

/* ---------------- callbacks ---------------- */

static int eff_bank_for(uint16_t addr)
{
    GB_gameboy_t *gb = CTX.gb;
    if (addr < 0x4000) return ext_rom0_bank(gb);
    if (addr < 0x8000) return ext_rom_bank(gb);
    if (addr >= 0xA000 && addr < 0xC000) return ext_cart_ram_bank(gb);
    if (addr >= 0xD000 && addr < 0xE000) return ext_wram_bank(gb);
    if (addr >= 0x8000 && addr < 0xA000) return ext_vram_bank(gb);
    return 0;
}

static bool wp_match(wp_t *wp, uint16_t addr, bool is_write, int value)
{
    if (!wp->used || !wp->enabled) return false;
    if (is_write && !wp->on_write) return false;
    if (!is_write && !wp->on_read) return false;
    if (addr < wp->addr || addr > wp->end) return false;
    if (is_write && wp->value >= 0 && value != wp->value) return false;
    if (wp->bank >= 0 && eff_bank_for(addr) != wp->bank) return false;
    return true;
}

static void banks_json(jw_t *w)
{
    GB_gameboy_t *gb = CTX.gb;
    jw_fmt(w, "\"banks\":{\"rom0\":%u,\"rom\":%u,\"vram\":%u,\"wram\":%u,\"sram\":%u}",
           ext_rom0_bank(gb), ext_rom_bank(gb), ext_vram_bank(gb),
           ext_wram_bank(gb), ext_cart_ram_bank(gb));
}

void emit_banks_fields(jw_t *w)
{
    jw_raw(w, ",");
    banks_json(w);
}

/* BP_MEM predicate match: everything except the free-form cond expr.
   pc = pc of the instruction performing the access. */
static bool bp_mem_match(bp_t *bp, uint16_t addr, bool is_write, int value, uint16_t pc)
{
    if (!bp->used || !bp->enabled || bp->kind != BP_MEM) return false;
    if (is_write && !bp->on_write) return false;
    if (!is_write && !bp->on_read) return false;
    if (addr < bp->addr || addr > bp->end) return false;
    if (bp->value_given && (value & bp->mask) != bp->value) return false;
    if (bp->bank >= 0 && eff_bank_for(addr) != bp->bank) return false;
    if (bp->pc_eq_given && pc != bp->pc_eq) return false;
    if (bp->pc_not_given && pc == bp->pc_not) return false;
    if (bp->pc_range_given && (pc < bp->pc_lo || pc > bp->pc_hi)) return false;
    return true;
}

static void bp_mem_check(uint16_t addr, uint8_t value, bool is_write)
{
    for (int i = 0; i < MAX_BREAKPOINTS; i++) {
        bp_t *bp = &CTX.bps[i];
        if (!bp_mem_match(bp, addr, is_write, is_write ? value : -1, CTX.cur_instr_pc)) continue;
        bp->hits++;
        if (bp->cond) {
            uint16_t r, b;
            if (!(!GB_debugger_evaluate(CTX.gb, bp->cond, &r, &b) && r)) continue;
        }
        /* hit log: ring-append every match (probing accesses never reach here) */
        if (!CTX.bp_hits) {
            CTX.bp_hits = calloc(BP_HITLOG_CAP, sizeof(bp_hit_t));
            if (!CTX.bp_hits) { set_err("out of memory (hit log)"); return; }
        }
        bp_hit_t *h = &CTX.bp_hits[CTX.bp_hits_head];
        h->bp = (uint32_t)i;
        h->pc = CTX.cur_instr_pc;
        h->addr = addr;
        h->value = value;
        h->is_write = is_write;
        h->effbank = (uint16_t)eff_bank_for(addr);
        CTX.bp_hits_head = (CTX.bp_hits_head + 1) % BP_HITLOG_CAP;
        if (CTX.bp_hits_count < BP_HITLOG_CAP) CTX.bp_hits_count++;
        CTX.bp_hits_total++;

        if (bp->stop && !CTX.stop_pending) {
            CTX.stop_pending = true;
            CTX.bp_fired = true;
            snprintf(CTX.stop_reason, sizeof(CTX.stop_reason),
                     "breakpoint %d (mem-%s $%04X-$%04X)", bp->id,
                     is_write ? "write" : "read", bp->addr, bp->end);
            snprintf(CTX.bp_fired_msg, sizeof(CTX.bp_fired_msg),
                     "breakpoint %d: %s $%04X = %02X (effbank %d, range $%04X-$%04X) at pc=$%04X (rom0=%u rom=%u wram=%u)",
                     bp->id, is_write ? "write" : "read", addr, value,
                     eff_bank_for(addr), bp->addr, bp->end,
                     CTX.cur_instr_pc,
                     ext_rom0_bank(CTX.gb), ext_rom_bank(CTX.gb), ext_wram_bank(CTX.gb));
        }
    }
}

static bool mem_access_common(uint16_t addr, uint8_t value, bool is_write)
{
    /* breakpoint mem predicates */
    if (!CTX.probing) {
        for (int i = 0; i < MAX_BREAKPOINTS; i++) {
            if (CTX.bps[i].used && CTX.bps[i].kind == BP_MEM) {
                bp_mem_check(addr, value, is_write);
                break; /* bp_mem_check scans the whole table */
            }
        }
    }
    /* watchpoints */
    if (!CTX.probing) {
        for (int i = 0; i < MAX_WATCHPOINTS; i++) {
            wp_t *wp = &CTX.wps[i];
            if (!wp_match(wp, addr, is_write, is_write ? value : -1)) continue;
            wp->hits++;
            bool fire = true;
            if (wp->cond) {
                uint16_t r, b;
                fire = !GB_debugger_evaluate(CTX.gb, wp->cond, &r, &b) && r;
            }
            if (fire && wp->stop && !CTX.stop_pending) {
                CTX.stop_pending = true;
                CTX.wp_fired = true;
                snprintf(CTX.wp_fired_msg, sizeof(CTX.wp_fired_msg),
                         "watchpoint %d hit: %s $%04X = %02X (bank %d)",
                         wp->id, is_write ? "write" : "read", addr, value,
                         wp->bank >= 0 ? wp->bank : eff_bank_for(addr));
                snprintf(CTX.stop_reason, sizeof(CTX.stop_reason), "watchpoint");
            }
        }
    }
    /* trace memory capture */
    if (CTX.trace.active && CTX.trace.with_mem && !CTX.probing) {
        if (CTX.mem_cap_n < 4) {
            CTX.mem_cap[CTX.mem_cap_n][0] = is_write;
            CTX.mem_cap[CTX.mem_cap_n][1] = addr >> 8;
            CTX.mem_cap[CTX.mem_cap_n][2] = addr & 0xFF;
            CTX.mem_cap[CTX.mem_cap_n][3] = value;
            CTX.mem_cap_n++;
        }
    }
    return true;
}

static uint8_t read_cb(GB_gameboy_t *gb, uint16_t addr, uint8_t data)
{
    (void)gb;
    mem_access_common(addr, data, false);
    return data;
}

static bool write_cb(GB_gameboy_t *gb, uint16_t addr, uint8_t data)
{
    (void)gb;
    mem_access_common(addr, data, true);
    return true; /* allow the write */
}

static void exec_cb(GB_gameboy_t *gb, uint16_t pc, uint8_t opcode)
{
    (void)gb;
    CTX.cur_instr_pc = pc; /* latch for mem-predicate pc gating + hit reasons */
    CTX.mem_cap_n = 0;
    CTX.mem_cap_pc = pc;
    if (CTX.trace.active) trace_push(pc, opcode);
    if (CTX.cov_active && pc < 0x8000 && CTX.cov_bits) {
        uint16_t bank = pc < 0x4000 ? ext_rom0_bank(CTX.gb) : ext_rom_bank(CTX.gb);
        size_t lin = (size_t)bank * 0x4000 + (pc & 0x3FFF);
        if (lin < CTX.cov_rom_size) {
            size_t byte = lin >> 3;
            if (!(CTX.cov_bits[byte] & (1 << (lin & 7)))) {
                CTX.cov_bits[byte] |= 1 << (lin & 7);
            }
        }
    }
    if (CTX.cov_active) CTX.cov_executed++;
}

/* ---------------- runner ---------------- */

static volatile bool vblank_seen;

static void vblank_cb(GB_gameboy_t *gb, GB_vblank_type_t type)
{
    (void)gb; (void)type;
    vblank_seen = true;
}

static uint16_t cur_pc(void) { return GB_get_registers(CTX.gb)->pc; }

/* breakpoint precheck: stop *before* executing the instruction at a breakpoint */
static bool bp_precheck(void)
{
    uint16_t pc = cur_pc();
    if (CTX.stop_pending) return true;
    if (CTX.last_stop_valid && pc == CTX.last_stop_pc) {
        CTX.last_stop_valid = false; /* just resumed from this stop; skip once */
        return false;
    }
    for (int i = 0; i < MAX_BREAKPOINTS; i++) {
        bp_t *bp = &CTX.bps[i];
        if (!bp->used || !bp->enabled || bp->addr != pc) continue;
        if (bp->kind != BP_PC) continue;
        if (bp->bank >= 0 && eff_bank_for(pc) != bp->bank) continue;
        bool fire = true;
        if (bp->cond) {
            uint16_t r, b;
            fire = !GB_debugger_evaluate(CTX.gb, bp->cond, &r, &b) && r;
        }
        if (!fire) continue;
        bp->hits++;
        CTX.stop_pending = true;
        snprintf(CTX.stop_reason, sizeof(CTX.stop_reason), "breakpoint %d at $%04X", bp->id, pc);
        CTX.last_stop_pc = pc;
        CTX.last_stop_valid = true;
        return true;
    }
    return false;
}

/* run.until address precheck */
static bool until_precheck(void)
{
    if (CTX.until_addr_armed && cur_pc() == CTX.until_addr) {
        if (CTX.last_stop_valid && cur_pc() == CTX.last_stop_pc) {
            CTX.last_stop_valid = false;
            return false;
        }
        CTX.stop_pending = true;
        snprintf(CTX.stop_reason, sizeof(CTX.stop_reason), "reached $%04X", CTX.until_addr);
        CTX.last_stop_pc = cur_pc();
        CTX.last_stop_valid = true;
        return true;
    }
    return false;
}

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

/* Executes one instruction. Returns false if a stop condition was hit (before or after exec). */
static bool run_one(void)
{
    if (CTX.stop_pending) return false;
    if (bp_precheck() || until_precheck()) return false;

    CTX.cycles_raw += GB_run(CTX.gb);
    CTX.instr_count++;

    /* attach captured memory accesses to the most recent trace entry */
    if (CTX.trace.active && CTX.trace.with_mem && CTX.trace.count) {
        uint32_t last = (CTX.trace.head + CTX.trace.cap - 1) % CTX.trace.cap;
        memcpy(CTX.trace.ring[last].mems, CTX.mem_cap, sizeof(CTX.mem_cap));
        CTX.trace.ring[last].n_mems = CTX.mem_cap_n;
    }

    if (CTX.stop_pending) {
        CTX.last_stop_pc = cur_pc();
        CTX.last_stop_valid = true;
        return false;
    }
    if (CTX.until_expr_armed) {
        uint16_t r, b;
        if (!GB_debugger_evaluate(CTX.gb, CTX.until_expr, &r, &b) && r) {
            CTX.stop_pending = true;
            snprintf(CTX.stop_reason, sizeof(CTX.stop_reason), "expression true: %s", CTX.until_expr);
            CTX.last_stop_pc = cur_pc();
            CTX.last_stop_valid = true;
            return false;
        }
    }
    return true;
}

uint64_t exec_run_instructions(uint64_t n)
{
    install_analysis_hooks();
    uint64_t ran = 0;
    while (ran < n) {
        if (!run_one()) break;
        ran++;
    }
    return ran;
}

uint64_t exec_run_frames(uint64_t n)
{
    install_analysis_hooks();
    uint64_t frames = 0;
    while (frames < n && !CTX.stop_pending) {
        vblank_seen = false;
        GB_set_vblank_callback(CTX.gb, vblank_cb);
        while (!vblank_seen) {
            if (!run_one()) goto out;
        }
        frames++;
        CTX.frame_count++;
        /* advance input script after each completed frame */
        if (CTX.script_pos < CTX.script_len) {
            input_step_t *st = &CTX.script[CTX.script_pos];
            if (--st->frames_left <= 0) CTX.script_pos++;
            if (CTX.script_pos < CTX.script_len)
                GB_set_key_mask(CTX.gb, CTX.script[CTX.script_pos].mask);
        }
    }
out:
    GB_set_vblank_callback(CTX.gb, NULL);
    return frames;
}

/* emit standard "stopped" info after any run command;
   includes frames_total when unit is frames */
static void emit_stop_result(uint64_t requested, uint64_t ran, const char *unit)
{
    (void)requested;
    jw_t *w = &CTX.out;
    GB_registers_t *r = GB_get_registers(CTX.gb);
    jw_fmt(w, "{\"ran_%s\":%llu,", unit, (unsigned long long)ran);
    jw_fmt(w, "\"pc\":\"%04x\",\"sp\":\"%04x\",\"a\":%u,", r->pc, r->sp, (unsigned)(r->af >> 8));
    jw_fmt(w, "\"instructions_total\":%llu,", (unsigned long long)CTX.instr_count);
    bool is_frames = strcmp(unit, "frames") == 0;
    if (is_frames) jw_fmt(w, "\"frames_total\":%llu,", (unsigned long long)CTX.frame_count);
    jw_fmt(w, "\"cycles8_total\":%llu,", (unsigned long long)CTX.cycles_raw);
    if (CTX.stop_pending) {
        jw_fmt(w, "\"stopped\":true,\"reason\":");
        jw_esc(w, CTX.stop_reason);
        /* hit-reason reporting: which predicate fired + live bank state */
        if (CTX.wp_fired) { jw_raw(w, ",\"detail\":"); jw_esc(w, CTX.wp_fired_msg); }
        else if (CTX.bp_fired) { jw_raw(w, ",\"detail\":"); jw_esc(w, CTX.bp_fired_msg); }
        jw_raw(w, ",");
        banks_json(w);
    }
    else jw_fmt(w, "\"stopped\":false");
    jw_raw(w, "}");
}

/* ---------------- commands: run control ---------------- */

bool cmd_step(const jval_t *p)
{
    uint64_t n = (uint64_t)j_int(p, "n", 1);
    if (n > 1000000000ULL) { set_err("n too large"); return false; }
    CTX.stop_pending = CTX.wp_fired = CTX.bp_fired = false;
    uint64_t ran = exec_run_instructions(n);
    emit_stop_result(n, ran, "instructions");
    return true;
}

bool cmd_run_frames(const jval_t *p)
{
    uint64_t n = (uint64_t)j_int(p, "n", 1);
    if (n > 1000000ULL) { set_err("n too large"); return false; }
    CTX.stop_pending = CTX.wp_fired = CTX.bp_fired = false;
    uint64_t ran = exec_run_frames(n);
    emit_stop_result(n, ran, "frames");
    return true;
}

bool cmd_run_until(const jval_t *p)
{
    CTX.stop_pending = CTX.wp_fired = CTX.bp_fired = false;
    CTX.until_addr_armed = CTX.until_expr_armed = false;

    if (j_has(p, "addr")) {
        uint16_t a;
        if (!parse_u16(j_get(p, "addr"), &a)) { set_err("bad 'addr'"); return false; }
        CTX.until_addr = a;
        CTX.until_addr_armed = true;
    }
    else if (j_str(p, "expr", NULL)) {
        snprintf(CTX.until_expr, sizeof(CTX.until_expr), "%s", j_str(p, "expr", ""));
        CTX.until_expr_armed = true;
    }
    else {
        set_err("need 'addr' or 'expr'");
        return false;
    }
    uint64_t max = (uint64_t)j_int(p, "max_instructions", 10000000);
    long long deadline = now_ms() + j_int(p, "timeout_ms", 2 * 60 * 1000L);

    install_analysis_hooks();
    uint64_t ran = 0;
    bool timed_out = false;
    while (ran < max && !CTX.stop_pending) {
        if (!run_one()) break;
        ran++;
        if ((ran & 0xFFF) == 0 && now_ms() > deadline) { timed_out = true; break; }
    }
    CTX.until_addr_armed = CTX.until_expr_armed = false;
    if (timed_out && !CTX.stop_pending) {
        CTX.stop_pending = true;
        snprintf(CTX.stop_reason, sizeof(CTX.stop_reason), "timeout");
    }
    emit_stop_result(max, ran, "instructions");
    return true;
}

/* ---------------- commands: breakpoints ---------------- */

bool cmd_break_add(const jval_t *p)
{
    uint16_t addr;
    if (!parse_u16(j_get(p, "addr"), &addr)) { set_err("bad 'addr'"); return false; }
    const char *type = j_str(p, "type", "pc");
    int kind = strcmp(type, "pc") == 0 ? BP_PC : (strcmp(type, "mem") == 0 ? BP_MEM : -1);
    if (kind < 0) { set_err("bad 'type' (pc|mem)"); return false; }
    int slot = -1;
    for (int i = 0; i < MAX_BREAKPOINTS; i++) if (!CTX.bps[i].used) { slot = i; break; }
    if (slot < 0) { set_err("max breakpoints reached"); return false; }
    bp_t *bp = &CTX.bps[slot];
    memset(bp, 0, sizeof(*bp));
    bp->id = CTX.next_bp_id++;
    bp->used = bp->enabled = true;
    bp->kind = kind;
    bp->addr = addr;
    bp->bank = j_has(p, "bank") ? (int)j_int(p, "bank", -1) : -1;
    bp->stop = j_bool(p, "stop", true);
    free(bp->cond);
    const char *cond = j_str(p, "cond", NULL);
    bp->cond = cond ? strdup(cond) : NULL;

    if (kind == BP_MEM) {
        const char *acc = j_str(p, "access", "w");
        bp->on_read = strchr(acc, 'r') != NULL;
        bp->on_write = strchr(acc, 'w') != NULL;
        if (!bp->on_read && !bp->on_write) { set_err("access must contain r and/or w"); bp->used = false; return false; }
        bp->end = addr;
        if (j_has(p, "end") && !parse_u16(j_get(p, "end"), &bp->end)) { set_err("bad 'end'"); bp->used = false; return false; }
        if (bp->end < bp->addr) { set_err("end < addr"); bp->used = false; return false; }
        bp->mask = 0xFF;
        if (j_has(p, "mask") && !parse_u16(j_get(p, "mask"), &bp->mask)) { set_err("bad 'mask'"); bp->used = false; return false; }
        if (j_has(p, "value")) {
            if (!parse_u16(j_get(p, "value"), &bp->value)) { set_err("bad 'value'"); bp->used = false; return false; }
            bp->value_given = true;
            if (bp->value > 0xFF) { set_err("'value' must be 0..255"); bp->used = false; return false; }
        }
        if (j_has(p, "pc")) {
            if (!parse_u16(j_get(p, "pc"), &bp->pc_eq)) { set_err("bad 'pc'"); bp->used = false; return false; }
            bp->pc_eq_given = true;
        }
        if (j_has(p, "pc_not") && !parse_u16(j_get(p, "pc_not"), &bp->pc_not)) { set_err("bad 'pc_not'"); bp->used = false; return false; }
        bp->pc_not_given = j_has(p, "pc_not");
        if (bp->pc_eq_given && bp->pc_not_given) { set_err("'pc' and 'pc_not' are mutually exclusive"); bp->used = false; return false; }
        if (j_has(p, "pc_lo") && j_has(p, "pc_hi")) {
            if (!parse_u16(j_get(p, "pc_lo"), &bp->pc_lo) || !parse_u16(j_get(p, "pc_hi"), &bp->pc_hi))
                { set_err("bad 'pc_lo'/'pc_hi'"); bp->used = false; return false; }
            if (bp->pc_hi < bp->pc_lo) { set_err("pc_hi < pc_lo"); bp->used = false; return false; }
            bp->pc_range_given = true;
        }
        /* mem predicates need the read/write callbacks armed */
        install_analysis_hooks();
    }
    bp->hits = 0;
    jw_fmt(&CTX.out, "{\"id\":%d,\"addr\":\"%04x\",\"kind\":\"%s\"}", bp->id, addr,
           kind == BP_MEM ? "mem" : "pc");
    return true;
}

bool cmd_break_list(const jval_t *p)
{
    (void)p;
    jw_t *w = &CTX.out;
    jw_raw(w, "{\"breakpoints\":[");
    bool first = true;
    for (int i = 0; i < MAX_BREAKPOINTS; i++) {
        bp_t *bp = &CTX.bps[i];
        if (!bp->used) continue;
        jw_fmt(w, "%s{\"id\":%d,\"kind\":\"%s\",\"enabled\":%s,\"addr\":\"%04x\",\"bank\":%d,\"hits\":%llu,\"cond\":",
               first ? "" : ",", bp->id, bp->kind == BP_MEM ? "mem" : "pc",
               bp->enabled ? "true" : "false",
               bp->addr, bp->bank, (unsigned long long)bp->hits);
        if (bp->cond) jw_esc(w, bp->cond); else jw_raw(w, "null");
        if (bp->kind == BP_MEM) {
            jw_fmt(w, ",\"end\":\"%04x\",\"read\":%s,\"write\":%s,\"stop\":%s",
                   bp->end, bp->on_read ? "true" : "false",
                   bp->on_write ? "true" : "false", bp->stop ? "true" : "false");
            if (bp->value_given) jw_fmt(w, ",\"mask\":\"%02x\",\"value\":\"%02x\"", bp->mask, bp->value);
            if (bp->pc_eq_given) jw_fmt(w, ",\"pc\":\"%04x\"", bp->pc_eq);
            if (bp->pc_not_given) jw_fmt(w, ",\"pc_not\":\"%04x\"", bp->pc_not);
            if (bp->pc_range_given) jw_fmt(w, ",\"pc_lo\":\"%04x\",\"pc_hi\":\"%04x\"", bp->pc_lo, bp->pc_hi);
        }
        jw_raw(w, "}");
        first = false;
    }
    jw_raw(w, "]}");
    return true;
}

/* ---------------- breakpoint hit log ---------------- */

bool cmd_break_log(const jval_t *p)
{
    jw_t *w = &CTX.out;
    long limit = j_int(p, "limit", 1000);
    if (limit > (long)BP_HITLOG_CAP) limit = BP_HITLOG_CAP;
    bool clear = j_bool(p, "clear", false);
    long n = CTX.bp_hits_count < (uint32_t)limit ? (long)CTX.bp_hits_count : limit;
    jw_fmt(w, "{\"total\":%llu,\"ring\":%u,\"entries\":[",
           (unsigned long long)CTX.bp_hits_total, (unsigned)CTX.bp_hits_count);
    /* oldest last: start from head-count */
    for (long i = n - 1; i >= 0; i--) {
        uint32_t idx = (CTX.bp_hits_head + BP_HITLOG_CAP - (uint32_t)n + (uint32_t)i) % BP_HITLOG_CAP;
        bp_hit_t *h = &CTX.bp_hits[idx];
        jw_fmt(w, "%s{\"bp\":%u,\"kind\":\"%c\",\"addr\":\"%04x\",\"v\":\"%02x\",\"bank\":%u,\"pc\":\"%04x\"}",
               (i == n - 1) ? "" : ",", h->bp, h->is_write ? 'w' : 'r',
               h->addr, h->value, h->effbank, h->pc);
    }
    jw_raw(w, "]}");
    if (clear) { CTX.bp_hits_head = CTX.bp_hits_count = 0; }
    return true;
}

bool cmd_break_del(const jval_t *p)
{
    long id = j_int(p, "id", -1);
    for (int i = 0; i < MAX_BREAKPOINTS; i++) {
        if (CTX.bps[i].used && CTX.bps[i].id == id) {
            bool was_mem = CTX.bps[i].kind == BP_MEM;
            free(CTX.bps[i].cond);
            CTX.bps[i] = (bp_t){0};
            if (was_mem) install_analysis_hooks();
            jw_raw(&CTX.out, "{\"deleted\":true}");
            return true;
        }
    }
    set_err("no breakpoint with id %ld", id);
    return false;
}

bool cmd_break_clear(const jval_t *p)
{
    (void)p;
    bool had_mem = false;
    for (int i = 0; i < MAX_BREAKPOINTS; i++) {
        if (CTX.bps[i].kind == BP_MEM) had_mem = true;
        free(CTX.bps[i].cond); CTX.bps[i] = (bp_t){0};
    }
    if (had_mem) install_analysis_hooks();
    jw_raw(&CTX.out, "{\"cleared\":true}");
    return true;
}

/* ---------------- commands: watchpoints ---------------- */

bool cmd_watch_add(const jval_t *p)
{
    uint16_t addr, end;
    if (!parse_u16(j_get(p, "addr"), &addr)) { set_err("bad 'addr'"); return false; }
    end = addr;
    if (j_has(p, "end") && !parse_u16(j_get(p, "end"), &end)) { set_err("bad 'end'"); return false; }
    if (end < addr) { set_err("end < addr"); return false; }
    const char *acc = j_str(p, "access", "w");
    int slot = -1;
    for (int i = 0; i < MAX_WATCHPOINTS; i++) if (!CTX.wps[i].used) { slot = i; break; }
    if (slot < 0) { set_err("max watchpoints reached"); return false; }
    wp_t *wp = &CTX.wps[slot];
    wp->id = CTX.next_wp_id++;
    wp->used = wp->enabled = true;
    wp->addr = addr; wp->end = end;
    wp->on_read = strchr(acc, 'r') != NULL;
    wp->on_write = strchr(acc, 'w') != NULL;
    if (!wp->on_read && !wp->on_write) { set_err("access must contain r and/or w"); wp->used = false; return false; }
    wp->bank = j_has(p, "bank") ? (int)j_int(p, "bank", -1) : -1;
    wp->stop = j_bool(p, "stop", true);
    wp->value = j_has(p, "value") ? (int)(j_int(p, "value", 0) & 0xFF) : -1;
    free(wp->cond);
    const char *cond = j_str(p, "cond", NULL);
    wp->cond = cond ? strdup(cond) : NULL;
    wp->hits = 0;
    install_analysis_hooks();
    jw_fmt(&CTX.out, "{\"id\":%d,\"addr\":\"%04x\",\"end\":\"%04x\"}", wp->id, addr, end);
    return true;
}

bool cmd_watch_list(const jval_t *p)
{
    (void)p;
    jw_t *w = &CTX.out;
    jw_raw(w, "{\"watchpoints\":[");
    bool first = true;
    for (int i = 0; i < MAX_WATCHPOINTS; i++) {
        wp_t *wp = &CTX.wps[i];
        if (!wp->used) continue;
        jw_fmt(w, "%s{\"id\":%d,\"enabled\":%s,\"addr\":\"%04x\",\"end\":\"%04x\",\"read\":%s,\"write\":%s,\"bank\":%d,\"stop\":%s,\"value\":%d,\"hits\":%llu,\"cond\":",
               first ? "" : ",", wp->id, wp->enabled ? "true" : "false",
               wp->addr, wp->end, wp->on_read ? "true" : "false", wp->on_write ? "true" : "false",
               wp->bank, wp->stop ? "true" : "false", wp->value, (unsigned long long)wp->hits);
        if (wp->cond) jw_esc(w, wp->cond); else jw_raw(w, "null");
        jw_raw(w, "}");
        first = false;
    }
    jw_raw(w, "]}");
    return true;
}

bool cmd_watch_del(const jval_t *p)
{
    long id = j_int(p, "id", -1);
    for (int i = 0; i < MAX_WATCHPOINTS; i++) {
        if (CTX.wps[i].used && CTX.wps[i].id == id) {
            free(CTX.wps[i].cond);
            CTX.wps[i] = (wp_t){0};
            install_analysis_hooks();
            jw_raw(&CTX.out, "{\"deleted\":true}");
            return true;
        }
    }
    set_err("no watchpoint with id %ld", id);
    return false;
}

bool cmd_watch_clear(const jval_t *p)
{
    (void)p;
    for (int i = 0; i < MAX_WATCHPOINTS; i++) { free(CTX.wps[i].cond); CTX.wps[i] = (wp_t){0}; }
    install_analysis_hooks();
    jw_raw(&CTX.out, "{\"cleared\":true}");
    return true;
}

/* ---------------- commands: trace ---------------- */

bool cmd_trace_start(const jval_t *p)
{
    trace_t *t = &CTX.trace;
    long cap = j_int(p, "max", (long)t->cap);
    if (cap < 16) cap = 16;
    if (cap > (1 << 22)) { set_err("max too large (cap 4M entries)"); return false; }
    if ((uint32_t)cap != t->cap) {
        free(t->ring);
        t->ring = calloc((size_t)cap, sizeof(trace_entry_t));
        if (!t->ring) { t->cap = 0; set_err("out of memory"); return false; }
        t->cap = (uint32_t)cap;
    }
    t->head = t->count = 0;
    t->total = t->dropped = 0;
    t->with_regs = j_bool(p, "with_regs", false);
    t->with_mem = j_bool(p, "with_mem", false);
    t->rom_only = j_bool(p, "rom_only", false);
    t->pc_filter = false;
    if (j_has(p, "pc_lo") && j_has(p, "pc_hi")) {
        parse_u16(j_get(p, "pc_lo"), &t->pc_lo);
        parse_u16(j_get(p, "pc_hi"), &t->pc_hi);
        t->pc_filter = true;
    }
    t->active = true;
    install_analysis_hooks();
    jw_fmt(&CTX.out, "{\"tracing\":true,\"capacity\":%u}", t->cap);
    return true;
}

bool cmd_trace_stop(const jval_t *p)
{
    (void)p;
    CTX.trace.active = false;
    install_analysis_hooks();
    jw_fmt(&CTX.out, "{\"tracing\":false,\"total\":%llu,\"dropped\":%llu}",
           (unsigned long long)CTX.trace.total, (unsigned long long)CTX.trace.dropped);
    return true;
}

bool cmd_trace_dump(const jval_t *p)
{
    trace_t *t = &CTX.trace;
    long limit = j_int(p, "limit", 1000);
    if (limit > 100000) limit = 100000;
    jw_t *w = &CTX.out;

    jw_fmt(w, "{\"total\":%llu,\"dropped\":%llu,\"entries\":[",
           (unsigned long long)t->total, (unsigned long long)t->dropped);
    long n = t->count < limit ? t->count : limit;
    /* newest last */
    for (long i = 0; i < n; i++) {
        uint32_t idx = (t->head + t->cap - t->count + (uint32_t)i) % t->cap;
        trace_entry_t *e = &t->ring[idx];
        jw_fmt(w, "%s{\"pc\":\"%04x\",\"bank\":%d,\"op\":\"%02x\"",
               i ? "," : "", e->pc, e->bank, e->opcode);
        if (e->has_regs) {
            jw_fmt(w, ",\"regs\":\"%02x%02x %02x%02x %02x%02x %02x%02x\"",
                   e->regs[0], e->regs[1], e->regs[2], e->regs[3],
                   e->regs[4], e->regs[5], e->regs[6], e->regs[7]);
        }
        if (e->n_mems) {
            jw_raw(w, ",\"mem\":[");
            for (int m = 0; m < e->n_mems; m++) {
                jw_fmt(w, "%s[\"%c\",\"%02x%02x\",\"%02x\"]", m ? "," : "",
                       e->mems[m][0] ? 'w' : 'r', e->mems[m][1], e->mems[m][2], e->mems[m][3]);
            }
            jw_raw(w, "]");
        }
        const char *nm = GB_debugger_name_for_address(CTX.gb, e->pc);
        if (nm) { jw_raw(w, ",\"symbol\":"); jw_esc(w, nm); }
        jw_raw(w, "}");
    }
    jw_raw(w, "]}");
    return true;
}

/* ---------------- commands: coverage ---------------- */

bool cmd_coverage_get(const jval_t *p)
{
    jw_t *w = &CTX.out;
    bool arm = j_bool(p, "arm", false);
    if (arm && !CTX.cov_bits && CTX.loaded) {
        CTX.cov_rom_size = CTX.gb->rom_size > (8 << 20) ? (8 << 20) : CTX.gb->rom_size;
        CTX.cov_bits = calloc(CTX.cov_rom_size / 8 + 1, 1);
        CTX.cov_active = true;
        install_analysis_hooks();
    }
    if (!CTX.cov_bits) { jw_raw(w, "{\"armed\":false}"); return true; }

    size_t size = CTX.cov_rom_size;
    size_t banks = size / 0x4000;
    long max_ranges = j_int(p, "max_ranges", 4096);
    jw_raw(w, "{\"armed\":true");
    jw_fmt(w, ",\"instructions\":%llu", (unsigned long long)CTX.cov_executed);
    jw_fmt(w, ",\"rom_bytes\":%u,\"banks\":%u", (unsigned)CTX.gb->rom_size, (unsigned)(CTX.gb->rom_size / 0x4000));
    jw_raw(w, ",\"per_bank\":[");
    long ranges_emitted = 0;
    bool truncated = false;
    for (size_t b = 0; b < banks && !truncated; b++) {
        size_t executed = 0;
        for (size_t i = 0; i < 0x4000; i++) {
            size_t lin = b * 0x4000 + i;
            if (CTX.cov_bits[lin >> 3] & (1 << (lin & 7))) executed++;
        }
        jw_fmt(w, "%s{\"bank\":%zu,\"executed\":%zu,\"pct\":%.1f",
               b ? "," : "", b, executed, executed * 100.0 / 0x4000);
        if (j_bool(p, "ranges", false) && executed) {
            jw_raw(w, ",\"ranges\":[");
            bool in_range = false;
            size_t start = 0;
            bool first_r = true;
            for (size_t i = 0; i <= 0x4000; i++) {
                size_t lin = b * 0x4000 + i;
                bool set = i < 0x4000 && (CTX.cov_bits[lin >> 3] & (1 << (lin & 7)));
                if (set && !in_range) { in_range = true; start = i; }
                else if (!set && in_range) {
                    in_range = false;
                    if (ranges_emitted >= max_ranges) { truncated = true; jw_raw(w, ",\"...\":"); jw_esc(w, "truncated"); break; }
                    jw_fmt(w, "%s[\"%04zx\",\"%04zx\"]", first_r ? "" : ",", start, i - 1);
                    ranges_emitted++;
                    first_r = false;
                }
            }
            jw_raw(w, "]");
            if (truncated) jw_fmt(w, ",\"truncated\":true");
        }
        jw_raw(w, "}");
    }
    jw_raw(w, "]}");
    return true;
}

bool cmd_coverage_reset(const jval_t *p)
{
    (void)p;
    bool arm = true;
    if (CTX.cov_bits) memset(CTX.cov_bits, 0, CTX.cov_rom_size / 8 + 1);
    else if (CTX.loaded) {
        CTX.cov_rom_size = CTX.gb->rom_size > (8 << 20) ? (8 << 20) : CTX.gb->rom_size;
        CTX.cov_bits = calloc(CTX.cov_rom_size / 8 + 1, 1);
    }
    CTX.cov_executed = 0;
    CTX.cov_active = arm && CTX.cov_bits != NULL;
    install_analysis_hooks();
    jw_fmt(&CTX.out, "{\"armed\":%s}", CTX.cov_active ? "true" : "false");
    return true;
}

/* ---------------- commands: backtrace ---------------- */

bool cmd_backtrace(const jval_t *p)
{
    (void)p;
    jw_t *w = &CTX.out;
    unsigned n = ext_backtrace_size(CTX.gb);
    jw_fmt(w, "{\"call_depth\":%d,\"stack_sp\":\"%04x\",\"entries\":[",
           ext_call_depth(CTX.gb), GB_get_registers(CTX.gb)->sp);
    for (unsigned i = 0; i < n; i++) {
        uint16_t addr, bank, sp;
        ext_backtrace_entry(CTX.gb, i, &addr, &bank, &sp);
        jw_fmt(w, "%s{\"addr\":\"%04x\",\"bank\":%u,\"sp\":\"%04x\"",
               i ? "," : "", addr, bank, sp);
        const char *nm = GB_debugger_name_for_address(CTX.gb, addr);
        if (nm) { jw_raw(w, ",\"symbol\":"); jw_esc(w, nm); }
        jw_raw(w, "}");
    }
    jw_raw(w, "]}");
    return true;
}
