/**
 * Progressive Codec WASM - Main Entry Point
 * Standalone progressive RFX decoder for WebAssembly
 * 
 * Based on FreeRDP's progressive codec (Apache 2.0 License)
 * Optimized for browser execution via Emscripten with pthread support
 */

#define BUILD_VERSION "__BUILD_TIME__"

#include "rfx_types.h"
#include <emscripten.h>
#include <pthread.h>
#include <stdio.h>

/* External functions from other modules */
extern int rfx_rlgr_decode(const uint8_t* input, size_t inputSize,
                           int16_t* output, size_t outputSize);
extern int rfx_srl_decode(const uint8_t* input, size_t inputSize,
                          int16_t* current, int8_t* sign,
                          size_t coeffCount, int bitPos);
extern void rfx_differential_decode(int16_t* buffer, size_t size);
extern void rfx_dwt_decode(int16_t* buffer, int size);
extern void rfx_dequantize(int16_t* buffer, const RfxComponentCodecQuant* quant);
extern void rfx_dequantize_progressive(int16_t* buffer, 
                                        const RfxComponentCodecQuant* quant,
                                        const RfxComponentCodecQuant* progQuant);
extern void rfx_ycbcr_to_rgba(const int16_t* yData, const int16_t* cbData,
                              const int16_t* crData, uint8_t* dst, int dstStride);

/* Tile pixel buffer size */
#define TILE_PIXELS (RFX_TILE_SIZE * RFX_TILE_SIZE)
#define TILE_BYTES (TILE_PIXELS * 4)

/* Thread pool configuration */
#define MAX_WORKER_THREADS 4
#define MAX_PENDING_TILES 1024

/* Forward declarations */
void prog_free(ProgressiveContext* ctx);
void prog_delete_surface(ProgressiveContext* ctx, uint16_t surfaceId);

/* Thread-local decode buffers */
typedef struct {
    int16_t* yBuffer;
    int16_t* cbBuffer;
    int16_t* crBuffer;
    int16_t* rlgrBuffer;
    size_t rlgrBufferSize;
    bool initialized;
} ThreadLocalBuffers;

/* Per-thread storage */
static __thread ThreadLocalBuffers tls_buffers = { NULL, NULL, NULL, NULL, 0, false };

/* Pending tile work item for parallel decode */
typedef struct {
    ProgressiveContext* ctx;
    RfxSurface* surface;
    const uint8_t* data;
    size_t size;
    uint16_t blockType;
} TileWorkItem;

/* Work queue for parallel processing */
static TileWorkItem pending_tiles[MAX_PENDING_TILES];
static int pending_count = 0;
static pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;

/**
 * Get or initialize thread-local decode buffers
 */
static ThreadLocalBuffers* get_thread_buffers(void) {
    if (!tls_buffers.initialized) {
        tls_buffers.yBuffer = (int16_t*)calloc(TILE_PIXELS, sizeof(int16_t));
        tls_buffers.cbBuffer = (int16_t*)calloc(TILE_PIXELS, sizeof(int16_t));
        tls_buffers.crBuffer = (int16_t*)calloc(TILE_PIXELS, sizeof(int16_t));
        tls_buffers.rlgrBufferSize = TILE_PIXELS * 2;
        tls_buffers.rlgrBuffer = (int16_t*)calloc(tls_buffers.rlgrBufferSize, sizeof(int16_t));
        tls_buffers.initialized = true;
    }
    return &tls_buffers;
}

/**
 * Create new progressive decoder context
 */
EMSCRIPTEN_KEEPALIVE
ProgressiveContext* prog_create(void) {
    printf("[PROG-WASM] ===== BUILD %s =====\n", BUILD_VERSION);
    
    ProgressiveContext* ctx = (ProgressiveContext*)calloc(1, sizeof(ProgressiveContext));
    if (!ctx) return NULL;
    
    /* Allocate temporary decode buffers */
    ctx->yBuffer = (int16_t*)calloc(TILE_PIXELS, sizeof(int16_t));
    ctx->cbBuffer = (int16_t*)calloc(TILE_PIXELS, sizeof(int16_t));
    ctx->crBuffer = (int16_t*)calloc(TILE_PIXELS, sizeof(int16_t));
    
    ctx->rlgrBufferSize = TILE_PIXELS * 2;
    ctx->rlgrBuffer = (int16_t*)calloc(ctx->rlgrBufferSize, sizeof(int16_t));
    
    if (!ctx->yBuffer || !ctx->cbBuffer || !ctx->crBuffer || !ctx->rlgrBuffer) {
        prog_free(ctx);
        return NULL;
    }
    
    return ctx;
}

/**
 * Free progressive decoder context
 */
EMSCRIPTEN_KEEPALIVE
void prog_free(ProgressiveContext* ctx) {
    if (!ctx) return;
    
    /* Free all surfaces */
    for (int i = 0; i < RFX_MAX_SURFACES; i++) {
        if (ctx->surfaces[i]) {
            prog_delete_surface(ctx, i);
        }
    }
    
    free(ctx->yBuffer);
    free(ctx->cbBuffer);
    free(ctx->crBuffer);
    free(ctx->rlgrBuffer);
    free(ctx);
}

/**
 * Allocate a tile with all its buffers
 */
static RfxTile* alloc_tile(uint16_t xIdx, uint16_t yIdx) {
    RfxTile* tile = (RfxTile*)calloc(1, sizeof(RfxTile));
    if (!tile) return NULL;
    
    tile->xIdx = xIdx;
    tile->yIdx = yIdx;
    tile->x = xIdx * RFX_TILE_SIZE;
    tile->y = yIdx * RFX_TILE_SIZE;
    tile->pass = 0;
    
    /* Allocate pixel data */
    tile->data = (uint8_t*)calloc(TILE_BYTES, 1);
    
    /* Allocate coefficient buffers */
    tile->yData = (int16_t*)calloc(TILE_PIXELS, sizeof(int16_t));
    tile->cbData = (int16_t*)calloc(TILE_PIXELS, sizeof(int16_t));
    tile->crData = (int16_t*)calloc(TILE_PIXELS, sizeof(int16_t));
    
    /* Allocate sign buffer for progressive refinement */
    tile->sign = (int8_t*)calloc(TILE_PIXELS * 3, sizeof(int8_t));
    
    if (!tile->data || !tile->yData || !tile->cbData || !tile->crData || !tile->sign) {
        free(tile->data);
        free(tile->yData);
        free(tile->cbData);
        free(tile->crData);
        free(tile->sign);
        free(tile);
        return NULL;
    }
    
    return tile;
}

