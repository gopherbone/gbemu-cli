#include "json.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <ctype.h>

void arena_init(arena_t *a, size_t cap)
{
    a->buf = malloc(cap);
    a->cap = a->buf ? cap : 0;
    a->used = 0;
}

void arena_free(arena_t *a)
{
    free(a->buf);
    a->buf = NULL;
    a->cap = a->used = 0;
}

void *arena_alloc(arena_t *a, size_t size)
{
    size = (size + 7) & ~7;
    if (a->used + size > a->cap) return NULL;
    void *ret = a->buf + a->used;
    a->used += size;
    return ret;
}

typedef struct {
    const char *p;
    arena_t *a;
    char *err;
    size_t errcap;
} pstate_t;

static void p_err(pstate_t *st, const char *msg)
{
    if (st->err && !st->err[0]) {
        snprintf(st->err, st->errcap, "JSON parse error near '%.16s': %s", st->p, msg);
    }
}

static void skip_ws(pstate_t *st)
{
    while (*st->p == ' ' || *st->p == '\t' || *st->p == '\n' || *st->p == '\r') st->p++;
}

static jval_t *p_new(pstate_t *st, jtype_t t)
{
    jval_t *v = arena_alloc(st->a, sizeof(*v));
    if (!v) { p_err(st, "out of arena memory"); return NULL; }
    memset(v, 0, sizeof(*v));
    v->t = t;
    return v;
}

static jval_t *p_value(pstate_t *st);

static jval_t *p_string(pstate_t *st)
{
    st->p++; /* skip " */
    /* first pass: length */
    const char *q = st->p;
    size_t len = 0;
    while (*q && *q != '"') {
        if (*q == '\\') { q++; if (!*q) break; }
        q++; len++;
    }
    if (*q != '"') { p_err(st, "unterminated string"); return NULL; }
    jval_t *v = p_new(st, J_STR);
    if (!v) return NULL;
    v->s = arena_alloc(st->a, len + 1);
    if (!v->s) { p_err(st, "out of arena memory"); return NULL; }
    char *out = v->s;
    while (*st->p && *st->p != '"') {
        if (*st->p == '\\') {
            st->p++;
            switch (*st->p) {
                case 'n': *out++ = '\n'; break;
                case 't': *out++ = '\t'; break;
                case 'r': *out++ = '\r'; break;
                case 'b': *out++ = '\b'; break;
                case 'f': *out++ = '\f'; break;
                case 'u':
                    /* keep \uXXXX as-is (agent protocol is ASCII-safe) */
                    st->p++;
                    {
                        unsigned cp = 0;
                        for (int i = 0; i < 4 && isxdigit((unsigned char)st->p[i]); i++) {
                            cp = cp * 16 + (unsigned)(st->p[i] <= '9' ? st->p[i] - '0' :
                                                      (st->p[i] | 32) - 'a' + 10);
                        }
                        st->p += 3;
                        if (cp < 0x80) *out++ = (char)cp;
                        else if (cp < 0x800) { *out++ = (char)(0xC0 | cp >> 6); *out++ = (char)(0x80 | (cp & 63)); }
                        else { *out++ = (char)(0xE0 | cp >> 12); *out++ = (char)(0x80 | (cp >> 6 & 63)); *out++ = (char)(0x80 | (cp & 63)); }
                    }
                    st->p--;
                    break;
                default: *out++ = *st->p; break;
            }
        }
        else *out++ = *st->p;
        st->p++;
    }
    *out = 0;
    st->p++; /* skip " */
    return v;
}

static jval_t *p_number(pstate_t *st)
{
    char *end = NULL;
    double d = strtod(st->p, &end);
    if (end == st->p) { p_err(st, "invalid number"); return NULL; }
    st->p = end;
    jval_t *v = p_new(st, J_NUM);
    if (v) v->num = d;
    return v;
}

static bool p_lit(pstate_t *st, const char *lit)
{
    size_t n = strlen(lit);
    if (strncmp(st->p, lit, n) == 0) { st->p += n; return true; }
    return false;
}

