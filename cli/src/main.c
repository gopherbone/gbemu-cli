#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libgen.h>
#include <stdarg.h>
#include <ctype.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#include <Core/gb.h>
#include <Core/debugger.h>
#include <Core/random.h>
#include <Core/rewind.h>
#include <Core/cheat_search.h>
#include <Core/apu.h>
#include "json.h"
#include "ctx.h"
#include "agent_ext.h"

ctx_t CTX;
static uint32_t framebuffer[256 * 224];

#define GBEMU_VERSION "0.1.0"

void set_err(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(CTX.err, sizeof(CTX.err), fmt, args);
    va_end(args);
}

/* ---------------- core callbacks ---------------- */

static void log_cb(GB_gameboy_t *gb, const char *string, GB_log_attributes_t attr)
{
    (void)gb; (void)attr;
    if (CTX.log_capturing) {
        jw_raw(&CTX.log_cap, string);
    }
    else {
        fprintf(stderr, "%s", string);
    }
}

static char *input_cb(GB_gameboy_t *gb)
{
    (void)gb;
    return NULL; /* debugger REPL never engages; be defensive anyway */
}

static char *async_input_cb(GB_gameboy_t *gb)
{
    (void)gb;
    return NULL;
}

static uint32_t rgb_encode_cb(GB_gameboy_t *gb, uint8_t r, uint8_t g, uint8_t b)
{
    (void)gb;
    return 0xFF000000 | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

static void sample_cb(GB_gameboy_t *gb, GB_sample_t *sample)
{
    (void)gb; (void)sample; /* v1: audio discarded */
}

/* ---------------- address parsing ---------------- */

static bool str_to_u16(const char *s, uint16_t *out)
{
    if (!s || !*s) return false;
    char *end = NULL;
    long v;
    if (s[0] == '$') v = strtol(s + 1, &end, 16);
    else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) v = strtol(s + 2, &end, 16);
    else if (isdigit((unsigned char)s[0])) v = strtol(s, &end, 10);
    else return false;
    if (end == s || *end != 0) return false;
    if (v < -0x8000 || v > 0xFFFF) return false;
    *out = (uint16_t)v;
    return true;
}

/* Accepts J_NUM or J_STR. Strings may be numbers or debugger expressions/symbols. */
bool parse_u16(const jval_t *v, uint16_t *out)
{
    if (!v) return false;
    if (v->t == J_NUM) { *out = (uint16_t)(long long)v->num; return true; }
    if (v->t != J_STR) return false;
    if (str_to_u16(v->s, out)) return true;
    if (!CTX.loaded) return false;
    uint16_t bank;
    bool err = GB_debugger_evaluate(CTX.gb, v->s, out, &bank);
    return !err;
}

long j_addr_expr(const jval_t *params, const char *key, long dflt)
{
    const jval_t *v = j_get(params, key);
    uint16_t out;
    if (parse_u16(v, &out)) return out;
    return dflt;
}

/* ---------------- response plumbing ---------------- */

/* ---------------- emulator lifecycle ---------------- */

void free_analysis_state(void)
{
    for (int i = 0; i < MAX_SNAPSHOTS; i++) {
        free(CTX.snaps[i].buf); CTX.snaps[i] = (snap_t){0};
    }
    for (int i = 0; i < MAX_MEMSNAPS; i++) {
        memsnap_t *m = &CTX.memsnaps[i];
        free(m->vram); free(m->wram); free(m->oam); free(m->hram); free(m->cart);
        *m = (memsnap_t){0};
    }
    for (int i = 0; i < MAX_BREAKPOINTS; i++) { free(CTX.bps[i].cond); CTX.bps[i] = (bp_t){0}; }
    for (int i = 0; i < MAX_WATCHPOINTS; i++) { free(CTX.wps[i].cond); CTX.wps[i] = (wp_t){0}; }
    free(CTX.bp_hits); CTX.bp_hits = NULL; CTX.bp_hits_head = CTX.bp_hits_count = 0; CTX.bp_hits_total = 0;
    free(CTX.cov_bits); CTX.cov_bits = NULL; CTX.cov_rom_size = 0; CTX.cov_active = false;
    if (CTX.trace.ring) CTX.trace.total = CTX.trace.dropped = CTX.trace.head = CTX.trace.count = 0;
    CTX.trace.active = false;
    CTX.script_len = CTX.script_pos = 0;
    CTX.key_mask = 0;
    CTX.stop_pending = false;
}