/**
 * Free a tile
 */
static void free_tile(RfxTile* tile) {
    if (!tile) return;
    free(tile->data);
    free(tile->yData);
    free(tile->cbData);
    free(tile->crData);
    free(tile->sign);
    free(tile);
}

/**
 * Create surface context for progressive decoding
 */
EMSCRIPTEN_KEEPALIVE
int prog_create_surface(ProgressiveContext* ctx, uint16_t surfaceId, 
                        uint32_t width, uint32_t height) {
    if (!ctx || surfaceId >= RFX_MAX_SURFACES) return -1;
    
    /* Delete existing surface if present */
    if (ctx->surfaces[surfaceId]) {
        prog_delete_surface(ctx, surfaceId);
    }
    
    RfxSurface* surface = (RfxSurface*)calloc(1, sizeof(RfxSurface));
    if (!surface) return -1;
    
    surface->id = surfaceId;
    surface->width = width;
    surface->height = height;
    surface->gridWidth = (width + RFX_TILE_SIZE - 1) / RFX_TILE_SIZE;
    surface->gridHeight = (height + RFX_TILE_SIZE - 1) / RFX_TILE_SIZE;
    surface->gridSize = surface->gridWidth * surface->gridHeight;
    
    /* Allocate tile grid */
    surface->tiles = (RfxTile**)calloc(surface->gridSize, sizeof(RfxTile*));
    if (!surface->tiles) {
        free(surface);
        return -1;
    }
    
    ctx->surfaces[surfaceId] = surface;
    return 0;
}

/**
 * Delete surface context
 */
EMSCRIPTEN_KEEPALIVE
void prog_delete_surface(ProgressiveContext* ctx, uint16_t surfaceId) {
    if (!ctx || surfaceId >= RFX_MAX_SURFACES) return;
    
    RfxSurface* surface = ctx->surfaces[surfaceId];
    if (!surface) return;
    
    if (surface->tiles) {
        for (uint32_t i = 0; i < surface->gridSize; i++) {
            if (surface->tiles[i]) {
                free_tile(surface->tiles[i]);
            }
        }
        free(surface->tiles);
    }
    
    free(surface);
    ctx->surfaces[surfaceId] = NULL;
}

/**
 * Reset surface (clear all tiles)
 */
EMSCRIPTEN_KEEPALIVE
void prog_reset_surface(ProgressiveContext* ctx, uint16_t surfaceId) {
    if (!ctx || surfaceId >= RFX_MAX_SURFACES) return;
    
    RfxSurface* surface = ctx->surfaces[surfaceId];
    if (!surface) return;
    
    for (uint32_t i = 0; i < surface->gridSize; i++) {
        if (surface->tiles[i]) {
            surface->tiles[i]->pass = 0;
            surface->tiles[i]->dirty = false;
            memset(surface->tiles[i]->data, 0, TILE_BYTES);
            memset(surface->tiles[i]->yData, 0, TILE_PIXELS * sizeof(int16_t));
            memset(surface->tiles[i]->cbData, 0, TILE_PIXELS * sizeof(int16_t));
            memset(surface->tiles[i]->crData, 0, TILE_PIXELS * sizeof(int16_t));
            memset(surface->tiles[i]->sign, 0, TILE_PIXELS * 3);
        }
    }
}

/**
 * Get or create tile at grid position
 */
static RfxTile* get_or_create_tile(RfxSurface* surface, uint16_t xIdx, uint16_t yIdx) {
    if (xIdx >= surface->gridWidth || yIdx >= surface->gridHeight) {
        return NULL;
    }
    
    uint32_t idx = yIdx * surface->gridWidth + xIdx;
    if (!surface->tiles[idx]) {
        surface->tiles[idx] = alloc_tile(xIdx, yIdx);
    }
    
    return surface->tiles[idx];
}

/**
 * Parse quantization values from stream
 */
static int parse_quant_vals(const uint8_t* data, size_t size, 
                            RfxComponentCodecQuant* quant, uint8_t count) {
    if (size < (size_t)(count * 5)) return -1;
    
    /* FreeRDP order (from progressive_component_codec_quant_read):
     * byte 0: LL3 (low), HL3 (high)
     * byte 1: LH3 (low), HH3 (high)
     * byte 2: HL2 (low), LH2 (high)
     * byte 3: HH2 (low), HL1 (high)
     * byte 4: LH1 (low), HH1 (high)
     */
    for (int i = 0; i < count; i++) {
        const uint8_t* p = data + i * 5;
        quant[i].LL3 = p[0] & 0x0F;
        quant[i].HL3 = (p[0] >> 4) & 0x0F;
        quant[i].LH3 = p[1] & 0x0F;
        quant[i].HH3 = (p[1] >> 4) & 0x0F;
        quant[i].HL2 = p[2] & 0x0F;
        quant[i].LH2 = (p[2] >> 4) & 0x0F;
        quant[i].HH2 = p[3] & 0x0F;
        quant[i].HL1 = (p[3] >> 4) & 0x0F;
        quant[i].LH1 = p[4] & 0x0F;
        quant[i].HH1 = (p[4] >> 4) & 0x0F;
    }
    
    return count * 5;
}

/**
 * Parse progressive quantization values from stream
 * Each entry is 16 bytes: 1 byte quality + 5 bytes Y + 5 bytes Cb + 5 bytes Cr
 * 
 * FreeRDP order (from progressive_component_codec_quant_read):
 * byte 0: LL3 (low), HL3 (high)
 * byte 1: LH3 (low), HH3 (high)
 * byte 2: HL2 (low), LH2 (high)
 * byte 3: HH2 (low), HL1 (high)
 * byte 4: LH1 (low), HH1 (high)
 */
