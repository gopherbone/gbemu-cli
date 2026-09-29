/* sanitize.c — runtime memory-safety sanitizer for patched Game Boy code.
 *
 * Flags, at the exact instruction, the hardware-level failure classes that
 * show up as "text corruption" or "random crashes" in ROM hacks:
 *
 *   vram_write_blocked   CPU write to VRAM while the PPU owns it (mode 3):
 *                        real hardware (and SameBoy) DROP the write
 *   vram_read_blocked    CPU read of VRAM in mode 3: returns $FF
 *   oam_write_blocked    OAM write in mode 2/3 (dropped + OAM bug on DMG)
 *   pal_write_blocked    CGB palette data write ($FF69/$FF6B) in mode 3
 *   mbc_stray            write to $6000-$7FFF (no MBC5 register), $3000-$3FFF
 *                        with a nonzero value (bank bit 8), a ROM bank number
 *                        beyond the cart, or an SRAM bank beyond the cart
 *   echo_write           write to echo RAM $E000-$FDFF or $FEA0-$FEFF
 *   sram_disabled        cart-RAM access while RAM is disabled
 *   exec_ram             instruction fetch outside ROM (allowlist: HRAM)
 *   illegal_opcode       $D3/$DB/$DD/$E3/$E4/$EB/$EC/$ED/$F4/$FC/$FD (lock-up)
 *   ret_mismatch         ret/reti resumes somewhere other than what the
 *                        matching call/rst/interrupt pushed (return address
 *                        overwritten on the stack = stack corruption)
 *   sp_range             SP left the configured [sp_lo, sp_hi] window
 *   irq_selfid           (opt-in, games using the "[$4000] = own bank number"
 *                        self-ID idiom) an interrupt was dispatched while the
 *                        mapped ROMX bank's first byte is not its own number:
 *                        an ISR that saves/restores via [$4000] resumes the
 *                        interrupted code in the WRONG bank
 *   map_selfid_ime       (opt-in, same idiom) a self-ID-broken bank was mapped
 *                        with IME=1 (the latent window irq_selfid realizes)
 *
 * Events aggregate per (class, pc, bank) site; the first N raw events are kept
 * in order.  Pure observation: the sanitizer never alters emulation.
 */
#include <string.h>
#include <stdlib.h>
#include <Core/gb.h>
#include <Core/display.h>
#include <Core/memory.h>
#include "ctx.h"
#include "agent_ext.h"

enum {
    SC_VRAM_W, SC_VRAM_R, SC_OAM_W, SC_PAL_W, SC_MBC, SC_ECHO, SC_SRAM,
    SC_EXEC_RAM, SC_ILLEGAL, SC_RET, SC_SP, SC_IRQ_SELFID, SC_MAP_SELFID,
    SC_N
};
static const char *SC_NAME[SC_N] = {
    "vram_write_blocked", "vram_read_blocked", "oam_write_blocked",
    "pal_write_blocked", "mbc_stray", "echo_write", "sram_disabled",
    "exec_ram", "illegal_opcode", "ret_mismatch", "sp_range", "irq_selfid",
    "map_selfid_ime",
};

#define SITE_CAP 8192
#define EVENT_CAP 4096
#define SHADOW_CAP 512
#define MAX_ALLOW 16

typedef struct {
    bool used;
    uint8_t cls;
    uint16_t pc, bank;
    uint64_t count;
    uint64_t first_frame, first_instr;
    uint16_t addr;      /* first access addr (or expected ret addr) */
    uint16_t value;     /* first value (or actual resume pc) */
    uint8_t ly, mode;
    uint16_t aux;       /* class-specific (interrupted pc, selected bank...) */
} san_site_t;

typedef struct {
    uint8_t cls;
    uint16_t pc, bank, addr, value, aux;
    uint8_t ly, mode;
    uint64_t frame, instr;
} san_event_t;

typedef struct { uint16_t sp, ret, from_pc, from_bank; uint8_t kind; } shadow_t;