static jval_t *p_container(pstate_t *st, bool obj)
{
    jval_t *v = p_new(st, obj ? J_OBJ : J_ARR);
    if (!v) return NULL;
    st->p++;
    unsigned cap = 8;
    v->items = arena_alloc(st->a, cap * sizeof(*v->items));
    if (obj) v->keys = arena_alloc(st->a, cap * sizeof(*v->keys));
    if (!v->items || (obj && !v->keys)) { p_err(st, "out of arena memory"); return NULL; }
    skip_ws(st);
    if (*st->p == (obj ? '}' : ']')) { st->p++; return v; }
    while (1) {
        skip_ws(st);
        if (obj) {
            if (*st->p != '"') { p_err(st, "expected string key"); return NULL; }
            jval_t *k = p_string(st);
            if (!k) return NULL;
            skip_ws(st);
            if (*st->p != ':') { p_err(st, "expected ':'"); return NULL; }
            st->p++;
            v->keys[v->n] = k->s;
        }
        skip_ws(st);
        jval_t *item = p_value(st);
        if (!item) return NULL;
        if (v->n == cap) {
            unsigned ncap = cap * 2;
            jval_t **ni = arena_alloc(st->a, ncap * sizeof(*ni));
            if (!ni) { p_err(st, "out of arena memory"); return NULL; }
            memcpy(ni, v->items, cap * sizeof(*ni));
            v->items = ni;
            if (obj) {
                char **nk = arena_alloc(st->a, ncap * sizeof(*nk));
                if (!nk) { p_err(st, "out of arena memory"); return NULL; }
                memcpy(nk, v->keys, cap * sizeof(*nk));
                v->keys = nk;
            }
            cap = ncap;
        }
        v->items[v->n++] = item;
        skip_ws(st);
        if (*st->p == ',') { st->p++; continue; }
        if (*st->p == (obj ? '}' : ']')) { st->p++; return v; }
        p_err(st, "expected ',' or close");
        return NULL;
    }
}

static jval_t *p_value(pstate_t *st)
{
    skip_ws(st);
    switch (*st->p) {
        case '"': return p_string(st);
        case '{': return p_container(st, true);
        case '[': return p_container(st, false);
        case 't': if (p_lit(st, "true")) { jval_t *v = p_new(st, J_BOOL); if (v) v->b = true; return v; } break;
        case 'f': if (p_lit(st, "false")) { jval_t *v = p_new(st, J_BOOL); return v; } break;
        case 'n': if (p_lit(st, "null")) return p_new(st, J_NULL); break;
        default:
            if (*st->p == '-' || isdigit((unsigned char)*st->p)) return p_number(st);
            break;
    }
    p_err(st, "unexpected token");
    return NULL;
}

jval_t *j_parse(arena_t *a, const char *s, char *err, size_t errcap)
{
    pstate_t st = {.p = s, .a = a, .err = err, .errcap = errcap};
    if (err) err[0] = 0;
    jval_t *v = p_value(&st);
    if (!v) return NULL;
    skip_ws(&st);
    if (*st.p) { p_err(&st, "trailing data"); return NULL; }
    return v;
}

const jval_t *j_get(const jval_t *obj, const char *key)
{
    if (!obj || obj->t != J_OBJ) return NULL;
    for (unsigned i = 0; i < obj->n; i++)
        if (strcmp(obj->keys[i], key) == 0) return obj->items[i];
    return NULL;
}

bool j_has(const jval_t *obj, const char *key) { return j_get(obj, key) != NULL; }

double j_num(const jval_t *v, double dflt)
{
    if (!v) return dflt;
    if (v->t == J_NUM) return v->num;
    if (v->t == J_STR) return strtod(v->s, NULL);
    return dflt;
}

long long j_int(const jval_t *obj, const char *key, long long dflt)
{
    return (long long)j_num(j_get(obj, key), (double)dflt);
}

const char *j_str(const jval_t *obj, const char *key, const char *dflt)
{
    const jval_t *v = j_get(obj, key);
    return (v && v->t == J_STR) ? v->s : dflt;
}

bool j_bool(const jval_t *obj, const char *key, bool dflt)
{
    const jval_t *v = j_get(obj, key);
    if (!v) return dflt;
    if (v->t == J_BOOL) return v->b;
    if (v->t == J_NUM) return v->num != 0;
    return dflt;
}

const jval_t *j_arr(const jval_t *obj, const char *key)
{
    const jval_t *v = j_get(obj, key);
    return (v && v->t == J_ARR) ? v : NULL;
}