static int parse_prog_quant_vals(const uint8_t* data, size_t size,
                                  RfxProgressiveCodecQuant* progQuant, uint8_t count) {
    if (size < (size_t)(count * 16)) return -1;
    
    for (int i = 0; i < count; i++) {
        const uint8_t* p = data + i * 16;
        /* p[0] = quality byte - ignored, quality comes from tile header */
        
        /* Y quant values (bytes 1-5) */
        progQuant[i].yQuant.LL3 = p[1] & 0x0F;
        progQuant[i].yQuant.HL3 = (p[1] >> 4) & 0x0F;
        progQuant[i].yQuant.LH3 = p[2] & 0x0F;
        progQuant[i].yQuant.HH3 = (p[2] >> 4) & 0x0F;
        progQuant[i].yQuant.HL2 = p[3] & 0x0F;
        progQuant[i].yQuant.LH2 = (p[3] >> 4) & 0x0F;
        progQuant[i].yQuant.HH2 = p[4] & 0x0F;
        progQuant[i].yQuant.HL1 = (p[4] >> 4) & 0x0F;
        progQuant[i].yQuant.LH1 = p[5] & 0x0F;
        progQuant[i].yQuant.HH1 = (p[5] >> 4) & 0x0F;
        
        /* Cb quant values (bytes 6-10) */
        progQuant[i].cbQuant.LL3 = p[6] & 0x0F;
        progQuant[i].cbQuant.HL3 = (p[6] >> 4) & 0x0F;
        progQuant[i].cbQuant.LH3 = p[7] & 0x0F;
        progQuant[i].cbQuant.HH3 = (p[7] >> 4) & 0x0F;
        progQuant[i].cbQuant.HL2 = p[8] & 0x0F;
        progQuant[i].cbQuant.LH2 = (p[8] >> 4) & 0x0F;
        progQuant[i].cbQuant.HH2 = p[9] & 0x0F;
        progQuant[i].cbQuant.HL1 = (p[9] >> 4) & 0x0F;
        progQuant[i].cbQuant.LH1 = p[10] & 0x0F;
        progQuant[i].cbQuant.HH1 = (p[10] >> 4) & 0x0F;
        
        /* Cr quant values (bytes 11-15) */
        progQuant[i].crQuant.LL3 = p[11] & 0x0F;
        progQuant[i].crQuant.HL3 = (p[11] >> 4) & 0x0F;
        progQuant[i].crQuant.LH3 = p[12] & 0x0F;
        progQuant[i].crQuant.HH3 = (p[12] >> 4) & 0x0F;
        progQuant[i].crQuant.HL2 = p[13] & 0x0F;
        progQuant[i].crQuant.LH2 = (p[13] >> 4) & 0x0F;
        progQuant[i].crQuant.HH2 = p[14] & 0x0F;
        progQuant[i].crQuant.HL1 = (p[14] >> 4) & 0x0F;
        progQuant[i].crQuant.LH1 = p[15] & 0x0F;
        progQuant[i].crQuant.HH1 = (p[15] >> 4) & 0x0F;
    }
    
    return count * 16;
}

/**
 * Decode a simple tile (non-progressive) - thread-safe version
 * 
 * Uses stack-allocated buffers for Y/Cb/Cr to avoid race conditions
 * when multiple tiles are decoded concurrently in Web Workers.
 */
static int decode_tile_simple(ProgressiveContext* ctx, RfxSurface* surface,
                              const uint8_t* data, size_t size) {
    if (size < 22) return -1;
    
    /* Stack-allocated buffers for thread safety in concurrent WASM workers */
    int16_t yBuffer[TILE_PIXELS];
    int16_t cbBuffer[TILE_PIXELS];
    int16_t crBuffer[TILE_PIXELS];
    
    uint8_t quantIdxY = data[0];
    uint8_t quantIdxCb = data[1];
    uint8_t quantIdxCr = data[2];
    uint16_t xIdx = read_u16_le(data + 3);
    uint16_t yIdx = read_u16_le(data + 5);
    /* uint8_t flags = data[7]; */
    uint16_t yLen = read_u16_le(data + 8);
    uint16_t cbLen = read_u16_le(data + 10);
    uint16_t crLen = read_u16_le(data + 12);
    uint16_t tailLen = read_u16_le(data + 14);
    
    const uint8_t* yData = data + 16;
    const uint8_t* cbData = yData + yLen;
    const uint8_t* crData = cbData + cbLen;
    /* const uint8_t* tailData = crData + crLen; */
    
    if (size < (size_t)(16 + yLen + cbLen + crLen + tailLen)) return -1;
    
    RfxTile* tile = get_or_create_tile(surface, xIdx, yIdx);
    if (!tile) return -1;
    
    /* Track tile index for batch updates */
    uint32_t tileIdx = yIdx * surface->gridWidth + xIdx;
    if (ctx->numUpdatedTiles < RFX_MAX_TILES_PER_SURFACE) {
        ctx->updatedTileIndices[ctx->numUpdatedTiles++] = tileIdx;
    }
    
    /* Get quantization values */
    RfxComponentCodecQuant* yQuant = &ctx->quantVals[quantIdxY];
    RfxComponentCodecQuant* cbQuant = &ctx->quantVals[quantIdxCb];
    RfxComponentCodecQuant* crQuant = &ctx->quantVals[quantIdxCr];
    
    /* Decode Y component */
    memset(yBuffer, 0, TILE_PIXELS * sizeof(int16_t));
    memset(cbBuffer, 0, TILE_PIXELS * sizeof(int16_t));
    memset(crBuffer, 0, TILE_PIXELS * sizeof(int16_t));
    rfx_rlgr_decode(yData, yLen, yBuffer, TILE_PIXELS);
    rfx_differential_decode(&yBuffer[4015], 81);  /* LL3 subband (extrapolated) */
    rfx_dequantize(yBuffer, yQuant);
    rfx_dwt_decode(yBuffer, RFX_TILE_SIZE);
    
    /* Decode Cb component */
    rfx_rlgr_decode(cbData, cbLen, cbBuffer, TILE_PIXELS);
    rfx_differential_decode(&cbBuffer[4015], 81);
    rfx_dequantize(cbBuffer, cbQuant);
    rfx_dwt_decode(cbBuffer, RFX_TILE_SIZE);
    
    /* Decode Cr component */
    rfx_rlgr_decode(crData, crLen, crBuffer, TILE_PIXELS);
    rfx_differential_decode(&crBuffer[4015], 81);
    rfx_dequantize(crBuffer, crQuant);
    rfx_dwt_decode(crBuffer, RFX_TILE_SIZE);
    
    /* Convert to RGBA */
    rfx_ycbcr_to_rgba(yBuffer, cbBuffer, crBuffer,
                      tile->data, RFX_TILE_SIZE * 4);
    
    tile->pass = 1;
    tile->dirty = true;
    
    return 0;
}

