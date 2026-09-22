#include <string.h>
#include <stdlib.h>
#include <Core/gb.h>
#include <Core/rewind.h>
#include <Core/save_state.h>
#include "ctx.h"

static int slot_by_name(const char *name)
{
    for (int i = 0; i < MAX_SNAPSHOTS; i++)
        if (CTX.snaps[i].used && !strncmp(CTX.snaps[i].name, name, MAX_NAME)) return i;
    return -1;
}

bool cmd_snapshot_save(const jval_t *p)
{
    const char *name = j_str(p, "name", NULL);
    const char *path = j_str(p, "path", NULL);
    if (path) {
        if (GB_save_state(CTX.gb, path)) { set_err("failed to save state to '%s'", path); return false; }
        jw_fmt(&CTX.out, "{\"saved\":"); jw_esc(&CTX.out, path); jw_raw(&CTX.out, "}");
        return true;
    }
    int slot = name ? slot_by_name(name) : -1;
    if (slot < 0) for (int i = 0; i < MAX_SNAPSHOTS; i++) if (!CTX.snaps[i].used) { slot = i; break; }
    if (slot < 0) { set_err("no free snapshot slots (max %d)", MAX_SNAPSHOTS); return false; }
    snap_t *s = &CTX.snaps[slot];
    free(s->buf);
    size_t size = GB_get_save_state_size(CTX.gb);
    s->buf = malloc(size);
    if (!s->buf) { s->used = false; set_err("oom"); return false; }
    s->size = size;
    GB_save_state_to_buffer(CTX.gb, s->buf);
    s->used = true;
    snprintf(s->name, MAX_NAME, "%s", name ? name : "snapshot");
    jw_fmt(&CTX.out, "{\"slot\":%d,\"size\":%zu,\"name\":", slot, size);
    jw_esc(&CTX.out, s->name);
    jw_fmt(&CTX.out, ",\"pc\":\"%04x\",\"frame\":%llu}",
           GB_get_registers(CTX.gb)->pc, (unsigned long long)CTX.frame_count);
    return true;
}

bool cmd_snapshot_load(const jval_t *p)
{
    const char *name = j_str(p, "name", NULL);
    const char *path = j_str(p, "path", NULL);
    int r;
    if (path) {
        r = GB_load_state(CTX.gb, path);
        if (r) { set_err("failed to load state from '%s' (err %d)", path, r); return false; }
        jw_fmt(&CTX.out, "{\"loaded\":"); jw_esc(&CTX.out, path);
    }
    else {
        int slot = name ? slot_by_name(name) : -1;
        if (slot < 0) { set_err("no snapshot named '%s'", name ? name : ""); return false; }
        snap_t *s = &CTX.snaps[slot];
        r = GB_load_state_from_buffer(CTX.gb, s->buf, s->size);
        if (r) { set_err("state load failed (err %d)", r); return false; }
        jw_fmt(&CTX.out, "{\"loaded\":"); jw_esc(&CTX.out, s->name);
        jw_fmt(&CTX.out, ",\"slot\":%d", slot);
    }
    CTX.stop_pending = false;
    CTX.last_stop_valid = false;
    install_analysis_hooks();
    jw_fmt(&CTX.out, ",\"pc\":\"%04x\"}", GB_get_registers(CTX.gb)->pc);
    return true;
}

bool cmd_snapshot_list(const jval_t *p)
{
    (void)p;
    jw_t *w = &CTX.out;
    jw_raw(w, "{\"snapshots\":[");
    bool first = true;
    for (int i = 0; i < MAX_SNAPSHOTS; i++) {
        if (!CTX.snaps[i].used) continue;
        jw_fmt(w, "%s{\"slot\":%d,\"size\":%zu,\"name\":", first ? "" : ",", i, CTX.snaps[i].size);
        jw_esc(w, CTX.snaps[i].name);
        jw_raw(w, "}");
        first = false;
        bool dummy = false; (void)dummy;
    }
    jw_raw(w, "],\"mem_captures\":[");
    first = true;
    for (int i = 0; i < MAX_MEMSNAPS; i++) {
        if (!CTX.memsnaps[i].used) continue;
        jw_fmt(w, "%s{\"name\":", first ? "" : ",");
        jw_esc(w, CTX.memsnaps[i].name);
        jw_fmt(w, "}");
        first = false;
    }
    jw_raw(w, "]}");
    return true;
}

bool cmd_snapshot_delete(const jval_t *p)
{
    const char *name = j_str(p, "name", NULL);
    int slot = name ? slot_by_name(name) : -1;
    if (slot < 0) { set_err("no snapshot named '%s'", name ? name : ""); return false; }
    free(CTX.snaps[slot].buf);
    CTX.snaps[slot] = (snap_t){0};
    jw_raw(&CTX.out, "{\"deleted\":true}");
    return true;
}

bool cmd_rewind_set(const jval_t *p)
{
    double seconds = j_num(j_get(p, "seconds"), 0);
    if (seconds < 0) seconds = 0;
    if (seconds > 300) { set_err("seconds capped at 300"); return false; }
    CTX.rewind_len = seconds;
    GB_set_rewind_length(CTX.gb, seconds);
    jw_fmt(&CTX.out, "{\"rewind_seconds\":%g}", seconds);
    return true;
}

bool cmd_rewind_pop(const jval_t *p)
{
    long frames = j_int(p, "frames", 1);
    if (frames < 1 || frames > 3600) { set_err("frames out of range"); return false; }
    long popped = 0;
    for (long i = 0; i < frames; i++) {
        if (!GB_rewind_pop(CTX.gb)) break;
        popped++;
    }
    CTX.stop_pending = false;
    CTX.last_stop_valid = false;
    jw_fmt(&CTX.out, "{\"rewound\":%ld,\"pc\":\"%04x\",\"frame\":%llu}",
           popped, GB_get_registers(CTX.gb)->pc, (unsigned long long)CTX.frame_count);
    if (CTX.frame_count >= (uint64_t)popped) CTX.frame_count -= popped;
    return true;
}