/* ---------------- writer ---------------- */

static void jw_grow(jw_t *w, size_t need)
{
    if (w->oom) return;
    if (w->len + need + 1 > w->cap) {
        size_t ncap = w->cap * 2;
        while (ncap < w->len + need + 1) ncap *= 2;
        char *nb = realloc(w->buf, ncap);
        if (!nb) { w->oom = true; return; }
        w->buf = nb;
        w->cap = ncap;
    }
}

void jw_init(jw_t *w, size_t cap_hint)
{
    w->cap = cap_hint < 64 ? 64 : cap_hint;
    w->buf = malloc(w->cap);
    w->len = 0;
    w->oom = !w->buf;
}

void jw_free(jw_t *w) { free(w->buf); w->buf = NULL; w->cap = w->len = 0; }

void jw_rawn(jw_t *w, const char *s, size_t n)
{
    jw_grow(w, n);
    if (w->oom) return;
    memcpy(w->buf + w->len, s, n);
    w->len += n;
    w->buf[w->len] = 0;
}

void jw_raw(jw_t *w, const char *s) { jw_rawn(w, s, strlen(s)); }

void jw_fmt(jw_t *w, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    va_list copy;
    va_copy(copy, args);
    int need = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (need < 0) { va_end(args); return; }
    jw_grow(w, (size_t)need);
    if (w->oom) { va_end(args); return; }
    vsnprintf(w->buf + w->len, w->cap - w->len, fmt, args);
    va_end(args);
    w->len += (size_t)need;
}

void jw_esc(jw_t *w, const char *s)
{
    jw_raw(w, "\"");
    if (s) {
        for (; *s; s++) {
            unsigned char c = (unsigned char)*s;
            switch (c) {
                case '"': jw_raw(w, "\\\""); break;
                case '\\': jw_raw(w, "\\\\"); break;
                case '\n': jw_raw(w, "\\n"); break;
                case '\r': jw_raw(w, "\\r"); break;
                case '\t': jw_raw(w, "\\t"); break;
                default:
                    if (c < 0x20) jw_fmt(w, "\\u%04x", c);
                    else jw_rawn(w, (const char *)s, 1);
            }
        }
    }
    jw_raw(w, "\"");
}

void jw_key(jw_t *w, const char *key)
{
    jw_esc(w, key);
    jw_raw(w, ":");
}

void jw_hex(jw_t *w, const uint8_t *data, size_t len)
{
    static const char hexdig[] = "0123456789abcdef";
    jw_grow(w, len * 2 + 2);
    if (w->oom) return;
    w->buf[w->len++] = '"';
    for (size_t i = 0; i < len; i++) {
        w->buf[w->len++] = hexdig[data[i] >> 4];
        w->buf[w->len++] = hexdig[data[i] & 15];
    }
    w->buf[w->len++] = '"';
    w->buf[w->len] = 0;
}

static const char b64tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void jw_b64(jw_t *w, const uint8_t *data, size_t len)
{
    jw_grow(w, (len + 2) / 3 * 4 + 3);
    if (w->oom) return;
    w->buf[w->len++] = '"';
    size_t i;
    for (i = 0; i + 3 <= len; i += 3) {
        uint32_t v = data[i] << 16 | data[i+1] << 8 | data[i+2];
        w->buf[w->len++] = b64tab[v >> 18 & 63];
        w->buf[w->len++] = b64tab[v >> 12 & 63];
        w->buf[w->len++] = b64tab[v >> 6 & 63];
        w->buf[w->len++] = b64tab[v & 63];
    }
    if (len - i == 1) {
        uint32_t v = data[i] << 16;
        w->buf[w->len++] = b64tab[v >> 18 & 63];
        w->buf[w->len++] = b64tab[v >> 12 & 63];
        w->buf[w->len++] = '=';
        w->buf[w->len++] = '=';
    }
    else if (len - i == 2) {
        uint32_t v = data[i] << 16 | data[i+1] << 8;
        w->buf[w->len++] = b64tab[v >> 18 & 63];
        w->buf[w->len++] = b64tab[v >> 12 & 63];
        w->buf[w->len++] = b64tab[v >> 6 & 63];
        w->buf[w->len++] = '=';
    }
    w->buf[w->len++] = '"';
    w->buf[w->len] = 0;
}