/**
 * Decode first progressive tile - thread-safe version
 * 
 * Uses stack-allocated buffers for Y/Cb/Cr to avoid race conditions
 * when multiple tiles are decoded concurrently in Web Workers.
 */
static int decode_tile_first(ProgressiveContext* ctx, RfxSurface* surface,
                             const uint8_t* data, size_t size) {
    if (size < 17) return -1;
    
    /* Stack-allocated buffers for thread safety in concurrent WASM workers */
    int16_t yBuffer[TILE_PIXELS];
    int16_t cbBuffer[TILE_PIXELS];
    int16_t crBuffer[TILE_PIXELS];
    
    uint8_t quantIdxY = data[0];
    uint8_t quantIdxCb = data[1];
    uint8_t quantIdxCr = data[2];
    uint16_t xIdx = read_u16_le(data + 3);
    uint16_t yIdx = read_u16_le(data + 5);
    /* uint8_t flags = data[7]; */
    uint8_t quality = data[8];
    uint16_t yLen = read_u16_le(data + 9);
    uint16_t cbLen = read_u16_le(data + 11);
    uint16_t crLen = read_u16_le(data + 13);
    uint16_t tailLen = read_u16_le(data + 15);
    
    const uint8_t* yData = data + 17;
    const uint8_t* cbData = yData + yLen;
    const uint8_t* crData = cbData + cbLen;
    
    if (size < (size_t)(17 + yLen + cbLen + crLen + tailLen)) return -1;
    
    RfxTile* tile = get_or_create_tile(surface, xIdx, yIdx);
    if (!tile) return -1;
    
    /* Track tile index for batch updates */
    uint32_t tileIdx = yIdx * surface->gridWidth + xIdx;
    if (ctx->numUpdatedTiles < RFX_MAX_TILES_PER_SURFACE) {
        ctx->updatedTileIndices[ctx->numUpdatedTiles++] = tileIdx;
    }
    
    /* Store quantization indices for progressive refinement */
    tile->yQuant = ctx->quantVals[quantIdxY];
    tile->cbQuant = ctx->quantVals[quantIdxCb];
    tile->crQuant = ctx->quantVals[quantIdxCr];
    tile->quality = quality;
    
    /* Get progressive quantization values based on quality index */
    /* Quality 0xFF means full quality (use zero progQuant for all subbands) */
    RfxProgressiveCodecQuant* progQuant = NULL;
    if (quality == 0xFF || ctx->numProgQuant == 0) {
        /* Full quality - use zero shift for progressive quant */
        static const RfxProgressiveCodecQuant zeroProgQuant = {{0,0,0,0,0,0,0,0,0,0}, 
                                                                {0,0,0,0,0,0,0,0,0,0}, 
                                                                {0,0,0,0,0,0,0,0,0,0}};
        tile->yProgQuant = zeroProgQuant.yQuant;
        tile->cbProgQuant = zeroProgQuant.cbQuant;
        tile->crProgQuant = zeroProgQuant.crQuant;
    } else if (quality < ctx->numProgQuant) {
        progQuant = &ctx->quantProgVals[quality];
        tile->yProgQuant = progQuant->yQuant;
        tile->cbProgQuant = progQuant->cbQuant;
        tile->crProgQuant = progQuant->crQuant;
    } else {
        /* Invalid quality index - use zero */
        static const RfxProgressiveCodecQuant zeroProgQuant = {{0,0,0,0,0,0,0,0,0,0}, 
                                                                {0,0,0,0,0,0,0,0,0,0}, 
                                                                {0,0,0,0,0,0,0,0,0,0}};
        tile->yProgQuant = zeroProgQuant.yQuant;
        tile->cbProgQuant = zeroProgQuant.cbQuant;
        tile->crProgQuant = zeroProgQuant.crQuant;
    }
    
    /* Decode Y component and store for refinement - use stack-allocated buffer */
    /* Sequence: RLGR decode -> differential decode (LL3) -> dequantize -> DWT */
    /* Note: Extrapolated tiles have LL3 at offset 4015 with 81 coefficients (9x9) */
    /* IMPORTANT: Zero the buffer before RLGR decode for first tiles to ensure any
     * unencoded coefficients are zero (RLGR may not fill all 4096 slots) */
    memset(tile->yData, 0, TILE_PIXELS * sizeof(int16_t));
    rfx_rlgr_decode(yData, yLen, tile->yData, TILE_PIXELS);
    rfx_differential_decode(&tile->yData[4015], 81);  /* LL3 subband (extrapolated) */
    
    memcpy(yBuffer, tile->yData, TILE_PIXELS * sizeof(int16_t));
    rfx_dequantize_progressive(yBuffer, &tile->yQuant, &tile->yProgQuant);
    
    /* CRITICAL: Store dequantized coefficients back to tile->yData for progressive upgrades.
     * FreeRDP stores dequantized coefficients in 'current' buffer between passes.
     * Upgrades add new shifted values to already-dequantized coefficients. */
    memcpy(tile->yData, yBuffer, TILE_PIXELS * sizeof(int16_t));
    
    rfx_dwt_decode(yBuffer, RFX_TILE_SIZE);
    
    /* Decode Cb component */
    memset(tile->cbData, 0, TILE_PIXELS * sizeof(int16_t));
    rfx_rlgr_decode(cbData, cbLen, tile->cbData, TILE_PIXELS);
    rfx_differential_decode(&tile->cbData[4015], 81);  /* LL3 subband (extrapolated) */
    memcpy(cbBuffer, tile->cbData, TILE_PIXELS * sizeof(int16_t));
    rfx_dequantize_progressive(cbBuffer, &tile->cbQuant, &tile->cbProgQuant);
    memcpy(tile->cbData, cbBuffer, TILE_PIXELS * sizeof(int16_t));
    rfx_dwt_decode(cbBuffer, RFX_TILE_SIZE);
    
    /* Decode Cr component */
    memset(tile->crData, 0, TILE_PIXELS * sizeof(int16_t));
    rfx_rlgr_decode(crData, crLen, tile->crData, TILE_PIXELS);
    rfx_differential_decode(&tile->crData[4015], 81);  /* LL3 subband (extrapolated) */
    memcpy(crBuffer, tile->crData, TILE_PIXELS * sizeof(int16_t));
    rfx_dequantize_progressive(crBuffer, &tile->crQuant, &tile->crProgQuant);
    memcpy(tile->crData, crBuffer, TILE_PIXELS * sizeof(int16_t));
    rfx_dwt_decode(crBuffer, RFX_TILE_SIZE);
    
    /* Convert to RGBA */
    rfx_ycbcr_to_rgba(yBuffer, cbBuffer, crBuffer,
                      tile->data, RFX_TILE_SIZE * 4);
    
    tile->pass = 1;
    tile->dirty = true;
    
    return 0;
}

