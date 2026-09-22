#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <Core/gb.h>
#include <Core/debugger.h>
#include <Core/memory.h>
#include <Core/cheat_search.h>
#include "ctx.h"
#include "agent_ext.h"

/* ---------------- registers ---------------- */

bool cmd_regs_get(const jval_t *p)
{
    (void)p;
    GB_registers_t *r = GB_get_registers(CTX.gb);
    jw_t *w = &CTX.out;
    jw_fmt(w, "{\"pc\":\"%04x\",\"sp\":\"%04x\",\"af\":\"%04x\",\"bc\":\"%04x\",\"de\":\"%04x\",\"hl\":\"%04x\",",
           r->pc, r->sp, r->af, r->bc, r->de, r->hl);
    jw_fmt(w, "\"a\":%u,\"f\":%u,\"b\":%u,\"c\":%u,\"d\":%u,\"e\":%u,\"h\":%u,\"l\":%u,",
           r->af >> 8, r->af & 0xFF, r->bc >> 8, r->bc & 0xFF,
           r->de >> 8, r->de & 0xFF, r->hl >> 8, r->hl & 0xFF);
    jw_fmt(w, "\"ime\":%s,\"halted\":%s,\"stopped\":%s,\"double_speed\":%s,",
           ext_ime(CTX.gb) ? "true" : "false", ext_halted(CTX.gb) ? "true" : "false",
           ext_stopped(CTX.gb) ? "true" : "false", ext_double_speed(CTX.gb) ? "true" : "false");
    jw_fmt(w, "\"banks\":{\"rom\":%u,\"rom0\":%u,\"wram\":%u,\"vram\":%u,\"cart_ram\":%u},",
           ext_rom_bank(CTX.gb), ext_rom0_bank(CTX.gb), ext_wram_bank(CTX.gb),
           ext_vram_bank(CTX.gb), ext_cart_ram_bank(CTX.gb));
    uint16_t ie = GB_safe_read_memory(CTX.gb, 0xFFFF);
    uint16_t iff = GB_safe_read_memory(CTX.gb, 0xFF0F);
    jw_fmt(w, "\"ie\":\"%02x\",\"if\":\"%02x\"", ie & 0xFF, iff & 0xFF);
    const char *nm = GB_debugger_name_for_address(CTX.gb, r->pc);
    if (nm) { jw_raw(w, ",\"pc_symbol\":"); jw_esc(w, nm); }
    jw_raw(w, "}");
    return true;
}

bool cmd_regs_set(const jval_t *p)
{
    GB_registers_t *r = GB_get_registers(CTX.gb);
    bool any = false;
    const char *names16[] = {"pc", "sp", "af", "bc", "de", "hl"};
    uint16_t *regs16[] = {&r->pc, &r->sp, &r->af, &r->bc, &r->de, &r->hl};
    for (int i = 0; i < 6; i++) {
        const jval_t *v = j_get(p, names16[i]);
        if (!v) continue;
        uint16_t x;
        if (!parse_u16(v, &x)) { set_err("bad value for '%s'", names16[i]); return false; }
        if (i == 2) x &= 0xFFF0; /* F low nibble */
        *regs16[i] = x;
        any = true;
    }
    const char *names8[] = {"a", "f", "b", "c", "d", "e", "h", "l"};
    for (int i = 0; i < 8; i++) {
        const jval_t *v = j_get(p, names8[i]);
        if (!v) continue;
        uint16_t x;
        if (!parse_u16(v, &x)) { set_err("bad value for '%s'", names8[i]); return false; }
        uint16_t *pair = regs16[i / 2 + 2]; /* af,bc,de,hl */
        if (i % 2 == 0) *pair = (uint16_t)((x << 8) | (*pair & 0x00FF));
        else *pair = (uint16_t)((*pair & 0xFF00) | (x & (i == 1 ? 0xF0 /*F*/ : 0xFF)));
        any = true;
    }
    if (!any) { set_err("no registers given"); return false; }
    return cmd_regs_get(p);
}

/* ---------------- eval / disasm / symbols ---------------- */