static struct {
    bool active;
    bool enabled[SC_N];
    bool stop_on[SC_N];
    bool freeze;            /* sticky stop: once a stop_on event fires, every
                               later run command refuses to execute */
    bool frozen;
    char frozen_reason[160];
    char frozen_msg[256];
    uint64_t counts[SC_N];
    san_site_t *sites;
    uint32_t n_sites;
    uint64_t sites_dropped;
    san_event_t *events;
    uint32_t n_events;
    uint64_t events_total;
    /* exec allowlist beyond ROM */
    uint16_t allow_lo[MAX_ALLOW], allow_hi[MAX_ALLOW];
    unsigned n_allow;
    /* sp window */
    bool sp_given; uint16_t sp_lo, sp_hi; bool sp_out;
    /* self-ID idiom */
    uint8_t *selfid_bad;   /* per bank flag */
    unsigned n_banks;
    /* shadow call stack */
    shadow_t shadow[SHADOW_CAP];
    unsigned n_shadow;
    uint64_t rets_unmatched, shadow_overflow;
    /* previous instruction */
    bool prev_valid;
    uint16_t prev_pc, prev_sp, prev_bank;
    uint8_t prev_op;
    uint8_t cur_op;
} S;

bool sanitize_active(void) { return S.active; }

/* run_one() gate: a frozen session reports the original stop again and
   executes nothing (the harness cannot silently resume past the fault). */
bool sanitize_frozen_gate(void)
{
    if (!S.frozen) return false;
    if (!CTX.stop_pending) {
        CTX.stop_pending = true;
        CTX.bp_fired = true;
        snprintf(CTX.stop_reason, sizeof(CTX.stop_reason), "%s", S.frozen_reason);
        snprintf(CTX.bp_fired_msg, sizeof(CTX.bp_fired_msg), "%s", S.frozen_msg);
    }
    return true;
}

static uint8_t stat_mode(void) { return CTX.gb->io_registers[GB_IO_STAT] & 3; }
static uint8_t cur_ly(void) { return CTX.gb->io_registers[GB_IO_LY]; }

static uint16_t cur_bank_for_pc(uint16_t pc)
{
    int b = ext_effective_rom_bank(CTX.gb, pc);
    return b < 0 ? 0xFFFF : (uint16_t)b;
}

static void san_event(int cls, uint16_t pc, uint16_t addr, uint16_t value, uint16_t aux)
{
    if (!S.enabled[cls]) return;
    S.counts[cls]++;
    S.events_total++;
    uint16_t bank = cur_bank_for_pc(pc);
    uint8_t ly = cur_ly(), mode = stat_mode();
    if (S.n_events < EVENT_CAP) {
        san_event_t *e = &S.events[S.n_events++];
        e->cls = (uint8_t)cls; e->pc = pc; e->bank = bank; e->addr = addr;
        e->value = value; e->aux = aux; e->ly = ly; e->mode = mode;
        e->frame = CTX.frame_count; e->instr = CTX.instr_count;
    }
    /* site aggregation: open addressing on (cls, pc, bank) */
    uint32_t h = ((uint32_t)cls * 2654435761u) ^ ((uint32_t)pc * 40503u) ^ ((uint32_t)bank << 17);
    for (uint32_t i = 0; i < SITE_CAP; i++) {
        san_site_t *s = &S.sites[(h + i) % SITE_CAP];
        if (!s->used) {
            if (S.n_sites >= SITE_CAP * 3 / 4) { S.sites_dropped++; break; }
            s->used = true; s->cls = (uint8_t)cls; s->pc = pc; s->bank = bank;
            s->count = 1; s->first_frame = CTX.frame_count; s->first_instr = CTX.instr_count;
            s->addr = addr; s->value = value; s->ly = ly; s->mode = mode; s->aux = aux;
            S.n_sites++;
            break;
        }
        if (s->cls == cls && s->pc == pc && s->bank == bank) { s->count++; break; }
    }
    if (S.stop_on[cls] && !CTX.stop_pending && !S.frozen) {
        CTX.stop_pending = true;
        CTX.bp_fired = true;
        snprintf(CTX.stop_reason, sizeof(CTX.stop_reason), "sanitizer %s at $%04X", SC_NAME[cls], pc);
        snprintf(CTX.bp_fired_msg, sizeof(CTX.bp_fired_msg),
                 "sanitizer %s: pc=$%04X bank=%u addr=$%04X value=$%04X aux=$%04X ly=%u mode=%u frame=%llu instr=%llu",
                 SC_NAME[cls], pc, bank, addr, value, aux, ly, mode,
                 (unsigned long long)CTX.frame_count, (unsigned long long)CTX.instr_count);
        if (S.freeze) {
            S.frozen = true;
            snprintf(S.frozen_reason, sizeof(S.frozen_reason), "%s (frozen)", CTX.stop_reason);
            snprintf(S.frozen_msg, sizeof(S.frozen_msg), "%s", CTX.bp_fired_msg);
        }
    }
}

