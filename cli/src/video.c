#include <stdlib.h>
#include <string.h>
#include <Core/gb.h>
#include "ctx.h"
#include "agent_ext.h"
#include "minipng.h"

static bool lcd_on(void)
{
    return (GB_safe_read_memory(CTX.gb, 0xFF40) & 0x80) != 0;
}

static void screen_geometry(uint32_t *w, uint32_t *h, uint32_t *stride)
{
    /* SameBoy renders 160x144 (SGB border mode renders 256x224 with the game at offset 48,40). */
    *w = 160; *h = 144; *stride = 160;
    /* v1: SGB border not exposed; the 160x144 region is extracted if needed */
}

static const uint32_t *pixels_out(void)
{
    return (const uint32_t *)GB_get_pixels_output(CTX.gb);
}

bool cmd_screen_capture(const jval_t *p)
{
    uint32_t w, h, stride;
    screen_geometry(&w, &h, &stride);
    const uint32_t *pix = pixels_out();
    if (!pix) { set_err("no pixel output (run some frames first)"); return false; }
    long scale = j_int(p, "scale", 1);
    if (scale < 1 || scale > 4) { set_err("scale 1..4"); return false; }

    const char *fmt = j_str(p, "format", "png");

    /* convert to RGBA */
    uint32_t W = w * (uint32_t)scale, H = h * (uint32_t)scale;
    uint8_t *rgba = malloc((size_t)W * H * 4);
    if (!rgba) { set_err("oom"); return false; }
    for (long y = 0; y < (long)H; y++)
        for (long x = 0; x < (long)W; x++) {
            uint32_t v = pix[(y / scale) * stride + (x / scale)];
            uint8_t *o = rgba + ((size_t)y * W + (size_t)x) * 4;
            o[0] = v >> 16; o[1] = v >> 8; o[2] = v; o[3] = 0xFF;
        }

    size_t png_len = 0;
    uint8_t *png = png_encode(rgba, W, H, &png_len);
    free(rgba);
    if (!png) { set_err("png encode failed"); return false; }

    const char *path = j_str(p, "path", NULL);
    if (path) {
        FILE *f = fopen(path, "wb");
        if (!f) { free(png); set_err("cannot write '%s'", path); return false; }
        fwrite(png, 1, png_len, f);
        fclose(f);
        jw_fmt(&CTX.out, "{\"path\":");
        jw_esc(&CTX.out, path);
        jw_fmt(&CTX.out, ",\"bytes\":%zu,\"width\":%u,\"height\":%u,\"lcd_on\":%s}",
               png_len, W, H, lcd_on() ? "true" : "false");
        free(png);
    }
    else {
        jw_fmt(&CTX.out, "{\"width\":%u,\"height\":%u,\"lcd_on\":%s,\"png_b64\":",
               W, H, lcd_on() ? "true" : "false");
        jw_b64(&CTX.out, png, png_len);
        jw_raw(&CTX.out, "}");
        free(png);
    }
    (void)fmt;
    return true;
}

/* CGB 15-bit color → 8-bit RGB */
static uint32_t cgb_color(uint16_t c)
{
    uint8_t r = (c & 0x1F) * 255 / 31;
    uint8_t g = ((c >> 5) & 0x1F) * 255 / 31;
    uint8_t b = ((c >> 10) & 0x1F) * 255 / 31;
    return 0xFF000000 | (uint32_t)r << 16 | (uint32_t)g << 8 | b;
}

