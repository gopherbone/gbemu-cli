#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
#include <Core/gb.h>
#include "json.h"

#define MAX_BREAKPOINTS 64
#define MAX_WATCHPOINTS 64
#define BP_PC  0   /* halt when PC == addr (legacy) */
#define BP_MEM 1   /* halt on memory access matching an AND-composed predicate */
#define BP_HITLOG_CAP 65536  /* shared hit-log ring across all mem bps */

typedef struct {
    uint32_t bp;       /* slot index of the breakpoint that matched */
    uint16_t pc;       /* accessing instruction's pc (mem hits) or bp addr (pc hits) */
    uint16_t addr;     /* access address (mem) */
    uint8_t  value;    /* data value */
    uint8_t  is_write; /* 1 = write, 0 = read */
    uint16_t effbank;  /* effective bank of the access */
} bp_hit_t;
#define MAX_SNAPSHOTS 32
#define MAX_MEMSNAPS 8
#define MAX_NAME 48

typedef struct {
    int id;
    bool used;
    bool enabled;
    int kind;          /* BP_PC or BP_MEM */
    uint16_t addr;     /* BP_PC: halt address. BP_MEM: range start (inclusive) */
    uint16_t end;      /* BP_MEM: range end (inclusive); == addr for single */
    int bank;          /* -1 = any; effective bank of the access (BP_MEM) or at PC (BP_PC) */
    char *cond;        /* optional expression, malloc'd; ANDed with the predicate */
    uint64_t hits;
    /* BP_MEM predicate fields (all AND-composed; unset = no constraint) */
    bool on_read, on_write;   /* access types that match */
    uint16_t mask;            /* (data & mask) */
    uint16_t value;           /* == value (value_given = false => any data) */
    bool value_given;
    bool pc_eq_given;  uint16_t pc_eq;   /* pc == pc_eq */
    bool pc_not_given; uint16_t pc_not;  /* pc != pc_not */
    bool pc_range_given; uint16_t pc_lo, pc_hi; /* pc in [lo,hi] */
    bool stop;         /* false = count hits only, never halt */
} bp_t;

typedef struct {
    int id;
    bool used;
    bool enabled;
    uint16_t addr, end; /* inclusive range */
    bool on_read, on_write;
    int bank;           /* -1 = any, only matched for ROM region */
    bool stop;          /* false = just log/trace access */
    int value;          /* -1 = any; if >=0, write wp fires only when this value written */
    char *cond;
    uint64_t hits;
} wp_t;

typedef struct {
    bool used;
    char name[MAX_NAME];
    uint8_t *buf;
    size_t size;
} snap_t;

typedef struct {
    bool used;
    char name[MAX_NAME];
    uint8_t *vram, *wram, *oam, *hram, *cart;
    size_t vram_size, wram_size, oam_size, hram_size, cart_size;
    uint16_t wram_bank, vram_bank, cart_bank;
} memsnap_t;

typedef struct {
    uint16_t pc;
    uint16_t bank;
    uint8_t opcode;
    uint8_t regs[8]; /* AF.. aligned: a,f,b,c,d,e,h,l — only with with_regs */
    bool has_regs;
    /* memory accesses (with_mem) — stored inline after entry? keep flat: */
    uint8_t mems[4][4]; /* type(r/w), addr hi, addr lo, value */
    uint8_t n_mems;
} trace_entry_t;

typedef struct {
    trace_entry_t *ring;
    uint32_t cap, head, count; /* head = next write pos */
    uint64_t total, dropped;   /* total entries seen, dropped (overwritten) */
    bool with_regs, with_mem;
    bool rom_only;
    uint16_t pc_lo, pc_hi;     /* optional PC filter window */
    bool pc_filter;
    bool active;
} trace_t;

typedef struct {
    uint8_t mask;
    int frames_left;
} input_step_t;

typedef struct {
    GB_gameboy_t *gb;
    bool loaded;
    char rom_path[1024];
    char bootrom_dir[1024];
    char title[32];
    uint64_t instr_count;   /* GB_run iterations since load */
    uint64_t frame_count;
    uint64_t seed;

    bp_t bps[MAX_BREAKPOINTS];
    wp_t wps[MAX_WATCHPOINTS];
    int next_bp_id, next_wp_id;

    snap_t snaps[MAX_SNAPSHOTS];
    memsnap_t memsnaps[MAX_MEMSNAPS];

    trace_t trace;

    /* coverage */
    uint8_t *cov_bits;    /* one bit per ROM byte of addressability per bank: [bank][0x4000 bits] */
    size_t cov_rom_size;  /* rom size in bytes */
    uint32_t cov_max_bytes;
    bool cov_active;
    uint64_t cov_executed; /* total executed instruction count while active */

    /* stop state, set by callbacks during run loops */
    bool stop_pending;
    char stop_reason[160];
    uint16_t last_stop_pc;
    bool last_stop_valid;
    bool probing;   /* set during safe reads/writes from agent commands (suppresses wp/trace) */
    /* armed conditions for run.until */
    bool until_addr_armed;
    uint16_t until_addr;
    bool until_expr_armed;
    char until_expr[160];

    /* input */
    uint8_t key_mask;            /* persistent mask (input.set) */
    input_step_t script[256];    /* queued script steps */
    unsigned script_len, script_pos;

    /* memory access capture for current instruction (filled by read/write cbs) */
    bool mem_cap_armed;
    uint16_t mem_cap_pc;
    uint16_t cur_instr_pc; /* executing instruction's pc, latched by exec_cb */
    uint8_t mem_cap[4][4];
    uint8_t mem_cap_n;

    /* watchpoint fired info */
    bool wp_fired;
    char wp_fired_msg[128];

    /* breakpoint fired info (hit-reason reporting on stop) */
    bool bp_fired;
    char bp_fired_msg[256];

    /* shared hit-log ring (break.log) — every mem-bp match appends */
    bp_hit_t *bp_hits;
    uint32_t bp_hits_head;   /* next write pos */
    uint32_t bp_hits_count;  /* valid entries (up to cap) */
    uint64_t bp_hits_total;  /* total matches ever (may exceed ring) */

    /* log capture for disasm etc. */
    jw_t log_cap;
    bool log_capturing;

    /* rewind */
    double rewind_len;

    /* sandbox mode (cpu.*) */
    bool sandbox;
    uint8_t sandbox_rom_storage[0x8000]; /* flat ROM buffer, Core holds pointer */

    /* cycle accounting (raw GB_run units; 2 units = 1 T-cycle at 4MHz) */
    uint64_t cycles_raw;

    jw_t out;       /* per-command result writer */
    char err[256];

    bool running_through_cmds; /* set while inside any run cmd (suppresses stray logs) */
} ctx_t;

