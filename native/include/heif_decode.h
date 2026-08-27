#pragma once

#include <stdint.h>

/// Internal C++-side entry points for the HEIF route. Not part of the shipped
/// C ABI — that is heif_api.h. Split so the ABI wrapper holds no libheif types
/// and the whole libheif dependency is confined to heif_decode.cpp.
int32_t heifProbePrimary(const char *path, uint32_t *width, uint32_t *height,
                         int32_t *orientation);

int32_t heifDecodePrimaryRgba(const char *path, int32_t max_dim,
                              uint8_t **out_rgba, int64_t *out_len,
                              uint32_t *out_width, uint32_t *out_height,
                              int32_t *out_orientation);
