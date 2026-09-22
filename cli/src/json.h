#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Minimal JSON parser (DOM over a single arena buffer) + writer. */

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } jtype_t;

typedef struct jval_s {
    jtype_t t;
    char *s;          /* J_STR */
    double num;       /* J_NUM */
    bool b;           /* J_BOOL */
    struct jval_s **items; /* J_ARR | J_OBJ */
    char **keys;      /* J_OBJ */
    unsigned n;
} jval_t;

typedef struct {
    char *buf;
    size_t cap, used;
} arena_t;

void arena_init(arena_t *a, size_t cap);
void arena_free(arena_t *a);
void *arena_alloc(arena_t *a, size_t size);

/* Parses s into arena-backed DOM. Returns NULL + err on failure. */
jval_t *j_parse(arena_t *a, const char *s, char *err, size_t errcap);

const jval_t *j_get(const jval_t *obj, const char *key);
double j_num(const jval_t *v, double dflt);
long long j_int(const jval_t *obj, const char *key, long long dflt);
/* Accepts J_NUM or J_STR, strings passed through as-is */
const char *j_str(const jval_t *obj, const char *key, const char *dflt);
bool j_bool(const jval_t *obj, const char *key, bool dflt);
const jval_t *j_arr(const jval_t *obj, const char *key);
bool j_has(const jval_t *obj, const char *key);

typedef struct {
    char *buf;
    size_t cap, len;
    bool oom;
} jw_t;

void jw_init(jw_t *w, size_t cap_hint);
void jw_free(jw_t *w);
void jw_raw(jw_t *w, const char *s);
void jw_rawn(jw_t *w, const char *s, size_t n);
void jw_fmt(jw_t *w, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void jw_esc(jw_t *w, const char *s); /* appends quoted+escaped string */
void jw_key(jw_t *w, const char *key); /* "key": */
void jw_hex(jw_t *w, const uint8_t *data, size_t len); /* appends quoted lowercase hex */
void jw_b64(jw_t *w, const uint8_t *data, size_t len); /* appends quoted base64 */
