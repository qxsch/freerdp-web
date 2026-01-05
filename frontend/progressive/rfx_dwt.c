/**
 * DWT (Discrete Wavelet Transform) Inverse Transform
 * Based on FreeRDP's rfx_dwt.c (Apache 2.0 License)
 * 
 * Performs 2D inverse DWT on 64x64 tiles to reconstruct image data.
 */

#include "rfx_types.h"

/* DWT coefficients for 5/3 wavelet (used by RFX) */
#define DWT_ALPHA (-1)
#define DWT_BETA  (1)

/* Temporary buffers for row/column operations */
static int16_t dwt_temp[RFX_TILE_SIZE * 2];

/**
 * 1D inverse DWT lifting step
 * Interleaves and processes low/high subbands
 */
static void dwt_1d_decode(int16_t* dst, const int16_t* low, const int16_t* high, 
                          int count, int shift) {
    int i;
    int half = count / 2;
    
    /* Interleave low and high into dst */
    for (i = 0; i < half; i++) {
        dst[i * 2] = low[i] << shift;
        dst[i * 2 + 1] = high[i] << shift;
    }
    
    /* Inverse lifting: undo the predict step */
    for (i = 1; i < count - 1; i += 2) {
        dst[i] -= (dst[i - 1] + dst[i + 1] + 1) >> 1;
    }
    if (count > 1) {
        dst[count - 1] -= dst[count - 2];
    }
    
    /* Inverse lifting: undo the update step */
    dst[0] += (dst[1] + 1) >> 1;
    for (i = 2; i < count; i += 2) {
        dst[i] += (dst[i - 1] + dst[i + 1] + 2) >> 2;
    }
}

/**
 * 1D inverse DWT for a single row or column
 */
static void dwt_decode_row(int16_t* data, int length, int stride, int shift) {
    int half = length / 2;
    
    /* Copy to temp buffer separating low and high */
    for (int i = 0; i < half; i++) {
        dwt_temp[i] = data[i * stride];           /* Low subband */
        dwt_temp[half + i] = data[(half + i) * stride]; /* High subband */
    }
    
    /* Perform inverse DWT */
    int16_t result[RFX_TILE_SIZE];
    dwt_1d_decode(result, dwt_temp, dwt_temp + half, length, shift);
    
    /* Copy back */
    for (int i = 0; i < length; i++) {
        data[i * stride] = result[i];
    }
}

/**
 * 2D inverse DWT on subbands
 * Layout of 64x64 coefficients after 3-level decomposition:
 * 
 *  LL3 | LH3 | LH2         | LH1
 *  ----+-----+-------------+----------------
 *  HL3 | HH3 |             |
 *  ---------+             |
 *  HL2      |             |
 *  ---------+-------------+
 *  HL1                    |
 *  -----------------------+----------------
 *                HH1
 */
static void dwt_2d_decode_level(int16_t* data, int size, int level) {
    int subsize = size >> level;
    int half = subsize / 2;
    
    /* Apply inverse DWT to rows */
    for (int y = 0; y < subsize; y++) {
        dwt_decode_row(&data[y * size], subsize, 1, 0);
    }
    
    /* Apply inverse DWT to columns */
    for (int x = 0; x < subsize; x++) {
        dwt_decode_row(&data[x], subsize, size, 0);
    }
}

/**
 * Full 2D inverse DWT (3 levels)
 * Reconstructs 64x64 tile from wavelet coefficients
 */
void rfx_dwt_decode(int16_t* buffer, int size) {
    /* Level 3 (8x8 -> 16x16) */
    dwt_2d_decode_level(buffer, size, 2);
    
    /* Level 2 (16x16 -> 32x32) */
    dwt_2d_decode_level(buffer, size, 1);
    
    /* Level 1 (32x32 -> 64x64) */
    dwt_2d_decode_level(buffer, size, 0);
}

/**
 * Apply dequantization to coefficients
 * Each subband has different quantization factor
 */
void rfx_dequantize(int16_t* buffer, const RfxComponentCodecQuant* quant) {
    int x, y;
    int shift;
    
    /* LL3: top-left 8x8 */
    shift = quant->LL3 - 6;
    if (shift > 0) {
        for (y = 0; y < 8; y++) {
            for (x = 0; x < 8; x++) {
                buffer[y * 64 + x] <<= shift;
            }
        }
    }
    
    /* LH3: top-right 8x8 of top-left 16x16 */
    shift = quant->LH3 - 6;
    if (shift > 0) {
        for (y = 0; y < 8; y++) {
            for (x = 8; x < 16; x++) {
                buffer[y * 64 + x] <<= shift;
            }
        }
    }
    
    /* HL3: bottom-left 8x8 of top-left 16x16 */
    shift = quant->HL3 - 6;
    if (shift > 0) {
        for (y = 8; y < 16; y++) {
            for (x = 0; x < 8; x++) {
                buffer[y * 64 + x] <<= shift;
            }
        }
    }
    
    /* HH3: bottom-right 8x8 of top-left 16x16 */
    shift = quant->HH3 - 6;
    if (shift > 0) {
        for (y = 8; y < 16; y++) {
            for (x = 8; x < 16; x++) {
                buffer[y * 64 + x] <<= shift;
            }
        }
    }
    
    /* LH2: top-right 16x16 of top-left 32x32 */
    shift = quant->LH2 - 6;
    if (shift > 0) {
        for (y = 0; y < 16; y++) {
            for (x = 16; x < 32; x++) {
                buffer[y * 64 + x] <<= shift;
            }
        }
    }
    
    /* HL2: bottom-left 16x16 of top-left 32x32 */
    shift = quant->HL2 - 6;
    if (shift > 0) {
        for (y = 16; y < 32; y++) {
            for (x = 0; x < 16; x++) {
                buffer[y * 64 + x] <<= shift;
            }
        }
    }
    
    /* HH2: bottom-right 16x16 of top-left 32x32 */
    shift = quant->HH2 - 6;
    if (shift > 0) {
        for (y = 16; y < 32; y++) {
            for (x = 16; x < 32; x++) {
                buffer[y * 64 + x] <<= shift;
            }
        }
    }
    
    /* LH1: top-right 32x32 */
    shift = quant->LH1 - 6;
    if (shift > 0) {
        for (y = 0; y < 32; y++) {
            for (x = 32; x < 64; x++) {
                buffer[y * 64 + x] <<= shift;
            }
        }
    }
    
    /* HL1: bottom-left 32x32 */
    shift = quant->HL1 - 6;
    if (shift > 0) {
        for (y = 32; y < 64; y++) {
            for (x = 0; x < 32; x++) {
                buffer[y * 64 + x] <<= shift;
            }
        }
    }
    
    /* HH1: bottom-right 32x32 */
    shift = quant->HH1 - 6;
    if (shift > 0) {
        for (y = 32; y < 64; y++) {
            for (x = 32; x < 64; x++) {
                buffer[y * 64 + x] <<= shift;
            }
        }
    }
}