/* ---------------- memory hooks (called from exec.c mem callbacks) -------- */

void sanitize_on_write(uint16_t addr, uint8_t value)
{
    if (!S.active || CTX.probing) return;
    GB_gameboy_t *gb = CTX.gb;
    uint16_t pc = CTX.cur_instr_pc;
    if (addr < 0x8000) {
        if (addr >= 0x6000) {
            san_event(SC_MBC, pc, addr, value, 0);
        }
        else if (addr >= 0x4000) {
            unsigned nram = gb->mbc_ram_size / 0x2000;
            if (nram && (value & 0x0F) >= nram) san_event(SC_MBC, pc, addr, value, nram);
        }
        else if (addr >= 0x3000) {
            if (value) san_event(SC_MBC, pc, addr, value, 0);
        }
        else if (addr >= 0x2000) {
            unsigned bank = value | (gb->mbc5.rom_bank_high << 8);
            if (bank >= S.n_banks) san_event(SC_MBC, pc, addr, value, (uint16_t)bank);
            else if (S.selfid_bad && S.selfid_bad[bank] && gb->ime)
                san_event(SC_MAP_SELFID, pc, addr, value, (uint16_t)bank);
        }
        return;
    }
    if (addr < 0xA000) {
        if (gb->io_registers[GB_IO_LCDC] & 0x80) {
            GB_display_sync(gb);
            if (gb->vram_write_blocked) san_event(SC_VRAM_W, pc, addr, value, 0);
        }
        return;
    }
    if (addr < 0xC000) {
        if (!gb->mbc_ram_enable) san_event(SC_SRAM, pc, addr, value, 1);
        return;
    }
    if (addr >= 0xE000 && addr < 0xFE00) { san_event(SC_ECHO, pc, addr, value, 0); return; }
    if (addr >= 0xFE00 && addr < 0xFEA0) {
        GB_display_sync(gb);
        if (gb->oam_write_blocked) san_event(SC_OAM_W, pc, addr, value, 0);
        return;
    }
    if (addr >= 0xFEA0 && addr < 0xFF00) { san_event(SC_ECHO, pc, addr, value, 1); return; }
    if ((addr == 0xFF69 || addr == 0xFF6B) && GB_is_cgb(gb)) {
        GB_display_sync(gb);
        if (gb->cgb_palettes_blocked) san_event(SC_PAL_W, pc, addr, value, 0);
    }
}

void sanitize_on_read(uint16_t addr, uint8_t value)
{
    if (!S.active || CTX.probing) return;
    GB_gameboy_t *gb = CTX.gb;
    if (addr >= 0x8000 && addr < 0xA000) {
        if (gb->io_registers[GB_IO_LCDC] & 0x80) {
            GB_display_sync(gb);
            if (gb->vram_read_blocked && !gb->in_dma_read)
                san_event(SC_VRAM_R, CTX.cur_instr_pc, addr, value, 0);
        }
    }
    else if (addr >= 0xA000 && addr < 0xC000) {
        if (!gb->mbc_ram_enable) san_event(SC_SRAM, CTX.cur_instr_pc, addr, value, 0);
    }
}

/* ---------------- execution hook ---------------- */

static uint16_t rd16(uint16_t a)
{
    bool was = CTX.probing;
    CTX.probing = true;
    uint16_t v = GB_safe_read_memory(CTX.gb, a) | (GB_safe_read_memory(CTX.gb, (uint16_t)(a + 1)) << 8);
    CTX.probing = was;
    return v;
}

static bool is_call_op(uint8_t op)
{
    return op == 0xCD || op == 0xC4 || op == 0xCC || op == 0xD4 || op == 0xDC || (op & 0xC7) == 0xC7;
}
static bool is_ret_op(uint8_t op)
{
    return op == 0xC9 || op == 0xD9 || op == 0xC0 || op == 0xC8 || op == 0xD0 || op == 0xD8;
}
static bool is_xfer_op(uint8_t op)
{
    return is_call_op(op) || is_ret_op(op) || op == 0xC3 || op == 0xC2 || op == 0xCA ||
           op == 0xD2 || op == 0xDA || op == 0xE9 || op == 0x18 || op == 0x20 ||
           op == 0x28 || op == 0x30 || op == 0x38;
}

