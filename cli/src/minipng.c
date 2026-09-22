#include "minipng.h"
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

static void put_u32be(uint8_t **p, uint32_t v)
{
    (*p)[0] = v >> 24; (*p)[1] = v >> 16; (*p)[2] = v >> 8; (*p)[3] = v;
    *p += 4;
}

static void put_chunk(uint8_t **p, const char *type, const uint8_t *data, uint32_t len)
{
    put_u32be(p, len);
    uint8_t *crc_start = *p;
    memcpy(*p, type, 4); *p += 4;
    if (data && len) { memcpy(*p, data, len); *p += len; }
    uint32_t crc = (uint32_t)crc32(crc32(0L, NULL, 0), crc_start, len + 4);
    put_u32be(p, crc);
}

uint8_t *png_encode(const uint8_t *rgba, uint32_t w, uint32_t h, size_t *out_len)
{
    size_t raw_len = (size_t)h * (1 + (size_t)w * 4);
    uint8_t *raw = malloc(raw_len);
    if (!raw) return NULL;
    for (uint32_t y = 0; y < h; y++) {
        uint8_t *row = raw + y * (1 + (size_t)w * 4);
        row[0] = 0; /* filter: none */
        memcpy(row + 1, rgba + (size_t)y * w * 4, (size_t)w * 4);
    }
    uLongf comp_bound = compressBound((uLong)raw_len);
    uint8_t *comp = malloc(comp_bound);
    if (!comp) { free(raw); return NULL; }
    if (compress2(comp, &comp_bound, raw, (uLong)raw_len, 9) != Z_OK) {
        free(raw); free(comp); return NULL;
    }
    free(raw);

    size_t total = 8 + (12 + 13) + (12 + comp_bound) + 12;
    uint8_t *out = malloc(total);
    if (!out) { free(comp); return NULL; }
    uint8_t *p = out;
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    memcpy(p, sig, 8); p += 8;

    uint8_t ihdr[13];
    uint8_t *q = ihdr;
    put_u32be(&q, w);
    put_u32be(&q, h);
    ihdr[8] = 8; /* bit depth */
    ihdr[9] = 6; /* RGBA */
    ihdr[10] = 0; /* compression */
    ihdr[11] = 0; /* filter */
    ihdr[12] = 0; /* interlace */
    put_chunk(&p, "IHDR", ihdr, 13);
    put_chunk(&p, "IDAT", comp, (uint32_t)comp_bound);
    put_chunk(&p, "IEND", NULL, 0);
    free(comp);
    *out_len = (size_t)(p - out);
    return out;
}