bool cmd_eval(const jval_t *p)
{
    const char *expr = j_str(p, "expr", NULL);
    if (!expr) { set_err("missing 'expr'"); return false; }
    uint16_t val, bank;
    if (GB_debugger_evaluate(CTX.gb, expr, &val, &bank)) {
        set_err("cannot evaluate '%s'", expr);
        return false;
    }
    jw_fmt(&CTX.out, "{\"value\":\"%04x\",\"bank\":%d}", val, (int16_t)bank);
    return true;
}

static void log_capture_start(void)
{
    jw_free(&CTX.log_cap);
    jw_init(&CTX.log_cap, 4096);
    CTX.log_capturing = true;
}

static void log_capture_stop(void)
{
    CTX.log_capturing = false;
}

bool cmd_disasm(const jval_t *p)
{
    uint16_t addr = j_has(p, "addr") ? 0 : GB_get_registers(CTX.gb)->pc;
    if (j_has(p, "addr") && !parse_u16(j_get(p, "addr"), &addr)) { set_err("bad 'addr'"); return false; }
    long n = j_int(p, "n", 16);
    if (n < 1 || n > 512) { set_err("n out of range (1..512)"); return false; }
    log_capture_start();
    GB_cpu_disassemble(CTX.gb, addr, (uint16_t)n);
    log_capture_stop();
    jw_t *w = &CTX.out;
    jw_fmt(w, "{\"addr\":\"%04x\",\"n\":%ld,\"listing\":", addr, n);
    jw_esc(w, CTX.log_cap.buf ? CTX.log_cap.buf : "");
    jw_raw(w, "}");
    return true;
}

bool cmd_symbols_load(const jval_t *p)
{
    const char *path = j_str(p, "path", NULL);
    if (!path) { set_err("missing 'path'"); return false; }
    if (access(path, R_OK) != 0) { set_err("cannot read symbols file '%s'", path); return false; }
    GB_debugger_load_symbol_file(CTX.gb, path);
    jw_fmt(&CTX.out, "{\"loaded\":"); jw_esc(&CTX.out, path); jw_raw(&CTX.out, "}");
    return true;
}

bool cmd_symbols_clear(const jval_t *p)
{
    (void)p;
    GB_debugger_clear_symbols(CTX.gb);
    jw_raw(&CTX.out, "{\"cleared\":true}");
    return true;
}

bool cmd_symbol_resolve(const jval_t *p)
{
    const jval_t *q = j_get(p, "q");
    if (!q) { set_err("missing 'q'"); return false; }
    uint16_t addr;
    /* numeric-ish → describe address, else evaluate as expression/symbol */
    if ((q->t == J_NUM) || (q->t == J_STR && (q->s[0] == '$' || q->s[0] == '0' || (q->s[0] >= '1' && q->s[0] <= '9')))) {
        if (!parse_u16(q, &addr)) { set_err("bad address"); return false; }
        const char *d = GB_debugger_describe_address(CTX.gb, addr,
                                                     (uint16_t)j_int(p, "bank", 0xFFFF) == 0xFFFF ? -1 : (uint16_t)j_int(p, "bank", 0),
                                                     j_bool(p, "exact", false), j_bool(p, "prefer_local", true));
        jw_fmt(&CTX.out, "{\"addr\":\"%04x\",\"desc\":", addr);
        jw_esc(&CTX.out, d);
        jw_raw(&CTX.out, "}");
        return true;
    }
    const char *expr = q->s;
    uint16_t bank;
    if (GB_debugger_evaluate(CTX.gb, expr, &addr, &bank)) { set_err("cannot resolve '%s'", expr); return false; }
    jw_fmt(&CTX.out, "{\"addr\":\"%04x\",\"bank\":%d}", addr, (int16_t)bank);
    return true;
}

/* ---------------- memory read/write ---------------- */