extern ctx_t CTX;

/* shared helpers (main.c) */
void set_err(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
bool parse_u16(const jval_t *v, uint16_t *out); /* num, "$hex", "0x", dec, or expr/symbol */
bool parse_bank_range(const jval_t *params, uint16_t *addr, uint16_t *end, int *bank);
long j_addr_expr(const jval_t *params, const char *key, long dflt); /* evaluate addr-ish value */
void resp_send(jval_t *id, bool ok);
void out_bytes_hex(const uint8_t *data, size_t len);
void install_analysis_hooks(void);   /* decide which core callbacks must be armed */
bool need_exec_cb(void);
bool need_mem_cbs(void);

/* exec.c */
void exec_reset_state(void);
bool cmd_step(const jval_t *params);
bool cmd_run_frames(const jval_t *params);
bool cmd_run_until(const jval_t *params);
void emit_banks_fields(jw_t *w); /* live bank state; called by emit_stop_result */
bool cmd_break_log(const jval_t *params); /* dump the hit-log ring */
bool cmd_break_add(const jval_t *params);
bool cmd_break_list(const jval_t *params);
bool cmd_break_del(const jval_t *params);
bool cmd_break_clear(const jval_t *params);
bool cmd_watch_add(const jval_t *params);
bool cmd_watch_list(const jval_t *params);
bool cmd_watch_del(const jval_t *params);
bool cmd_watch_clear(const jval_t *params);
bool cmd_trace_start(const jval_t *params);
bool cmd_trace_stop(const jval_t *params);
bool cmd_trace_dump(const jval_t *params);
bool cmd_coverage_get(const jval_t *params);
bool cmd_coverage_reset(const jval_t *params);
bool cmd_backtrace(const jval_t *params);
uint64_t exec_run_frames(uint64_t n); /* shared runner; returns frames actually run */
uint64_t exec_run_instructions(uint64_t n);

/* mem.c */
bool cmd_regs_get(const jval_t *params);
bool cmd_regs_set(const jval_t *params);
bool cmd_eval(const jval_t *params);
bool cmd_disasm(const jval_t *params);
bool cmd_mem_read(const jval_t *params);
bool cmd_mem_write(const jval_t *params);
bool cmd_mem_regions(const jval_t *params);
bool cmd_mem_capture(const jval_t *params);
bool cmd_mem_diff(const jval_t *params);
bool cmd_mem_search(const jval_t *params);
bool cmd_mem_search_reset(const jval_t *params);
bool cmd_symbols_load(const jval_t *params);
bool cmd_symbols_clear(const jval_t *params);
bool cmd_symbol_resolve(const jval_t *params);

/* history.c */
bool cmd_snapshot_save(const jval_t *params);
bool cmd_snapshot_load(const jval_t *params);
bool cmd_snapshot_list(const jval_t *params);
bool cmd_snapshot_delete(const jval_t *params);
bool cmd_rewind_set(const jval_t *params);
bool cmd_rewind_pop(const jval_t *params);

/* video.c */
bool cmd_screen_capture(const jval_t *params);
bool cmd_screen_palette(const jval_t *params);
bool cmd_video_tilemap(const jval_t *params);
bool cmd_video_tiles(const jval_t *params);
bool cmd_video_sprites(const jval_t *params);
void video_on_load(void);

/* inputc.c */
bool cmd_input_set(const jval_t *params);
bool cmd_input_press(const jval_t *params);
bool cmd_input_tap(const jval_t *params);
bool cmd_input_script(const jval_t *params);
uint8_t input_parse_keys(const jval_t *keys, bool *ok);

/* sandbox.c */
bool cmd_cpu_init(const jval_t *params);
bool cmd_cpu_load(const jval_t *params);
bool cmd_cpu_exec(const jval_t *params);
bool cmd_cpu_reset(const jval_t *params);

/* main.c */
void core_setup_callbacks(void);
void core_dispose(void);
void free_analysis_state(void);
typedef struct { const char *name; GB_model_t model; const char *boot; } gbemu_model_t;
const gbemu_model_t *gbemu_find_model(const char *name);