static void shadow_push(uint16_t sp, uint16_t ret, uint16_t from_pc, uint16_t from_bank, uint8_t kind)
{
    /* entries at or below this sp are dead frames (overwritten now) */
    while (S.n_shadow && S.shadow[S.n_shadow - 1].sp <= sp) S.n_shadow--;
    if (S.n_shadow >= SHADOW_CAP) {
        memmove(S.shadow, S.shadow + 1, sizeof(shadow_t) * (SHADOW_CAP - 1));
        S.n_shadow--;
        S.shadow_overflow++;
    }
    S.shadow[S.n_shadow++] = (shadow_t){sp, ret, from_pc, from_bank, kind};
}

static const uint8_t ILLEGAL_OPS[] = {0xD3, 0xDB, 0xDD, 0xE3, 0xE4, 0xEB, 0xEC, 0xED, 0xF4, 0xFC, 0xFD};

void sanitize_on_exec(uint16_t pc, uint8_t opcode)
{
    if (!S.active) return;
    GB_gameboy_t *gb = CTX.gb;
    uint16_t sp = GB_get_registers(gb)->sp;

    /* ---- reconcile the PREVIOUS instruction's control transfer ---- */
    bool irq = false;
    if (S.prev_valid) {
        int delta = (int)sp - (int)S.prev_sp;
        bool vec = (pc == 0x40 || pc == 0x48 || pc == 0x50 || pc == 0x58 || pc == 0x60);
        uint8_t op = S.prev_op;
        bool jmp = is_xfer_op(op) && !is_call_op(op) && !is_ret_op(op);
        bool legit = (is_call_op(op) && delta == -2) || (jmp && delta == 0);
        if (vec && !legit) {
            irq = true;
            delta += 2; /* the dispatch push */
        }
        if (op == 0x31 || op == 0xF9) {
            S.n_shadow = 0;           /* stack switch */
        }
        else if (is_call_op(op) && delta == -2) {
            uint16_t csp = (uint16_t)(S.prev_sp - 2);
            shadow_push(csp, rd16(csp), S.prev_pc, S.prev_bank, 0);
        }
        else if (is_ret_op(op) && delta == 2) {
            uint16_t rsp = S.prev_sp;
            while (S.n_shadow && S.shadow[S.n_shadow - 1].sp < rsp) S.n_shadow--;
            uint16_t resume = irq ? rd16((uint16_t)(sp)) : pc;
            if (S.n_shadow && S.shadow[S.n_shadow - 1].sp == rsp) {
                shadow_t *t = &S.shadow[S.n_shadow - 1];
                if (t->ret != resume) san_event(SC_RET, S.prev_pc, t->ret, resume, t->from_pc);
                S.n_shadow--;
            }
            else {
                S.rets_unmatched++;
            }
        }
        if (irq) {
            uint16_t ret = rd16(sp);
            shadow_push(sp, ret, ret, S.prev_bank, 1);
            if (S.selfid_bad) {
                unsigned bank = gb->mbc_rom_bank;
                if (bank < S.n_banks && S.selfid_bad[bank])
                    san_event(SC_IRQ_SELFID, ret, pc, (uint16_t)bank, S.prev_pc);
            }
        }
    }

    /* ---- this instruction ---- */
    if (pc >= 0x8000) {
        bool ok = false;
        for (unsigned i = 0; i < S.n_allow; i++)
            if (pc >= S.allow_lo[i] && pc <= S.allow_hi[i]) { ok = true; break; }
        if (!ok) san_event(SC_EXEC_RAM, pc, pc, opcode, 0);
    }
    for (unsigned i = 0; i < sizeof(ILLEGAL_OPS); i++)
        if (opcode == ILLEGAL_OPS[i]) { san_event(SC_ILLEGAL, pc, pc, opcode, 0); break; }
    if (S.sp_given) {
        bool out = sp < S.sp_lo || sp > S.sp_hi;
        if (out && !S.sp_out) san_event(SC_SP, pc, sp, 0, 0);
        S.sp_out = out;
    }
    S.prev_valid = true;
    S.prev_pc = pc;
    S.prev_sp = sp;
    S.prev_op = opcode;
    S.prev_bank = cur_bank_for_pc(pc);
}

/* ---------------- lifecycle ---------------- */

void sanitize_reset_all(void)
{
    free(S.sites); free(S.events); free(S.selfid_bad);
    memset(&S, 0, sizeof(S));
}

static int class_index(const char *name)
{
    for (int i = 0; i < SC_N; i++) if (!strcmp(SC_NAME[i], name)) return i;
    return -1;
}