static const gbemu_model_t models[] = {
    {"dmg",  GB_MODEL_DMG_B, "dmg_boot.bin"},
    {"mgb",  GB_MODEL_MGB,   "mgb_boot.bin"},
    {"sgb",  GB_MODEL_SGB,   "sgb_boot.bin"},
    {"sgb2", GB_MODEL_SGB2,  "sgb2_boot.bin"},
    {"cgb",  GB_MODEL_CGB_E, "cgb_boot.bin"},
    {"agb",  GB_MODEL_AGB_A, "agb_boot.bin"},
};

const gbemu_model_t *gbemu_find_model(const char *name)
{
    for (size_t i = 0; i < sizeof(models) / sizeof(models[0]); i++)
        if (strcmp(models[i].name, name) == 0) return &models[i];
    return NULL;
}

static bool boot_rom_load(const char *boot, const char *model_boot, char *desc, size_t descsz)
{
    /* boot: NULL → default by model; "builtin" → embedded DMG boot; else file path or name in bootrom dir */
    if (boot && strcmp(boot, "builtin") == 0) {
        GB_load_boot_rom_from_buffer(CTX.gb, builtin_dmg_boot, sizeof(builtin_dmg_boot));
        snprintf(desc, descsz, "builtin-dmg");
        return true;
    }
    char path[1536];
    const char *p = boot;
    if (!p) {
        snprintf(path, sizeof(path), "%s/%s", CTX.bootrom_dir, model_boot);
        p = path;
    }
    else if (access(p, R_OK) != 0 && !strchr(p, '/')) {
        snprintf(path, sizeof(path), "%s/%s", CTX.bootrom_dir, p);
        p = path;
    }
    if (GB_load_boot_rom(CTX.gb, p)) {
        set_err("cannot load boot ROM '%s' (set boot:\"builtin\" or --bootrom-dir)", p);
        return false;
    }
    snprintf(desc, descsz, "%s", p);
    return true;
}

/* ---------------- session commands ---------------- */

static bool cmd_ping(const jval_t *p)
{
    (void)p;
    jw_raw(&CTX.out, "{\"pong\":true,\"version\":\"" GBEMU_VERSION "\",\"sameboy\":\"" GB_VERSION "\"}");
    return true;
}

/* writes session info fields (no surrounding braces) */
static void emit_info_fields(jw_t *w)
{
    jw_fmt(w, "\"loaded\":%s", CTX.loaded ? "true" : "false");
    if (CTX.loaded) {
        const uint8_t *rom = (const uint8_t *)GB_get_direct_access(CTX.gb, GB_DIRECT_ACCESS_ROM, NULL, NULL);
        jw_fmt(w, ",\"title\":"); jw_esc(w, CTX.title);
        jw_fmt(w, ",\"rom_path\":"); jw_esc(w, CTX.rom_path);
        jw_fmt(w, ",\"crc32\":\"%08x\"", GB_get_rom_crc32(CTX.gb));
        jw_fmt(w, ",\"rom_size\":%u", (unsigned)CTX.gb->rom_size);
        jw_fmt(w, ",\"ram_size\":%u", (unsigned)CTX.gb->ram_size);
        jw_fmt(w, ",\"cart_ram_size\":%u", (unsigned)CTX.gb->mbc_ram_size);
        jw_fmt(w, ",\"cart_type\":\"%02x\"", rom[0x147]);
        jw_fmt(w, ",\"rom_banks\":%u", (unsigned)(CTX.gb->rom_size / 0x4000));
        const char *mname = "dmg";
        if (GB_is_cgb(CTX.gb)) mname = GB_get_model(CTX.gb) == GB_MODEL_AGB_A ? "agb" : "cgb";
        else if (GB_is_sgb(CTX.gb)) mname = "sgb";
        jw_fmt(w, ",\"model\":\"%s\"", mname);
        jw_fmt(w, ",\"instructions\":%llu,\"frames\":%llu",
               (unsigned long long)CTX.instr_count, (unsigned long long)CTX.frame_count);
        jw_fmt(w, ",\"clock\":%u", GB_get_clock_rate(CTX.gb));
    }
}

static bool cmd_info(const jval_t *p)
{
    (void)p;
    jw_raw(&CTX.out, "{");
    emit_info_fields(&CTX.out);
    jw_raw(&CTX.out, "}");
    return true;
}