static uint8_t *region_ptr(const char *region, size_t *size, uint16_t *bank)
{
    if (!strcmp(region, "rom") || !strcmp(region, "rom0")) return GB_get_direct_access(CTX.gb, GB_DIRECT_ACCESS_ROM, size, bank);
    if (!strcmp(region, "vram")) return GB_get_direct_access(CTX.gb, GB_DIRECT_ACCESS_VRAM, size, bank);
    if (!strcmp(region, "wram")) return GB_get_direct_access(CTX.gb, GB_DIRECT_ACCESS_RAM, size, bank);
    if (!strcmp(region, "cart_ram")) return GB_get_direct_access(CTX.gb, GB_DIRECT_ACCESS_CART_RAM, size, bank);
    if (!strcmp(region, "oam")) return GB_get_direct_access(CTX.gb, GB_DIRECT_ACCESS_OAM, size, bank);
    if (!strcmp(region, "hram")) return GB_get_direct_access(CTX.gb, GB_DIRECT_ACCESS_HRAM, size, bank);
    if (!strcmp(region, "bootrom")) return GB_get_direct_access(CTX.gb, GB_DIRECT_ACCESS_BOOTROM, size, bank);
    if (!strcmp(region, "bgp")) return GB_get_direct_access(CTX.gb, GB_DIRECT_ACCESS_BGP, size, bank);
    if (!strcmp(region, "obp")) return GB_get_direct_access(CTX.gb, GB_DIRECT_ACCESS_OBP, size, bank);
    if (!strcmp(region, "io")) return GB_get_direct_access(CTX.gb, GB_DIRECT_ACCESS_IO, size, bank);
    return NULL;
}

/* bank-aware read; if bank < 0 read through the CPU map */
static uint8_t read_addr(uint16_t addr, int bank)
{
    GB_gameboy_t *gb = CTX.gb;
    if (bank < 0) {
        CTX.probing = true;
        uint8_t v = GB_safe_read_memory(gb, addr);
        CTX.probing = false;
        return v;
    }
    if (addr < 0x8000) {
        size_t lin = (size_t)bank * 0x4000 + (addr & 0x3FFF);
        return lin < gb->rom_size ? gb->rom[lin] : 0xFF;
    }
    if (addr < 0xA000) {
        size_t lin = (size_t)bank * 0x2000 + (addr & 0x1FFF);
        return lin < gb->vram_size ? gb->vram[lin] : 0xFF;
    }
    if (addr < 0xC000) {
        size_t lin = (size_t)bank * 0x2000 + (addr & 0x1FFF);
        return lin < gb->mbc_ram_size ? gb->mbc_ram[lin] : 0xFF;
    }
    if (addr < 0xE000) {
        size_t lin = (size_t)bank * 0x1000 + (addr & 0xFFF);
        return lin < gb->ram_size ? gb->ram[lin] : 0xFF;
    }
    CTX.probing = true;
    uint8_t v = GB_safe_read_memory(gb, addr);
    CTX.probing = false;
    return v;
}

bool cmd_mem_read(const jval_t *p)
{
    uint16_t addr;
    long len = j_int(p, "len", 16);
    const char *region = j_str(p, "region", NULL);
    long offset = j_int(p, "offset", 0);

    if (region) {
        size_t size; uint16_t curbank;
        uint8_t *ptr = region_ptr(region, &size, &curbank);
        if (!ptr) { set_err("unknown region '%s'", region); return false; }
        if (offset < 0 || (size_t)offset >= size) { set_err("offset out of range (size %zu)", size); return false; }
        if (len < 1 || len > 65536) { set_err("len out of range"); return false; }
        if ((size_t)(offset + len) > size) len = size - offset;
        jw_t *w = &CTX.out;
        jw_fmt(w, "{\"region\":");
        jw_esc(w, region);
        jw_fmt(w, ",\"offset\":%ld,\"bank\":%u,\"len\":%ld,\"data\":", offset, curbank, len);
        jw_hex(w, ptr + offset, (size_t)len);
        jw_raw(w, "}");
        return true;
    }

    if (!parse_u16(j_get(p, "addr"), &addr)) { set_err("bad 'addr'"); return false; }
    if (len < 1 || len > 65536) { set_err("len out of range"); return false; }
    int bank = j_has(p, "bank") ? (int)j_int(p, "bank", -1) : -1;
    if ((uint32_t)addr + len > 0x10000) len = 0x10000 - addr;

    jw_t *w = &CTX.out;
    jw_fmt(w, "{\"addr\":\"%04x\",\"bank\":%d,\"len\":%ld,\"data\":", addr, bank, len);
    jw_raw(w, "\"");
    for (long i = 0; i < len; i++)
        jw_fmt(w, "%02x", read_addr((uint16_t)(addr + i), bank));
    jw_raw(w, "\"}");
    return true;
}