static bool parse_class_list(const jval_t *arr, bool *out, bool dflt)
{
    for (int i = 0; i < SC_N; i++) out[i] = dflt;
    if (!arr) return true;
    if (arr->t != J_ARR) { set_err("bad class list (expected array of names)"); return false; }
    for (int i = 0; i < SC_N; i++) out[i] = false;
    for (unsigned k = 0; k < arr->n; k++) {
        const jval_t *v = arr->items[k];
        if (v->t != J_STR) { set_err("bad class name"); return false; }
        if (!strcmp(v->s, "all")) { for (int i = 0; i < SC_N; i++) out[i] = true; continue; }
        int c = class_index(v->s);
        if (c < 0) { set_err("unknown sanitizer class '%s'", v->s); return false; }
        out[c] = true;
    }
    return true;
}

bool cmd_sanitize_start(const jval_t *params)
{
    sanitize_reset_all();
    S.sites = calloc(SITE_CAP, sizeof(san_site_t));
    S.events = calloc(EVENT_CAP, sizeof(san_event_t));
    if (!S.sites || !S.events) { set_err("out of memory"); sanitize_reset_all(); return false; }
    bool selfid = j_bool(params, "selfid", false);
    if (!parse_class_list(j_arr(params, "classes"), S.enabled, true)) { sanitize_reset_all(); return false; }
    if (!selfid) { S.enabled[SC_IRQ_SELFID] = false; S.enabled[SC_MAP_SELFID] = false; }
    if (!parse_class_list(j_arr(params, "stop_on"), S.stop_on, false)) { sanitize_reset_all(); return false; }
    if (!j_arr(params, "stop_on")) for (int i = 0; i < SC_N; i++) S.stop_on[i] = false;
    S.freeze = j_bool(params, "freeze", false);

    /* exec allowlist: default HRAM (OAM-DMA stubs) */
    const jval_t *al = j_arr(params, "exec_allow");
    if (al && al->t == J_ARR) {
        for (unsigned k = 0; k < al->n && S.n_allow < MAX_ALLOW; k++) {
            const jval_t *r = al->items[k];
            if (r->t != J_ARR || r->n != 2) { set_err("exec_allow: [[lo,hi],...]"); sanitize_reset_all(); return false; }
            uint16_t lo, hi;
            if (!parse_u16(r->items[0], &lo) || !parse_u16(r->items[1], &hi)) { set_err("exec_allow: bad address"); sanitize_reset_all(); return false; }
            S.allow_lo[S.n_allow] = lo; S.allow_hi[S.n_allow] = hi; S.n_allow++;
        }
    }
    else { S.allow_lo[0] = 0xFF80; S.allow_hi[0] = 0xFFFE; S.n_allow = 1; }

    const jval_t *lo = j_get(params, "sp_lo"), *hi = j_get(params, "sp_hi");
    if (lo || hi) {
        if (!lo || !hi || !parse_u16(lo, &S.sp_lo) || !parse_u16(hi, &S.sp_hi)) { set_err("sp_lo+sp_hi both required"); sanitize_reset_all(); return false; }
        S.sp_given = true;
    }
    else S.enabled[SC_SP] = false;

    size_t rsz = 0; uint16_t rb;
    uint8_t *rom = GB_get_direct_access(CTX.gb, GB_DIRECT_ACCESS_ROM, &rsz, &rb);
    S.n_banks = (unsigned)(rsz / 0x4000);
    if (selfid && rom && S.n_banks) {
        S.selfid_bad = calloc(S.n_banks, 1);
        for (unsigned b = 1; b < S.n_banks; b++) S.selfid_bad[b] = rom[(size_t)b * 0x4000] != (uint8_t)b;
    }
    S.active = true;
    install_analysis_hooks();

    jw_t *w = &CTX.out;
    jw_raw(w, "{\"active\":true,\"classes\":[");
    bool first = true;
    for (int i = 0; i < SC_N; i++) if (S.enabled[i]) { jw_fmt(w, "%s\"%s\"", first ? "" : ",", SC_NAME[i]); first = false; }
    jw_raw(w, "],\"selfid_bad_banks\":[");
    first = true;
    if (S.selfid_bad) for (unsigned b = 1; b < S.n_banks; b++) if (S.selfid_bad[b]) { jw_fmt(w, "%s%u", first ? "" : ",", b); first = false; }
    jw_fmt(w, "],\"banks\":%u}", S.n_banks);
    return true;
}

bool cmd_sanitize_stop(const jval_t *params)
{
    (void)params;
    S.active = false;
    S.frozen = false;
    install_analysis_hooks();
    jw_raw(&CTX.out, "{\"active\":false}");
    return true;
}