/**
 * Decode upgrade progressive tile - thread-safe version
 */
static int decode_tile_upgrade(ProgressiveContext* ctx, RfxSurface* surface,
                               const uint8_t* data, size_t size) {
    if (size < 20) return -1;
    
    /* Stack-allocated buffers for thread safety in concurrent WASM workers */
    int16_t yBuffer[TILE_PIXELS];
    int16_t cbBuffer[TILE_PIXELS];
    int16_t crBuffer[TILE_PIXELS];
    
    /* uint8_t quantIdxY = data[0]; */
    /* uint8_t quantIdxCb = data[1]; */
    /* uint8_t quantIdxCr = data[2]; */
    uint16_t xIdx = read_u16_le(data + 3);
    uint16_t yIdx = read_u16_le(data + 5);
    /* uint8_t quality = data[7]; */
    uint16_t ySrlLen = read_u16_le(data + 8);
    uint16_t yRawLen = read_u16_le(data + 10);
    uint16_t cbSrlLen = read_u16_le(data + 12);
    uint16_t cbRawLen = read_u16_le(data + 14);
    uint16_t crSrlLen = read_u16_le(data + 16);
    uint16_t crRawLen = read_u16_le(data + 18);
    
    size_t offset = 20;
    const uint8_t* ySrlData = data + offset; offset += ySrlLen;
    const uint8_t* yRawData = data + offset; offset += yRawLen;
    const uint8_t* cbSrlData = data + offset; offset += cbSrlLen;
    const uint8_t* cbRawData = data + offset; offset += cbRawLen;
    const uint8_t* crSrlData = data + offset; offset += crSrlLen;
    /* const uint8_t* crRawData = data + offset; offset += crRawLen; */
    
    if (size < offset) return -1;
    
    RfxTile* tile = get_or_create_tile(surface, xIdx, yIdx);
    if (!tile || tile->pass == 0) return -1;
    
    /* Track tile index for batch updates */
    uint32_t tileIdx = yIdx * surface->gridWidth + xIdx;
    if (ctx->numUpdatedTiles < RFX_MAX_TILES_PER_SURFACE) {
        ctx->updatedTileIndices[ctx->numUpdatedTiles++] = tileIdx;
    }
    
    int bitPos = 6 - tile->pass;
    if (bitPos < 0) bitPos = 0;
    
    /* Apply SRL refinement */
    if (ySrlLen > 0) {
        rfx_srl_decode(ySrlData, ySrlLen, tile->yData, tile->sign, TILE_PIXELS, bitPos);
    }
    if (cbSrlLen > 0) {
        rfx_srl_decode(cbSrlData, cbSrlLen, tile->cbData, tile->sign + TILE_PIXELS, TILE_PIXELS, bitPos);
    }
    if (crSrlLen > 0) {
        rfx_srl_decode(crSrlData, crSrlLen, tile->crData, tile->sign + TILE_PIXELS * 2, TILE_PIXELS, bitPos);
    }
    
    /* Apply RAW updates if present */
    (void)yRawData;
    (void)cbRawData;
    (void)crRawLen;
    /* RAW data is currently ignored - full implementation would merge RAW coefficients */
    
    /* Reconstruct tile from updated coefficients - use stack-allocated buffers.
     * IMPORTANT: tile->*Data now contains DEQUANTIZED coefficients from first pass.
     * SRL decode has already added shifted values. Do NOT call rfx_dequantize_progressive
     * again - that would shift the already-shifted coefficients! */
    memcpy(yBuffer, tile->yData, TILE_PIXELS * sizeof(int16_t));
    /* Note: coefficients are already dequantized, go straight to DWT */
    rfx_dwt_decode(yBuffer, RFX_TILE_SIZE);
    
    memcpy(cbBuffer, tile->cbData, TILE_PIXELS * sizeof(int16_t));
    rfx_dwt_decode(cbBuffer, RFX_TILE_SIZE);
    
    memcpy(crBuffer, tile->crData, TILE_PIXELS * sizeof(int16_t));
    rfx_dwt_decode(crBuffer, RFX_TILE_SIZE);
    
    rfx_ycbcr_to_rgba(yBuffer, cbBuffer, crBuffer,
                      tile->data, RFX_TILE_SIZE * 4);
    
    tile->pass++;
    tile->dirty = true;
    
    return 0;
}

/**
 * Parse and decode a region block
 */
static int decode_region(ProgressiveContext* ctx, RfxSurface* surface,
                         const uint8_t* data, size_t size) {
    if (size < 18) return -1;
    
    /* uint8_t tileSize = data[0]; */
    uint16_t numRects = read_u16_le(data + 1);
    uint8_t numQuant = data[3];
    uint8_t numProgQuant = data[4];
    uint8_t flags = data[5];
    uint16_t numTiles = read_u16_le(data + 6);
    uint32_t tileDataSize = read_u32_le(data + 8);
    
    /* Store extrapolate flag in context for use by tile decoders
     * Bit 0 (0x01) = RFX_DWT_REDUCE_EXTRAPOLATE:
     * If extrapolate=1: LL3@4015 (81 coefficients, 9x9)
     * If extrapolate=0: LL3@4032 (64 coefficients, 8x8) - DIFFERENT LAYOUT! */
    ctx->extrapolate = (flags & 0x01) ? true : false;
    
    size_t offset = 12;
    
    /* Skip rectangles (8 bytes each) */
    offset += numRects * 8;
    if (size < offset) return -1;
    
    /* Parse quantization values */
    int quantBytes = parse_quant_vals(data + offset, size - offset, ctx->quantVals, numQuant);
    if (quantBytes < 0) return -1;
    offset += quantBytes;
    ctx->numQuant = numQuant;
    
    /* Parse progressive quant values (16 bytes each) */
    int progQuantBytes = parse_prog_quant_vals(data + offset, size - offset, 
                                                ctx->quantProgVals, numProgQuant);
    if (progQuantBytes < 0) return -1;
    offset += progQuantBytes;
    ctx->numProgQuant = numProgQuant;

    /* Process tiles */
    for (uint16_t i = 0; i < numTiles; i++) {
        if (offset + 6 > size) break;
        
        uint16_t blockType = read_u16_le(data + offset);
        uint32_t blockLen = read_u32_le(data + offset + 2);
        
        if (offset + blockLen > size) break;
        
        const uint8_t* tileData = data + offset + 6;
        size_t tileSize = blockLen - 6;
        
        switch (blockType) {
            case PROGRESSIVE_WBT_TILE_SIMPLE:
                decode_tile_simple(ctx, surface, tileData, tileSize);
                break;
            case PROGRESSIVE_WBT_TILE_FIRST:
                decode_tile_first(ctx, surface, tileData, tileSize);
                break;
            case PROGRESSIVE_WBT_TILE_UPGRADE:
                decode_tile_upgrade(ctx, surface, tileData, tileSize);
                break;
        }
        
        offset += blockLen;
    }
    
    (void)tileDataSize;
    return 0;
}