bool cmd_screen_palette(const jval_t *p)
{
    (void)p;
    jw_t *w = &CTX.out;
    if (GB_is_cgb_in_cgb_mode(CTX.gb)) {
        size_t size;
        const uint8_t *bgp = GB_get_direct_access(CTX.gb, GB_DIRECT_ACCESS_BGP, &size, NULL);
        const uint8_t *obp = GB_get_direct_access(CTX.gb, GB_DIRECT_ACCESS_OBP, &size, NULL);
        jw_raw(w, "{\"mode\":\"cgb\",\"bg_palettes\":[");
        for (int pal = 0; pal < 8; pal++) {
            jw_fmt(w, "%s[", pal ? "," : "");
            for (int c = 0; c < 4; c++) {
                uint16_t col = bgp[pal * 8 + c * 2] | bgp[pal * 8 + c * 2 + 1] << 8;
                jw_fmt(w, "%s\"%06x\"", c ? "," : "", cgb_color(col) & 0xFFFFFF);
            }
            jw_raw(w, "]");
        }
        jw_raw(w, "],\"obj_palettes\":[");
        for (int pal = 0; pal < 8; pal++) {
            jw_fmt(w, "%s[", pal ? "," : "");
            for (int c = 0; c < 4; c++) {
                uint16_t col = obp[pal * 8 + c * 2] | obp[pal * 8 + c * 2 + 1] << 8;
                jw_fmt(w, "%s\"%06x\"", c ? "," : "", cgb_color(col) & 0xFFFFFF);
            }
            jw_raw(w, "]");
        }
        jw_raw(w, "]}");
    }
    else {
        static const int dmg_lum[4] = {255, 170, 85, 0};
        uint8_t bgp = GB_safe_read_memory(CTX.gb, 0xFF47);
        uint8_t obp0 = GB_safe_read_memory(CTX.gb, 0xFF48);
        uint8_t obp1 = GB_safe_read_memory(CTX.gb, 0xFF49);
        jw_raw(w, "{\"mode\":\"dmg\",\"bgp\":\"");
        for (int i = 0; i < 4; i++) jw_fmt(w, "%s%06x", i ? "," : "", dmg_lum[(bgp >> (i * 2)) & 3] * 0x10101);
        jw_raw(w, "\",\"obp0\":\"");
        for (int i = 0; i < 4; i++) jw_fmt(w, "%s%06x", i ? "," : "", dmg_lum[(obp0 >> (i * 2)) & 3] * 0x10101);
        jw_raw(w, "\",\"obp1\":\"");
        for (int i = 0; i < 4; i++) jw_fmt(w, "%s%06x", i ? "," : "", dmg_lum[(obp1 >> (i * 2)) & 3] * 0x10101);
        jw_raw(w, "\"}");
    }
    return true;
}

/* ---------------- VRAM-derived dumps ---------------- */

static uint8_t vram_at(uint16_t offset, uint8_t bank)
{
    size_t size;
    uint8_t *v = GB_get_direct_access(CTX.gb, GB_DIRECT_ACCESS_VRAM, &size, NULL);
    size_t lin = (size_t)bank * 0x2000 + offset;
    return lin < size ? v[lin] : 0;
}

/* decode one 8x8 2bpp tile into 64 color indices (0-3) */
static void decode_tile(uint8_t tile_idx, uint8_t bank, bool tile_data_8000, uint8_t out[64])
{
    uint16_t base;
    if (tile_data_8000) base = (uint16_t)(tile_idx * 16);
    else base = (uint16_t)(0x1000 + (int8_t)tile_idx * 16);
    for (int y = 0; y < 8; y++) {
        uint8_t lo = vram_at(base + y * 2, bank);
        uint8_t hi = vram_at(base + y * 2 + 1, bank);
        for (int x = 0; x < 8; x++) {
            out[y * 8 + x] = ((hi >> (7 - x) & 1) << 1) | (lo >> (7 - x) & 1);
        }
    }
}

bool cmd_video_tiles(const jval_t *p)
{
    long start = j_int(p, "start", 0);
    long count = j_int(p, "count", 32);
    long bank = j_int(p, "bank", 0);
    if (start < 0 || start >= 384 || count < 1 || count > 96 || start + count > 384) {
        set_err("start/count out of range (0..383, count <= 96)");
        return false;
    }
    jw_t *w = &CTX.out;
    jw_fmt(w, "{\"bank\":%ld,\"start\":%ld,\"tiles\":[", bank, start);
    uint8_t px[64];
    static const char nib[] = "0123";
    for (long i = start; i < start + count; i++) {
        decode_tile((uint8_t)i, (uint8_t)bank, true, px);
        jw_fmt(w, "%s\"", i > start ? "," : "");
        for (int j = 0; j < 64; j++) jw_rawn(w, &nib[px[j] & 3], 1);
        jw_raw(w, "\"");
    }
    jw_raw(w, "]}");
    return true;
}