static bool cmd_load_rom(const jval_t *p)
{
    const char *path = j_str(p, "path", NULL);
    if (!path) { set_err("missing 'path'"); return false; }
    if (access(path, R_OK)) { set_err("cannot read rom '%s'", path); return false; }
    const char *model_name = j_str(p, "model", "dmg");
    const gbemu_model_t *mm = gbemu_find_model(model_name);
    if (!mm) { set_err("unknown model '%s' (dmg|mgb|sgb|sgb2|cgb|agb)", model_name); return false; }

    uint64_t seed = (uint64_t)j_int(p, "seed", 0);
    if (!j_has(p, "seed")) seed = 9001;
    CTX.seed = seed;

    core_dispose();
    free_analysis_state();
    CTX.cycles_raw = 0;

    GB_random_seed(seed);
    CTX.gb = GB_alloc();
    GB_init(CTX.gb, mm->model);
    core_setup_callbacks();

    char boot_desc[1536] = {0};
    if (!boot_rom_load(j_str(p, "boot", NULL), mm->boot, boot_desc, sizeof(boot_desc))) {
        GB_free(CTX.gb); GB_dealloc(CTX.gb); CTX.gb = NULL;
        return false;
    }
    if (GB_load_rom(CTX.gb, path)) {
        set_err("failed to load rom '%s'", path);
        GB_free(CTX.gb); GB_dealloc(CTX.gb); CTX.gb = NULL;
        return false;
    }
    CTX.loaded = true;
    snprintf(CTX.rom_path, sizeof(CTX.rom_path), "%s", path);
    GB_get_rom_title(CTX.gb, CTX.title);

    const char *sav = j_str(p, "sav", NULL);
    int sav_status = -1;
    if (sav && access(sav, R_OK) == 0) sav_status = GB_load_battery(CTX.gb, sav);

    const char *syms = j_str(p, "symbols", NULL);
    if (syms) GB_debugger_load_symbol_file(CTX.gb, syms);

    CTX.instr_count = CTX.frame_count = 0;

    jw_t *w = &CTX.out;
    jw_raw(w, "{");
    emit_info_fields(w);
    jw_raw(w, ",\"bootrom\":"); jw_esc(w, boot_desc);
    jw_fmt(w, ",\"seed\":%llu", (unsigned long long)seed);
    jw_fmt(w, ",\"sav_status\":%d", sav_status);
    jw_fmt(w, ",\"symbols_loaded\":%s", syms ? "true" : "false");
    jw_raw(w, "}");
    return true;
}

void core_setup_callbacks(void)
{
    GB_set_log_callback(CTX.gb, log_cb);
    GB_set_input_callback(CTX.gb, input_cb);
    GB_set_async_input_callback(CTX.gb, async_input_cb);
    GB_set_rgb_encode_callback(CTX.gb, rgb_encode_cb);
    GB_set_pixels_output(CTX.gb, framebuffer);
    GB_set_sample_rate(CTX.gb, 44100);
    GB_apu_set_sample_callback(CTX.gb, sample_cb);
    GB_debugger_set_disabled(CTX.gb, true);
}

void core_dispose(void)
{
    if (!CTX.gb) return;
    GB_free(CTX.gb);
    GB_dealloc(CTX.gb);
    CTX.gb = NULL;
    CTX.loaded = false;
    CTX.sandbox = false;
}

/* turbo: disable SameBoy's wall-clock pacing (GB_timing_sync sleep only).
   Emulated timing, cycles and determinism are untouched; this only removes
   the realtime throttle for headless analysis runs. */
static bool cmd_turbo(const jval_t *p)
{
    bool on = true;
    if (p && p->t == J_OBJ) on = j_bool(p, "on", true);
    bool dont_skip = j_bool(p, "dont_skip", true);
    if (CTX.gb) {
        CTX.gb->turbo = on;
        /* dont_skip keeps vblank reporting 1:1 with emulated frames: the
           frame-skip fast path (GB_timing_sync_turbo) is bypassed, so the
           harness's frame accounting stays identical to realtime pacing. */
        CTX.gb->turbo_dont_skip = dont_skip;
        CTX.gb->turbo_cap_multiplier = 0;   /* uncap: no sleep at all */
    }
    jw_fmt(&CTX.out, "{\"turbo\":%s,\"dont_skip\":%s,\"loaded\":%s}",
           on ? "true" : "false", dont_skip ? "true" : "false",
           CTX.loaded ? "true" : "false");
    return true;
}

static bool cmd_quit(const jval_t *p)
{
    (void)p;
    jw_raw(&CTX.out, "{\"bye\":true}");
    return true;
}

