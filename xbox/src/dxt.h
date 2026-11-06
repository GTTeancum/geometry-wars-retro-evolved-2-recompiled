// DXT5 (BC3) compression for converted textures (src/nv2a.cpp): 1 byte per
// pixel instead of 4, so the game's large textures keep their resolution in
// the memory a quarter-size A8R8G8B8 copy took. The NV2A decodes DXT5 itself
// (NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT45_A8R8G8B8); blocks are stored in
// row order, not swizzled.
#pragma once

#include <stdint.h>

// Compresses one 4x4 block of A8R8G8B8 pixels (row by row, 16 values) into
// 16 bytes.
void gw_dxt5_block(const uint32_t pixels[16], uint8_t out[16]);

// Decodes one block back to A8R8G8B8 (for tools/native_check).
void gw_dxt5_decode_block(const uint8_t in[16], uint32_t pixels[16]);