/**
 * Decompress progressive bitstream for a surface
 * 
 * @param ctx       Progressive context
 * @param srcData   Raw progressive blocks from server
 * @param srcSize   Size of input data
 * @param surfaceId Target surface ID
 * @param frameId   Frame ID for this update
 * @return 0 on success, -1 on error
 */
EMSCRIPTEN_KEEPALIVE
int prog_decompress(ProgressiveContext* ctx, const uint8_t* srcData, 
                    uint32_t srcSize, uint16_t surfaceId, uint32_t frameId) {
    if (!ctx || !srcData || srcSize < 6) return -1;
    
    RfxSurface* surface = ctx->surfaces[surfaceId];
    if (!surface) return -1;
    
    ctx->frameId = frameId;
    ctx->currentSurfaceId = surfaceId;
    surface->frameId = frameId;
    
    /* Clear dirty flags */
    for (uint32_t i = 0; i < surface->gridSize; i++) {
        if (surface->tiles[i]) {
            surface->tiles[i]->dirty = false;
        }
    }
    
    size_t offset = 0;
    
    /* Reset updated tile tracking for new frame */
    ctx->numUpdatedTiles = 0;
    
    while (offset + 6 <= srcSize) {
        uint16_t blockType = read_u16_le(srcData + offset);
        uint32_t blockLen = read_u32_le(srcData + offset + 2);
        
        if (blockLen < 6 || offset + blockLen > srcSize) break;
        
        const uint8_t* blockData = srcData + offset + 6;
        size_t blockDataSize = blockLen - 6;
        
        switch (blockType) {
            case PROGRESSIVE_WBT_SYNC:
                /* Validate sync block: blockLen=12, magic=0xCACCACCA, version=0x0100 */
                if (blockLen == 12 && blockDataSize >= 6) {
                    uint32_t magic = read_u32_le(blockData);
                    uint16_t version = read_u16_le(blockData + 4);
                    if (magic != 0xCACCACCA) {
                        printf("[PROG] SYNC: bad magic 0x%08X\n", magic);
                        return -1;
                    }
                    if (version != 0x0100) {
                        printf("[PROG] SYNC: bad version 0x%04X\n", version);
                        return -1;
                    }
                    ctx->state |= FLAG_WBT_SYNC;
                }
                break;
                
            case PROGRESSIVE_WBT_FRAME_BEGIN:
                /* Parse frame begin: blockLen=12, frameIndex (u32), regionCount (u16) */
                if (blockLen == 12 && blockDataSize >= 6) {
                    ctx->frameIndex = read_u32_le(blockData);
                    ctx->regionCount = read_u16_le(blockData + 4);
                    ctx->state |= FLAG_WBT_FRAME_BEGIN;
                    ctx->state &= ~FLAG_WBT_FRAME_END; /* Clear previous frame end */
                }
                break;
                
            case PROGRESSIVE_WBT_FRAME_END:
                /* Frame end: blockLen=6, no payload */
                ctx->state |= FLAG_WBT_FRAME_END;
                ctx->state &= ~FLAG_WBT_FRAME_BEGIN; /* Clear frame begin for next frame */
                break;
                
            case PROGRESSIVE_WBT_CONTEXT:
                /* Parse context: blockLen=10, ctxId (u8), tileSize (u16), flags (u8) */
                if (blockLen == 10 && blockDataSize >= 4) {
                    ctx->ctxId = blockData[0];
                    ctx->tileSize = read_u16_le(blockData + 1);
                    ctx->ctxFlags = blockData[3];
                    if (ctx->tileSize != 64) {
                        printf("[PROG] CONTEXT: bad tileSize %u\n", ctx->tileSize);
                        return -1;
                    }
                    ctx->state |= FLAG_WBT_CONTEXT;
                }
                break;
                
            case PROGRESSIVE_WBT_REGION:
                decode_region(ctx, surface, blockData, blockDataSize);
                break;
        }
        
        offset += blockLen;
    }
    
    return 0;
}

/**
 * Get tile pixel data for rendering
 * Returns pointer to tile's RGBA data, or NULL if tile doesn't exist/not dirty
 */
EMSCRIPTEN_KEEPALIVE
uint8_t* prog_get_tile_data(ProgressiveContext* ctx, uint16_t surfaceId,
                            uint16_t xIdx, uint16_t yIdx) {
    if (!ctx || surfaceId >= RFX_MAX_SURFACES) return NULL;
    
    RfxSurface* surface = ctx->surfaces[surfaceId];
    if (!surface) return NULL;
    
    if (xIdx >= surface->gridWidth || yIdx >= surface->gridHeight) return NULL;
    
    uint32_t idx = yIdx * surface->gridWidth + xIdx;
    RfxTile* tile = surface->tiles[idx];
    
    if (!tile || !tile->dirty) return NULL;
    
    return tile->data;
}

/**
 * Get number of dirty tiles after decompress
 */
EMSCRIPTEN_KEEPALIVE
uint32_t prog_get_dirty_tile_count(ProgressiveContext* ctx, uint16_t surfaceId) {
    if (!ctx || surfaceId >= RFX_MAX_SURFACES) return 0;
    
    RfxSurface* surface = ctx->surfaces[surfaceId];
    if (!surface) return 0;
    
    uint32_t count = 0;
    for (uint32_t i = 0; i < surface->gridSize; i++) {
        if (surface->tiles[i] && surface->tiles[i]->dirty) {
            count++;
        }
    }
    
    return count;
}

