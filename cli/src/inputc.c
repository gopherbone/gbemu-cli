#include <string.h>
#include <Core/gb.h>
#include <Core/joypad.h>
#include "ctx.h"

static const struct { const char *name; GB_key_mask_t mask; } key_names[] = {
    {"right",  GB_KEY_RIGHT_MASK},
    {"left",   GB_KEY_LEFT_MASK},
    {"up",     GB_KEY_UP_MASK},
    {"down",   GB_KEY_DOWN_MASK},
    {"a",      GB_KEY_A_MASK},
    {"b",      GB_KEY_B_MASK},
    {"select", GB_KEY_SELECT_MASK},
    {"start",  GB_KEY_START_MASK},
};

uint8_t input_parse_keys(const jval_t *keys, bool *ok)
{
    *ok = true;
    if (!keys) return 0;
    if (keys->t == J_STR) {
        /* comma/space separated */
        uint8_t mask = 0;
        char tmp[128];
        snprintf(tmp, sizeof(tmp), "%s", keys->s);
        char *save = NULL;
        for (char *tok = strtok_r(tmp, ", ", &save); tok; tok = strtok_r(NULL, ", ", &save)) {
            bool found = false;
            for (size_t i = 0; i < sizeof(key_names) / sizeof(key_names[0]); i++)
                if (!strcmp(key_names[i].name, tok)) { mask |= key_names[i].mask; found = true; break; }
            if (!found) { *ok = false; return 0; }
        }
        return mask;
    }
    if (keys->t == J_ARR) {
        uint8_t mask = 0;
        for (unsigned i = 0; i < keys->n; i++) {
            if (keys->items[i]->t != J_STR) { *ok = false; return 0; }
            bool found = false;
            for (size_t j = 0; j < sizeof(key_names) / sizeof(key_names[0]); j++)
                if (!strcmp(key_names[j].name, keys->items[i]->s)) { mask |= key_names[j].mask; found = true; break; }
            if (!found) { *ok = false; return 0; }
        }
        return mask;
    }
    *ok = false;
    return 0;
}

static bool keys_from_params(const jval_t *p, uint8_t *mask)
{
    const jval_t *keys = j_get(p, "keys");
    if (!keys) { set_err("missing 'keys'"); return false; }
    bool ok;
    *mask = input_parse_keys(keys, &ok);
    if (!ok) { set_err("bad keys (use a,b,start,select,up,down,left,right)"); return false; }
    return true;
}

bool cmd_input_set(const jval_t *p)
{
    uint8_t mask;
    if (!keys_from_params(p, &mask)) return false;
    CTX.key_mask = mask;
    GB_set_key_mask(CTX.gb, mask);
    jw_fmt(&CTX.out, "{\"mask\":\"%02x\"}", mask);
    return true;
}

bool cmd_input_press(const jval_t *p)
{
    uint8_t mask;
    if (!keys_from_params(p, &mask)) return false;
    long frames = j_int(p, "frames", 4);
    if (frames < 1 || frames > 3600) { set_err("frames 1..3600"); return false; }
    uint8_t prev = CTX.key_mask;
    CTX.stop_pending = false;
    GB_set_key_mask(CTX.gb, mask);
    uint64_t ran = exec_run_frames((uint64_t)frames);
    if (j_bool(p, "release", true)) {
        GB_set_key_mask(CTX.gb, prev);
        CTX.key_mask = prev;
    }
    else CTX.key_mask = mask;
    jw_fmt(&CTX.out, "{\"ran_frames\":%llu,\"stopped\":%s,\"held_mask\":\"%02x\"}",
           (unsigned long long)ran, CTX.stop_pending ? "true" : "false", CTX.key_mask);
    return true;
}

bool cmd_input_tap(const jval_t *p)
{
    uint8_t mask;
    if (!keys_from_params(p, &mask)) return false;
    long press_frames = j_int(p, "press_frames", 2);
    long release_frames = j_int(p, "release_frames", 8);
    uint8_t prev = CTX.key_mask;
    CTX.stop_pending = false;
    GB_set_key_mask(CTX.gb, mask);
    uint64_t a = exec_run_frames((uint64_t)press_frames);
    GB_set_key_mask(CTX.gb, prev);
    uint64_t b = 0;
    if (!CTX.stop_pending) b = exec_run_frames((uint64_t)release_frames);
    CTX.key_mask = prev;
    jw_fmt(&CTX.out, "{\"ran_frames\":%llu,\"stopped\":%s}",
           (unsigned long long)(a + b), CTX.stop_pending ? "true" : "false");
    return true;
}

bool cmd_input_script(const jval_t *p)
{
    const jval_t *steps = j_arr(p, "steps");
    if (!steps) { set_err("missing 'steps' array of {keys,frames}"); return false; }
    if (steps->n > 256) { set_err("too many steps (max 256)"); return false; }
    CTX.script_len = steps->n;
    CTX.script_pos = 0;
    for (unsigned i = 0; i < steps->n; i++) {
        bool ok;
        CTX.script[i].mask = input_parse_keys(j_get(steps->items[i], "keys"), &ok);
        if (!ok) { set_err("bad keys in step %u", i); CTX.script_len = 0; return false; }
        long fr = j_int(steps->items[i], "frames", 1);
        if (fr < 1 || fr > 100000) { set_err("bad frames in step %u", i); CTX.script_len = 0; return false; }
        CTX.script[i].frames_left = (int)fr;
    }
    CTX.stop_pending = false;
    /* apply first step immediately; the frame runner advances the rest */
    GB_set_key_mask(CTX.gb, CTX.script[0].mask);
    uint64_t total = 0;
    for (unsigned i = 0; i < steps->n; i++) total += (uint64_t)CTX.script[i].frames_left;
    if (j_bool(p, "run", true)) {
        uint64_t ran = exec_run_frames(total);
        GB_set_key_mask(CTX.gb, CTX.key_mask);
        jw_fmt(&CTX.out, "{\"script_frames\":%llu,\"ran_frames\":%llu,\"stopped\":%s}",
               (unsigned long long)total, (unsigned long long)ran,
               CTX.stop_pending ? "true" : "false");
    }
    else {
        jw_fmt(&CTX.out, "{\"queued_frames\":%llu}", (unsigned long long)total);
    }
    return true;
}