/* ---------------- dispatch ---------------- */

static const struct { const char *name; bool (*fn)(const jval_t *); bool needs_rom; } commands[] = {
    {"ping", cmd_ping, false},
    {"turbo", cmd_turbo, false},
    {"info", cmd_info, false},
    {"load_rom", cmd_load_rom, false},
    {"quit", cmd_quit, false},
    {"regs.get", cmd_regs_get, true},
    {"regs.set", cmd_regs_set, true},
    {"eval", cmd_eval, true},
    {"disasm", cmd_disasm, true},
    {"mem.read", cmd_mem_read, true},
    {"mem.write", cmd_mem_write, true},
    {"mem.regions", cmd_mem_regions, true},
    {"mem.capture", cmd_mem_capture, true},
    {"mem.diff", cmd_mem_diff, true},
    {"mem.search", cmd_mem_search, true},
    {"mem.search.reset", cmd_mem_search_reset, true},
    {"rom.search", cmd_rom_search, true},
    {"symbols.load", cmd_symbols_load, true},
    {"symbols.clear", cmd_symbols_clear, true},
    {"symbols.resolve", cmd_symbol_resolve, true},
    {"step", cmd_step, true},
    {"run.frames", cmd_run_frames, true},
    {"run.until", cmd_run_until, true},
    {"break.add", cmd_break_add, true},
    {"break.list", cmd_break_list, true},
    {"break.del", cmd_break_del, true},
    {"break.clear", cmd_break_clear, true},
    {"break.log", cmd_break_log, true},
    {"watch.add", cmd_watch_add, true},
    {"watch.list", cmd_watch_list, true},
    {"watch.del", cmd_watch_del, true},
    {"watch.clear", cmd_watch_clear, true},
    {"trace.start", cmd_trace_start, true},
    {"trace.stop", cmd_trace_stop, true},
    {"trace.dump", cmd_trace_dump, true},
    {"coverage.get", cmd_coverage_get, true},
    {"coverage.reset", cmd_coverage_reset, true},
    {"backtrace", cmd_backtrace, true},
    {"snapshot.save", cmd_snapshot_save, true},
    {"snapshot.load", cmd_snapshot_load, true},
    {"snapshot.list", cmd_snapshot_list, true},
    {"snapshot.delete", cmd_snapshot_delete, true},
    {"rewind.set", cmd_rewind_set, true},
    {"rewind.pop", cmd_rewind_pop, true},
    {"input.set", cmd_input_set, true},
    {"input.press", cmd_input_press, true},
    {"input.tap", cmd_input_tap, true},
    {"input.script", cmd_input_script, true},
    {"cpu.init", cmd_cpu_init, false},
    {"cpu.load", cmd_cpu_load, true},
    {"cpu.exec", cmd_cpu_exec, true},
    {"cpu.reset", cmd_cpu_reset, true},
    {"screen.capture", cmd_screen_capture, true},
    {"screen.palette", cmd_screen_palette, true},
    {"video.tilemap", cmd_video_tilemap, true},
    {"video.tiles", cmd_video_tiles, true},
    {"video.sprites", cmd_video_sprites, true},
};

static void respond(jval_t *id, bool ok)
{
    jw_t r;
    jw_init(&r, CTX.out.len + 256);
    jw_raw(&r, "{\"id\":");
    if (!id) jw_raw(&r, "null");
    else if (id->t == J_NUM) jw_fmt(&r, "%lld", (long long)id->num);
    else if (id->t == J_STR) jw_esc(&r, id->s);
    else jw_raw(&r, "null");
    jw_fmt(&r, ",\"ok\":%s,", ok ? "true" : "false");
    if (ok) jw_fmt(&r, "\"result\":%s}", CTX.out.buf ? CTX.out.buf : "null");
    else { jw_raw(&r, "\"error\":"); jw_esc(&r, CTX.err[0] ? CTX.err : "error"); jw_raw(&r, "}"); }
    fwrite(r.buf, 1, r.len, stdout);
    fputc('\n', stdout);
    fflush(stdout);
    jw_free(&r);
}