/**
 * Get dirty tile info by index (for iteration)
 * Returns tile x,y position and grid index
 */
EMSCRIPTEN_KEEPALIVE
int prog_get_dirty_tile_info(ProgressiveContext* ctx, uint16_t surfaceId,
                             uint32_t dirtyIndex, uint16_t* outX, uint16_t* outY,
                             uint16_t* outXIdx, uint16_t* outYIdx) {
    if (!ctx || surfaceId >= RFX_MAX_SURFACES) return -1;
    
    RfxSurface* surface = ctx->surfaces[surfaceId];
    if (!surface) return -1;
    
    uint32_t count = 0;
    for (uint32_t i = 0; i < surface->gridSize; i++) {
        if (surface->tiles[i] && surface->tiles[i]->dirty) {
            if (count == dirtyIndex) {
                RfxTile* tile = surface->tiles[i];
                if (outX) *outX = tile->x;
                if (outY) *outY = tile->y;
                if (outXIdx) *outXIdx = tile->xIdx;
                if (outYIdx) *outYIdx = tile->yIdx;
                return 0;
            }
            count++;
        }
    }
    
    return -1;
}

/**
 * Get surface dimensions
 */
EMSCRIPTEN_KEEPALIVE
int prog_get_surface_info(ProgressiveContext* ctx, uint16_t surfaceId,
                          uint32_t* outWidth, uint32_t* outHeight,
                          uint32_t* outGridWidth, uint32_t* outGridHeight) {
    if (!ctx || surfaceId >= RFX_MAX_SURFACES) return -1;
    
    RfxSurface* surface = ctx->surfaces[surfaceId];
    if (!surface) return -1;
    
    if (outWidth) *outWidth = surface->width;
    if (outHeight) *outHeight = surface->height;
    if (outGridWidth) *outGridWidth = surface->gridWidth;
    if (outGridHeight) *outGridHeight = surface->gridHeight;
    
    return 0;
}

/**
 * Get number of updated tiles from the last decompress call
 * This is more efficient than scanning all tiles for dirty flags
 */
EMSCRIPTEN_KEEPALIVE
uint32_t prog_get_updated_tile_count(ProgressiveContext* ctx) {
    if (!ctx) return 0;
    return ctx->numUpdatedTiles;
}

/**
 * Get updated tile index by position in the updated list
 * Returns the tile grid index (can be used to compute x,y position)
 */
EMSCRIPTEN_KEEPALIVE
uint32_t prog_get_updated_tile_index(ProgressiveContext* ctx, uint32_t listIndex) {
    if (!ctx || listIndex >= ctx->numUpdatedTiles) return 0xFFFFFFFF;
    return ctx->updatedTileIndices[listIndex];
}

/**
 * Get frame state flags (for debugging/validation)
 * Returns bitmask: 0x01=SYNC, 0x02=CONTEXT, 0x04=FRAME_BEGIN, 0x08=FRAME_END
 */
EMSCRIPTEN_KEEPALIVE
uint32_t prog_get_frame_state(ProgressiveContext* ctx) {
    if (!ctx) return 0;
    return ctx->state;
}

/**
 * Check if frame is complete (FRAME_END received)
 */
EMSCRIPTEN_KEEPALIVE
int prog_is_frame_complete(ProgressiveContext* ctx) {
    if (!ctx) return 0;
    return (ctx->state & FLAG_WBT_FRAME_END) ? 1 : 0;
}

/* ============================================================================
 * Parallel Tile Decoding with pthreads
 * ============================================================================ */

/* Tile job for parallel processing */
typedef struct {
    ProgressiveContext* ctx;
    RfxSurface* surface;
    uint8_t* data;      /* Copy of tile data (owned by job) */
    size_t size;
    uint16_t blockType;
} TileJob;

/* Work queue for parallel tile decoding */
typedef struct {
    TileJob jobs[MAX_PENDING_TILES];
    int count;
    int next;
    pthread_mutex_t lock;
    pthread_cond_t work_ready;
    pthread_cond_t work_done;
    int active_workers;
    bool shutdown;
} TileWorkQueue;

static TileWorkQueue work_queue = {
    .count = 0,
    .next = 0,
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .work_ready = PTHREAD_COND_INITIALIZER,
    .work_done = PTHREAD_COND_INITIALIZER,
    .active_workers = 0,
    .shutdown = false
};

static pthread_t worker_threads[MAX_WORKER_THREADS];
static bool workers_started = false;

/**
 * Worker thread function - processes tiles from the queue
 */
static void* tile_worker_thread(void* arg) {
    (void)arg;
    
    /* Initialize thread-local buffers */
    get_thread_buffers();
    
    while (1) {
        pthread_mutex_lock(&work_queue.lock);
        
        /* Wait for work or shutdown */
        while (work_queue.next >= work_queue.count && !work_queue.shutdown) {
            pthread_cond_wait(&work_queue.work_ready, &work_queue.lock);
        }
        
        if (work_queue.shutdown) {
            pthread_mutex_unlock(&work_queue.lock);
            break;
        }
        
        /* Get next job */
        int job_idx = work_queue.next++;
        work_queue.active_workers++;
        TileJob job = work_queue.jobs[job_idx];
        
        pthread_mutex_unlock(&work_queue.lock);
        
        /* Process the tile */
        switch (job.blockType) {
            case PROGRESSIVE_WBT_TILE_SIMPLE:
                decode_tile_simple(job.ctx, job.surface, job.data, job.size);
                break;
            case PROGRESSIVE_WBT_TILE_FIRST:
                decode_tile_first(job.ctx, job.surface, job.data, job.size);
                break;
            case PROGRESSIVE_WBT_TILE_UPGRADE:
                decode_tile_upgrade(job.ctx, job.surface, job.data, job.size);
                break;
        }
        
        /* Free the copied tile data */
        free(job.data);
        
        /* Signal completion */
        pthread_mutex_lock(&work_queue.lock);
        work_queue.active_workers--;
        if (work_queue.next >= work_queue.count && work_queue.active_workers == 0) {
            pthread_cond_signal(&work_queue.work_done);
        }
        pthread_mutex_unlock(&work_queue.lock);
    }
    
    return NULL;
}