static uint8_t hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0xFF;
}

bool cmd_mem_write(const jval_t *p)
{
    const char *region = j_str(p, "region", NULL);
    long offset = j_int(p, "offset", 0);
    const char *hex = j_str(p, "data", NULL);
    if (!hex) { set_err("missing 'data' (hex string)"); return false; }
    /* decode hex */
    size_t hexlen = strlen(hex);
    size_t cap = hexlen / 2 + 2;
    uint8_t *buf = malloc(cap);
    if (!buf) { set_err("oom"); return false; }
    size_t n = 0;
    for (size_t i = 0; i + 1 < hexlen; i += 2) {
        uint8_t hi = hex_nibble(hex[i]), lo = hex_nibble(hex[i + 1]);
        if (hi == 0xFF || lo == 0xFF) { free(buf); set_err("bad hex in 'data'"); return false; }
        buf[n++] = hi << 4 | lo;
    }

    if (region) {
        size_t size; uint16_t bank;
        uint8_t *ptr = region_ptr(region, &size, &bank);
        if (!ptr) { free(buf); set_err("unknown region '%s'", region); return false; }
        if (offset < 0 || (size_t)offset + n > size) { free(buf); set_err("write out of range (size %zu)", size); return false; }
        if (!strcmp(region, "rom") || !strcmp(region, "rom0") || !strcmp(region, "bootrom")) { free(buf); set_err("region is read-only"); return false; }
        memcpy(ptr + offset, buf, n);
    }
    else {
        uint16_t addr;
        if (!parse_u16(j_get(p, "addr"), &addr)) { free(buf); set_err("bad 'addr'"); return false; }
        if ((uint32_t)addr + n > 0x10000) { free(buf); set_err("write wraps past $FFFF"); return false; }
        CTX.probing = true;
        for (size_t i = 0; i < n; i++)
            GB_write_memory(CTX.gb, (uint16_t)(addr + i), buf[i]);
        CTX.probing = false;
    }
    free(buf);
    jw_fmt(&CTX.out, "{\"written\":%zu}", n);
    return true;
}

bool cmd_mem_regions(const jval_t *p)
{
    (void)p;
    static const char *names[] = {"rom", "vram", "wram", "cart_ram", "oam", "hram", "bootrom", "bgp", "obp", "io", NULL};
    jw_t *w = &CTX.out;
    jw_raw(w, "{\"regions\":[");
    for (int i = 0; names[i]; i++) {
        size_t size; uint16_t bank;
        uint8_t *ptr = region_ptr(names[i], &size, &bank);
        if (!ptr || !size) continue;
        jw_fmt(w, "%s{\"name\":\"%s\",\"size\":%zu,\"bank\":%u}%s",
               i ? "," : "", names[i], size, bank, "");
    }
    jw_raw(w, "]}");
    return true;
}

/* ---------------- memory capture / diff ---------------- */