bool cmd_video_tilemap(const jval_t *p)
{
    const char *layer = j_str(p, "layer", "bg");
    uint8_t lcdc = GB_safe_read_memory(CTX.gb, 0xFF40);
    bool is_win = strcmp(layer, "win") == 0;
    uint16_t map_base;
    if (is_win) map_base = (lcdc & 0x40) ? 0x1C00 : 0x1800;
    else        map_base = (lcdc & 0x08) ? 0x1C00 : 0x1800;
    bool tile_data_8000 = (lcdc & 0x10) != 0;
    bool cgb = GB_is_cgb_in_cgb_mode(CTX.gb);
    uint8_t wx = GB_safe_read_memory(CTX.gb, 0xFF4B);
    uint8_t wy = GB_safe_read_memory(CTX.gb, 0xFF4A);

    jw_t *w = &CTX.out;
    jw_fmt(w, "{\"layer\":\"%s\",\"map_base\":\"%04x\",\"tile_data\":\"%s\",",
           is_win ? "win" : "bg", 0x8000 + map_base, tile_data_8000 ? "8000" : "8800");
    if (is_win) jw_fmt(w, "\"wx\":%u,\"wy\":%u,", wx, wy);
    jw_raw(w, "\"tiles\":[");
    for (int row = 0; row < 32; row++) {
        if (row) jw_raw(w, ",");
        jw_raw(w, "\"");
        for (int col = 0; col < 32; col++) {
            uint8_t t = vram_at(map_base + row * 32 + col, 0);
            jw_fmt(w, "%02x", t);
        }
        jw_raw(w, "\"");
    }
    jw_raw(w, "]");
    if (cgb) {
        jw_raw(w, ",\"attrs\":[");
        for (int row = 0; row < 32; row++) {
            if (row) jw_raw(w, ",");
            jw_raw(w, "\"");
            for (int col = 0; col < 32; col++) {
                uint8_t a = vram_at(map_base + row * 32 + col, 1);
                jw_fmt(w, "%02x", a);
            }
            jw_raw(w, "\"");
        }
        jw_raw(w, "]");
    }
    jw_raw(w, "}");
    return true;
}

bool cmd_video_sprites(const jval_t *p)
{
    (void)p;
    size_t size;
    const uint8_t *oam = GB_get_direct_access(CTX.gb, GB_DIRECT_ACCESS_OAM, &size, NULL);
    uint8_t lcdc = GB_safe_read_memory(CTX.gb, 0xFF40);
    bool tall = (lcdc & 0x04) != 0;
    bool cgb = GB_is_cgb_in_cgb_mode(CTX.gb);
    jw_t *w = &CTX.out;
    jw_fmt(w, "{\"tall_mode\":%s,\"sprites\":[", tall ? "true" : "false");
    bool first = true;
    for (int i = 0; i < 40; i++) {
        uint8_t y = oam[i * 4], x = oam[i * 4 + 1], tile = oam[i * 4 + 2], f = oam[i * 4 + 3];
        if (y == 0 && x == 0) continue;
        jw_fmt(w, "%s{\"i\":%d,\"x\":%d,\"y\":%d,\"tile\":\"%02x\"",
               first ? "" : ",", i, (int)x - 8, (int)y - 16, tile);
        first = false;
        jw_fmt(w, ",\"behind_bg\":%s,\"x_flip\":%s,\"y_flip\":%s,\"dmg_palette\":%u",
               (f & 0x80) ? "true" : "false", (f & 0x40) ? "true" : "false",
               (f & 0x20) ? "true" : "false", (f >> 4) & 1);
        if (cgb) jw_fmt(w, ",\"cgb_palette\":%u,\"vram_bank\":%u", f & 7, (f >> 3) & 1);
        jw_raw(w, "}");
    }
    jw_raw(w, "]}");
    return true;
}

void video_on_load(void) { }