static bool handle_line(char *line)
{
    arena_t a;
    size_t arena_cap = strlen(line) * 16 + (1 << 20);
    arena_init(&a, arena_cap);
    char perr[160] = {0};
    jval_t *req = j_parse(&a, line, perr, sizeof(perr));
    if (!req || req->t != J_OBJ) {
        jw_init(&CTX.out, 64);
        CTX.err[0] = 0;
        set_err("parse_error: %s", perr[0] ? perr : "not a JSON object");
        respond(NULL, false);
        arena_free(&a);
        return true;
    }
    jval_t *id = (jval_t *)j_get(req, "id");
    const char *cmd = j_str(req, "cmd", NULL);
    const jval_t *params = j_get(req, "params");
    if (params && params->t != J_OBJ) params = NULL;
    if (!cmd) {
        jw_init(&CTX.out, 64); set_err("parse_error: missing 'cmd'");
        respond(id, false);
        arena_free(&a);
        return true;
    }
    bool quit = false;
    jw_init(&CTX.out, 1024);
    CTX.err[0] = 0;

    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
        if (strcmp(commands[i].name, cmd) == 0) {
            if (commands[i].needs_rom && !CTX.loaded) {
                set_err("no_rom: load_rom first");
                respond(id, false);
            }
            else {
                bool ok = commands[i].fn(params ? params : &(jval_t){.t = J_OBJ});
                respond(id, ok);
                if (strcmp(cmd, "quit") == 0) quit = true;
            }
            goto done;
        }
    }
    set_err("unknown_cmd: '%s'", cmd);
    respond(id, false);
done:
    jw_free(&CTX.out);
    CTX.out = (jw_t){0};
    arena_free(&a);
    return !quit;
}

static void default_bootrom_dir(void)
{
    const char *env = getenv("GBEMU_BOOTROM_DIR");
    if (env) { snprintf(CTX.bootrom_dir, sizeof(CTX.bootrom_dir), "%s", env); return; }
    /* relative to executable */
    char exe[1024];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1); /* Linux */
    if (n <= 0) {
        unsigned sz = sizeof(exe);
        extern int _NSGetExecutablePath(char *, unsigned *); /* macOS */
        if (_NSGetExecutablePath(exe, &sz) != 0) exe[0] = 0;
        n = strlen(exe);
    }
    if (n > 0) {
        exe[n] = 0;
        char *d = dirname(exe);
        snprintf(CTX.bootrom_dir, sizeof(CTX.bootrom_dir), "%s/../cli/bootroms", d);
        if (access(CTX.bootrom_dir, R_OK) == 0) return;
        snprintf(CTX.bootrom_dir, sizeof(CTX.bootrom_dir), "%s/bootroms", d);
        if (access(CTX.bootrom_dir, R_OK) == 0) return;
    }
    snprintf(CTX.bootrom_dir, sizeof(CTX.bootrom_dir), "cli/bootroms");
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    memset(&CTX, 0, sizeof(CTX));
    default_bootrom_dir();

    const char *script = NULL;
    bool builtin_dir_set = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--bootrom-dir") && i + 1 < argc) {
            snprintf(CTX.bootrom_dir, sizeof(CTX.bootrom_dir), "%s", argv[++i]);
            builtin_dir_set = true;
        }
        else if (!strcmp(argv[i], "--script") && i + 1 < argc) script = argv[++i];
        else if (!strcmp(argv[i], "--version")) { printf("gbemu " GBEMU_VERSION " (SameBoy " GB_VERSION ")\n"); return 0; }
        else if (!strcmp(argv[i], "--help")) {
            fprintf(stderr,
                "gbemu — agent-facing headless SameBoy server (JSON lines on stdin/stdout)\n"
                "  --bootrom-dir DIR   dir containing *_boot.bin (default: auto)\n"
                "  --script FILE       run commands from FILE (one JSON per line), then exit\n"
                "  --version\n");
            return 0;
        }
    }
    (void)builtin_dir_set;

    /* trace ring default capacity */
    CTX.trace.cap = 1 << 16;
    CTX.trace.ring = calloc(CTX.trace.cap, sizeof(trace_entry_t));

    FILE *in = stdin;
    if (script) {
        in = fopen(script, "r");
        if (!in) { fprintf(stderr, "cannot open script '%s'\n", script); return 1; }
    }

    char *line = NULL;
    size_t cap = 0;
    bool cont = true;
    while (cont && getline(&line, &cap, in) != -1) {
        /* strip trailing whitespace/newlines */
        size_t l = strlen(line);
        while (l && (line[l-1] == '\n' || line[l-1] == '\r' || line[l-1] == ' ')) line[--l] = 0;
        if (!l) continue;
        cont = handle_line(line);
    }
    free(line);
    if (script) fclose(in);

    core_dispose();
    return 0;
}
