#pragma once
#include <stdint.h>
#include <stddef.h>

/* Encodes RGBA8888 pixels into PNG using zlib. Returns malloc'd buffer or NULL. */
uint8_t *png_encode(const uint8_t *rgba, uint32_t w, uint32_t h, size_t *out_len);