bool cmd_mem_capture(const jval_t *p)
{
    const char *name = j_str(p, "name", NULL);
    int slot = -1;
    if (name) {
        for (int i = 0; i < MAX_MEMSNAPS; i++)
            if (CTX.memsnaps[i].used && !strncmp(CTX.memsnaps[i].name, name, MAX_NAME)) { slot = i; break; }
    }
    if (slot < 0) for (int i = 0; i < MAX_MEMSNAPS; i++) if (!CTX.memsnaps[i].used) { slot = i; break; }
    if (slot < 0) { set_err("no free mem capture slots (max %d)", MAX_MEMSNAPS); return false; }
    memsnap_t *m = &CTX.memsnaps[slot];
    free(m->vram); free(m->wram); free(m->oam); free(m->hram); free(m->cart);
    memset(m, 0, sizeof(*m));
    m->used = true;
    snprintf(m->name, MAX_NAME, "%s", name ? name : "capture");

    uint16_t bank;
    size_t sz;
    uint8_t *src;
    src = GB_get_direct_access(CTX.gb, GB_DIRECT_ACCESS_VRAM, &sz, &m->vram_bank);
    if (sz) { m->vram_size = sz; m->vram = malloc(sz); memcpy(m->vram, src, sz); }
    src = GB_get_direct_access(CTX.gb, GB_DIRECT_ACCESS_RAM, &sz, &m->wram_bank);
    if (sz) { m->wram_size = sz; m->wram = malloc(sz); memcpy(m->wram, src, sz); }
    src = GB_get_direct_access(CTX.gb, GB_DIRECT_ACCESS_OAM, &sz, NULL);
    if (sz) { m->oam_size = sz; m->oam = malloc(sz); memcpy(m->oam, src, sz); }
    src = GB_get_direct_access(CTX.gb, GB_DIRECT_ACCESS_HRAM, &sz, NULL);
    if (sz) { m->hram_size = sz; m->hram = malloc(sz); memcpy(m->hram, src, sz); }
    src = GB_get_direct_access(CTX.gb, GB_DIRECT_ACCESS_CART_RAM, &sz, &m->cart_bank);
    if (sz && src) { m->cart_size = sz; m->cart = malloc(sz); memcpy(m->cart, src, sz); }
    bank = 0;

    jw_fmt(&CTX.out, "{\"slot\":%d,\"name\":", slot);
    jw_esc(&CTX.out, m->name);
    jw_fmt(&CTX.out, ",\"sizes\":{\"vram\":%zu,\"wram\":%zu,\"oam\":%zu,\"hram\":%zu,\"cart_ram\":%zu}}",
           m->vram_size, m->wram_size, m->oam_size, m->hram_size, m->cart_size);
    return true;
}

static void diff_region(jw_t *w, const char *rname, const uint8_t *a, const uint8_t *b, size_t n,
                        long *changes, long max_changes, bool *first)
{
    for (size_t i = 0; i < n; i++) {
        if (a[i] == b[i]) continue;
        if (*changes >= max_changes) { (*changes)++; continue; }
        (*changes)++;
        unsigned addr = 0;
        int bank = -1;
        if (!strcmp(rname, "vram")) { addr = 0x8000 + (i & 0x1FFF); bank = i / 0x2000; }
        else if (!strcmp(rname, "wram")) { addr = 0xC000 + (i & 0xFFF) + (i >= 0x1000 ? 0 : 0); bank = i / 0x1000;
            addr = (i < 0x1000) ? 0xC000 + i : 0xD000 + (i & 0xFFF); }
        else if (!strcmp(rname, "oam")) addr = 0xFE00 + i;
        else if (!strcmp(rname, "hram")) addr = 0xFF80 + i;
        else if (!strcmp(rname, "cart_ram")) { addr = 0xA000 + (i & 0x1FFF); bank = i / 0x2000; }
        jw_fmt(w, "%s{\"region\":\"%s\",\"off\":%zu,\"addr\":\"%04x\",\"bank\":%d,\"a\":%u,\"b\":%u}",
               *first ? "" : ",", rname, i, addr, bank, a[i], b[i]);
        *first = false;
    }
}