/**
 * Start worker threads for parallel tile decoding
 */
static void start_worker_threads(void) {
    if (workers_started) return;
    
    work_queue.shutdown = false;
    
    for (int i = 0; i < MAX_WORKER_THREADS; i++) {
        pthread_create(&worker_threads[i], NULL, tile_worker_thread, NULL);
    }
    
    workers_started = true;
}

/**
 * Submit a tile job to the work queue
 */
static void submit_tile_job(ProgressiveContext* ctx, RfxSurface* surface,
                           const uint8_t* data, size_t size, uint16_t blockType) {
    pthread_mutex_lock(&work_queue.lock);
    
    if (work_queue.count < MAX_PENDING_TILES) {
        TileJob* job = &work_queue.jobs[work_queue.count];
        job->ctx = ctx;
        job->surface = surface;
        /* Copy the tile data so it persists until processed */
        job->data = (uint8_t*)malloc(size);
        if (job->data) {
            memcpy(job->data, data, size);
            job->size = size;
            job->blockType = blockType;
            work_queue.count++;
            /* Wake up workers immediately when work is available */
            pthread_cond_signal(&work_queue.work_ready);
        }
    }
    /* Note: Tile queue full condition silently drops tiles - check work_queue.count if debugging */
    
    pthread_mutex_unlock(&work_queue.lock);
}

/**
 * Wait for all pending tile jobs to complete
 */
static void wait_for_tiles(void) {
    pthread_mutex_lock(&work_queue.lock);
    
    /* Signal workers that work is ready */
    pthread_cond_broadcast(&work_queue.work_ready);
    
    /* Wait for all work to complete */
    while (work_queue.next < work_queue.count || work_queue.active_workers > 0) {
        pthread_cond_wait(&work_queue.work_done, &work_queue.lock);
    }
    
    /* Reset queue for next batch */
    work_queue.count = 0;
    work_queue.next = 0;
    
    pthread_mutex_unlock(&work_queue.lock);
}

/**
 * Parse region and submit tiles for parallel processing
 */
static int decode_region_parallel(ProgressiveContext* ctx, RfxSurface* surface,
                                  const uint8_t* data, size_t size) {
    if (size < 18) return -1;
    
    /* uint8_t tileSize = data[0]; */
    uint16_t numRects = read_u16_le(data + 1);
    uint8_t numQuant = data[3];
    uint8_t numProgQuant = data[4];
    /* uint8_t flags = data[5]; */
    uint16_t numTiles = read_u16_le(data + 6);
    uint32_t tileDataSize = read_u32_le(data + 8);
    
    size_t offset = 12;
    
    /* Skip rectangles (8 bytes each) */
    offset += numRects * 8;
    if (size < offset) return -1;
    
    /* Parse quantization values (must be done in main thread) */
    int quantBytes = parse_quant_vals(data + offset, size - offset, ctx->quantVals, numQuant);
    if (quantBytes < 0) return -1;
    offset += quantBytes;
    ctx->numQuant = numQuant;
    
    /* Parse progressive quantization values (16 bytes each) */
    int progQuantBytes = parse_prog_quant_vals(data + offset, size - offset, 
                                                ctx->quantProgVals, numProgQuant);
    if (progQuantBytes < 0) return -1;
    offset += progQuantBytes;
    ctx->numProgQuant = numProgQuant;
    
    /* Submit tiles to worker threads */
    for (uint16_t i = 0; i < numTiles; i++) {
        if (offset + 6 > size) break;
        
        uint16_t blockType = read_u16_le(data + offset);
        uint32_t blockLen = read_u32_le(data + offset + 2);
        
        if (offset + blockLen > size) break;
        
        const uint8_t* tileData = data + offset + 6;
        size_t tileSize = blockLen - 6;
        
        if (blockType == PROGRESSIVE_WBT_TILE_SIMPLE ||
            blockType == PROGRESSIVE_WBT_TILE_FIRST ||
            blockType == PROGRESSIVE_WBT_TILE_UPGRADE) {
            submit_tile_job(ctx, surface, tileData, tileSize, blockType);
        }
        
        offset += blockLen;
    }
    
    (void)tileDataSize;
    return 0;
}

/**
 * Decompress progressive bitstream using parallel tile decoding
 * 
 * @param ctx       Progressive context
 * @param srcData   Raw progressive blocks from server
 * @param srcSize   Size of input data
 * @param surfaceId Target surface ID
 * @param frameId   Frame ID for this update
 * @return 0 on success, -1 on error
 */
EMSCRIPTEN_KEEPALIVE
int prog_decompress_parallel(ProgressiveContext* ctx, const uint8_t* srcData, 
                             uint32_t srcSize, uint16_t surfaceId, uint32_t frameId) {
    if (!ctx || !srcData || srcSize < 6) return -1;
    
    RfxSurface* surface = ctx->surfaces[surfaceId];
    if (!surface) return -1;
    
    /* Ensure worker threads are running */
    start_worker_threads();
    
    ctx->frameId = frameId;
    ctx->currentSurfaceId = surfaceId;
    surface->frameId = frameId;
    
    /* Clear dirty flags */
    for (uint32_t i = 0; i < surface->gridSize; i++) {
        if (surface->tiles[i]) {
            surface->tiles[i]->dirty = false;
        }
    }
    
    size_t offset = 0;
    
    while (offset + 6 <= srcSize) {
        uint16_t blockType = read_u16_le(srcData + offset);
        uint32_t blockLen = read_u32_le(srcData + offset + 2);
        
        if (blockLen < 6 || offset + blockLen > srcSize) break;
        
        const uint8_t* blockData = srcData + offset + 6;
        size_t blockDataSize = blockLen - 6;
        
        switch (blockType) {
            case PROGRESSIVE_WBT_SYNC:
            case PROGRESSIVE_WBT_FRAME_BEGIN:
            case PROGRESSIVE_WBT_FRAME_END:
            case PROGRESSIVE_WBT_CONTEXT:
                /* These are processed synchronously */
                break;
                
            case PROGRESSIVE_WBT_REGION:
                /* Parse region and submit tiles for parallel decoding */
                decode_region_parallel(ctx, surface, blockData, blockDataSize);
                break;
        }
        
        offset += blockLen;
    }
    
    /* Wait for all tiles to complete */
    wait_for_tiles();
    
    return 0;
}