void sanitize_resync(void)
{
    /* emulator state was replaced (snapshot.load / rewind.pop): the shadow
       call stack and previous-instruction latch describe a dead timeline */
    S.n_shadow = 0;
    S.prev_valid = false;
    S.sp_out = false;
}

bool cmd_sanitize_clear(const jval_t *params)
{
    (void)params;
    sanitize_resync();
    S.frozen = false;
    memset(S.counts, 0, sizeof(S.counts));
    if (S.sites) memset(S.sites, 0, SITE_CAP * sizeof(san_site_t));
    S.n_sites = 0; S.sites_dropped = 0; S.n_events = 0; S.events_total = 0;
    S.rets_unmatched = 0; S.shadow_overflow = 0;
    jw_raw(&CTX.out, "{\"cleared\":true}");
    return true;
}

static int site_cmp(const void *a, const void *b)
{
    const san_site_t *x = a, *y = b;
    if (x->cls != y->cls) return (int)x->cls - (int)y->cls;
    if (x->count != y->count) return x->count < y->count ? 1 : -1;
    return (int)x->pc - (int)y->pc;
}

bool cmd_sanitize_report(const jval_t *params)
{
    long max_sites = (long)j_int(params, "max_sites", 200);
    long max_events = (long)j_int(params, "max_events", 64);
    bool filt[SC_N];
    if (!parse_class_list(j_arr(params, "classes"), filt, true)) return false;
    jw_t *w = &CTX.out;
    jw_fmt(w, "{\"active\":%s,\"frozen\":%s,\"counts\":{", S.active ? "true" : "false", S.frozen ? "true" : "false");
    for (int i = 0; i < SC_N; i++) jw_fmt(w, "%s\"%s\":%llu", i ? "," : "", SC_NAME[i], (unsigned long long)S.counts[i]);
    jw_fmt(w, "},\"rets_unmatched\":%llu,\"shadow_overflow\":%llu,\"sites_total\":%u,\"sites_dropped\":%llu,\"events_total\":%llu,",
           (unsigned long long)S.rets_unmatched, (unsigned long long)S.shadow_overflow, S.n_sites,
           (unsigned long long)S.sites_dropped, (unsigned long long)S.events_total);
    /* sites */
    san_site_t *tmp = NULL;
    uint32_t n = 0;
    if (S.sites) {
        tmp = malloc(sizeof(san_site_t) * (S.n_sites ? S.n_sites : 1));
        for (uint32_t i = 0; i < SITE_CAP && tmp; i++) if (S.sites[i].used && filt[S.sites[i].cls]) tmp[n++] = S.sites[i];
        if (tmp) qsort(tmp, n, sizeof(san_site_t), site_cmp);
    }
    jw_raw(w, "\"sites\":[");
    for (uint32_t i = 0; i < n && (long)i < max_sites; i++) {
        san_site_t *s = &tmp[i];
        jw_fmt(w, "%s{\"class\":\"%s\",\"pc\":\"%04x\",\"bank\":%u,\"count\":%llu,\"first_frame\":%llu,\"first_instr\":%llu,"
               "\"addr\":\"%04x\",\"value\":\"%04x\",\"aux\":\"%04x\",\"ly\":%u,\"mode\":%u}",
               i ? "," : "", SC_NAME[s->cls], s->pc, s->bank, (unsigned long long)s->count,
               (unsigned long long)s->first_frame, (unsigned long long)s->first_instr,
               s->addr, s->value, s->aux, s->ly, s->mode);
    }
    free(tmp);
    jw_raw(w, "],\"events\":[");
    long k = 0;
    for (uint32_t i = 0; i < S.n_events && k < max_events; i++) {
        san_event_t *e = &S.events[i];
        if (!filt[e->cls]) continue;
        jw_fmt(w, "%s{\"class\":\"%s\",\"pc\":\"%04x\",\"bank\":%u,\"addr\":\"%04x\",\"value\":\"%04x\",\"aux\":\"%04x\","
               "\"ly\":%u,\"mode\":%u,\"frame\":%llu,\"instr\":%llu}",
               k ? "," : "", SC_NAME[e->cls], e->pc, e->bank, e->addr, e->value, e->aux, e->ly, e->mode,
               (unsigned long long)e->frame, (unsigned long long)e->instr);
        k++;
    }
    jw_raw(w, "]}");
    return true;
}
