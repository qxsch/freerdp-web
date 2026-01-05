/**
 * RFX Decode - YCbCr to BGRA conversion
 * Based on FreeRDP's rfx_decode.c (Apache 2.0 License)
 * 
 * Converts YCbCr coefficients to BGRA32 pixels
 */

#include "rfx_types.h"

/* YCbCr to RGB conversion constants (ITU-R BT.601) */
#define Y_OFFSET 128
#define CB_OFFSET 128
#define CR_OFFSET 128

/* Clamp value to 0-255 range */
static inline uint8_t clamp_byte(int val) {
    if (val < 0) return 0;
    if (val > 255) return 255;
    return (uint8_t)val;
}

/**
 * Convert YCbCr coefficients to BGRA32 pixels
 * 
 * @param yData   Y coefficients (64x64)
 * @param cbData  Cb coefficients (64x64)
 * @param crData  Cr coefficients (64x64)
 * @param dst     Output BGRA32 buffer (64x64x4 bytes)
 * @param dstStride Stride in bytes
 */
void rfx_ycbcr_to_bgra(const int16_t* yData, const int16_t* cbData, 
                       const int16_t* crData, uint8_t* dst, int dstStride) {
    int x, y;
    
    for (y = 0; y < RFX_TILE_SIZE; y++) {
        uint8_t* row = dst + y * dstStride;
        
        for (x = 0; x < RFX_TILE_SIZE; x++) {
            int idx = y * RFX_TILE_SIZE + x;
            
            /* Get YCbCr values with offset adjustment */
            int Y  = yData[idx] + Y_OFFSET;
            int Cb = cbData[idx];
            int Cr = crData[idx];
            
            /* BT.601 conversion:
             * R = Y + 1.402 * Cr
             * G = Y - 0.344136 * Cb - 0.714136 * Cr  
             * B = Y + 1.772 * Cb
             * 
             * Using fixed-point: multiply by 256 and shift
             */
            int R = Y + ((359 * Cr) >> 8);
            int G = Y - ((88 * Cb + 183 * Cr) >> 8);
            int B = Y + ((454 * Cb) >> 8);
            
            /* Write BGRA */
            row[x * 4 + 0] = clamp_byte(B);
            row[x * 4 + 1] = clamp_byte(G);
            row[x * 4 + 2] = clamp_byte(R);
            row[x * 4 + 3] = 255;  /* Alpha */
        }
    }
}

/**
 * Convert YCbCr to RGBA32 (for browser canvas which expects RGBA)
 */
void rfx_ycbcr_to_rgba(const int16_t* yData, const int16_t* cbData, 
                       const int16_t* crData, uint8_t* dst, int dstStride) {
    int x, y;
    
    for (y = 0; y < RFX_TILE_SIZE; y++) {
        uint8_t* row = dst + y * dstStride;
        
        for (x = 0; x < RFX_TILE_SIZE; x++) {
            int idx = y * RFX_TILE_SIZE + x;
            
            int Y  = yData[idx] + Y_OFFSET;
            int Cb = cbData[idx];
            int Cr = crData[idx];
            
            int R = Y + ((359 * Cr) >> 8);
            int G = Y - ((88 * Cb + 183 * Cr) >> 8);
            int B = Y + ((454 * Cb) >> 8);
            
            /* Write RGBA */
            row[x * 4 + 0] = clamp_byte(R);
            row[x * 4 + 1] = clamp_byte(G);
            row[x * 4 + 2] = clamp_byte(B);
            row[x * 4 + 3] = 255;
        }
    }
}

/**
 * Apply region offset to tile output
 * Copies a decoded 64x64 tile to destination at specified position
 */
void rfx_copy_tile_to_surface(const uint8_t* tile, int tileStride,
                              uint8_t* surface, int surfaceWidth,
                              int dstX, int dstY, int copyWidth, int copyHeight) {
    for (int y = 0; y < copyHeight; y++) {
        const uint8_t* src = tile + y * tileStride;
        uint8_t* dst = surface + (dstY + y) * surfaceWidth * 4 + dstX * 4;
        memcpy(dst, src, copyWidth * 4);
    }
}