bool cmd_mem_diff(const jval_t *p)
{
    const char *na = j_str(p, "a", NULL);
    const char *nb = j_str(p, "b", "live"); /* "live" = current memory */
    memsnap_t *A = NULL, *B = NULL;
    for (int i = 0; i < MAX_MEMSNAPS; i++) {
        if (CTX.memsnaps[i].used && na && !strncmp(CTX.memsnaps[i].name, na, MAX_NAME)) A = &CTX.memsnaps[i];
        if (CTX.memsnaps[i].used && nb && !strncmp(CTX.memsnaps[i].name, nb, MAX_NAME)) B = &CTX.memsnaps[i];
    }
    if (!A) { set_err("no capture named '%s' (run mem.capture first)", na ? na : ""); return false; }
    if (nb && strcmp(nb, "live") && !B) { set_err("no capture named '%s'", nb); return false; }
    memsnap_t live = {0};
    if (!B) {
        /* capture live into temp */
        uint16_t bk; size_t sz; uint8_t *src;
        src = GB_get_direct_access(CTX.gb, GB_DIRECT_ACCESS_VRAM, &sz, &bk);
        live.vram = malloc(sz); memcpy(live.vram, src, sz); live.vram_size = sz;
        src = GB_get_direct_access(CTX.gb, GB_DIRECT_ACCESS_RAM, &sz, &bk);
        live.wram = malloc(sz); memcpy(live.wram, src, sz); live.wram_size = sz;
        src = GB_get_direct_access(CTX.gb, GB_DIRECT_ACCESS_OAM, &sz, NULL);
        live.oam = malloc(sz); memcpy(live.oam, src, sz); live.oam_size = sz;
        src = GB_get_direct_access(CTX.gb, GB_DIRECT_ACCESS_HRAM, &sz, NULL);
        live.hram = malloc(sz); memcpy(live.hram, src, sz); live.hram_size = sz;
        src = GB_get_direct_access(CTX.gb, GB_DIRECT_ACCESS_CART_RAM, &sz, &bk);
        if (src && sz) { live.cart = malloc(sz); memcpy(live.cart, src, sz); live.cart_size = sz; }
        B = &live;
    }
    long max_changes = j_int(p, "max", 1024);
    if (max_changes > 65536) max_changes = 65536;
    jw_t *w = &CTX.out;
    jw_raw(w, "{\"changes\":[");
    long changes = 0;
    bool first = true;
    size_t n;
    n = A->vram_size < B->vram_size ? A->vram_size : B->vram_size;
    diff_region(w, "vram", A->vram, B->vram, n, &changes, max_changes, &first);
    n = A->wram_size < B->wram_size ? A->wram_size : B->wram_size;
    diff_region(w, "wram", A->wram, B->wram, n, &changes, max_changes, &first);
    n = A->oam_size < B->oam_size ? A->oam_size : B->oam_size;
    diff_region(w, "oam", A->oam, B->oam, n, &changes, max_changes, &first);
    n = A->hram_size < B->hram_size ? A->hram_size : B->hram_size;
    diff_region(w, "hram", A->hram, B->hram, n, &changes, max_changes, &first);
    n = A->cart_size < B->cart_size ? A->cart_size : B->cart_size;
    if (n) diff_region(w, "cart_ram", A->cart, B->cart, n, &changes, max_changes, &first);
    jw_raw(w, "]");
    jw_fmt(w, ",\"truncated\":%s}", changes > max_changes ? "true" : "false");
    free(live.vram); free(live.wram); free(live.oam); free(live.hram); free(live.cart);
    return true;
}

/* ---------------- memory search (core cheat_search engine) ---------------- */

bool cmd_mem_search(const jval_t *p)
{
    const char *filter = j_str(p, "filter", NULL);
    if (!filter) { set_err("missing 'filter', e.g. \"value == old\", \"value < 10\", \"value == old + 2\""); return false; }
    bool wide = j_bool(p, "wide", false);
    long max_results = j_int(p, "max", 512);
    if (max_results > 8192) max_results = 8192;
    CTX.probing = true;
    bool ok = GB_cheat_search_filter(CTX.gb, filter, wide ? GB_CHEAT_SEARCH_DATA_TYPE_16BIT : GB_CHEAT_SEARCH_DATA_TYPE_8BIT);
    CTX.probing = false;
    if (!ok) { set_err("bad filter expression '%s'", filter); return false; }
    size_t count = GB_cheat_search_result_count(CTX.gb);
    jw_t *w = &CTX.out;
    jw_fmt(w, "{\"count\":%zu,\"results\":[", count);
    if (count) {
        size_t want = count < (size_t)max_results ? count : (size_t)max_results;
        GB_cheat_search_result_t *all = malloc(count * sizeof(*all));
        if (all) {
            GB_cheat_search_get_results(CTX.gb, all);
            for (size_t i = 0; i < want; i++) {
                jw_fmt(w, "%s{\"addr\":\"%04x\",\"bank\":%u,\"value\":%u}",
                       i ? "," : "", all[i].addr, all[i].bank, all[i].value);
            }
            free(all);
        }
    }
    jw_raw(w, "]}");
    return true;
}

bool cmd_mem_search_reset(const jval_t *p)
{
    (void)p;
    GB_cheat_search_reset(CTX.gb);
    jw_raw(&CTX.out, "{\"reset\":true}");
    return true;
}
