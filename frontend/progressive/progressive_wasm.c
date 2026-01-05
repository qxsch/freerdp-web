/**
 * Progressive Codec WASM - Main Entry Point
 * Standalone progressive RFX decoder for WebAssembly
 * 
 * Based on FreeRDP's progressive codec (Apache 2.0 License)
 * Optimized for browser execution via Emscripten with pthread support
 */

#include "rfx_types.h"
#include <emscripten.h>
#include <pthread.h>

/* External functions from other modules */
extern int rfx_rlgr_decode(const uint8_t* input, size_t inputSize,
                           int16_t* output, size_t outputSize);
extern int rfx_srl_decode(const uint8_t* input, size_t inputSize,
                          int16_t* current, int8_t* sign,
                          size_t coeffCount, int bitPos);
extern void rfx_dwt_decode(int16_t* buffer, int size);
extern void rfx_dequantize(int16_t* buffer, const RfxComponentCodecQuant* quant);
extern void rfx_ycbcr_to_rgba(const int16_t* yData, const int16_t* cbData,
                              const int16_t* crData, uint8_t* dst, int dstStride);

/* Tile pixel buffer size */
#define TILE_PIXELS (RFX_TILE_SIZE * RFX_TILE_SIZE)
#define TILE_BYTES (TILE_PIXELS * 4)

/* Thread pool configuration */
#define MAX_WORKER_THREADS 4
#define MAX_PENDING_TILES 256

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
    
    for (int i = 0; i < count; i++) {
        const uint8_t* p = data + i * 5;
        quant[i].LL3 = p[0] & 0x0F;
        quant[i].LH3 = (p[0] >> 4) & 0x0F;
        quant[i].HL3 = p[1] & 0x0F;
        quant[i].HH3 = (p[1] >> 4) & 0x0F;
        quant[i].LH2 = p[2] & 0x0F;
        quant[i].HL2 = (p[2] >> 4) & 0x0F;
        quant[i].HH2 = p[3] & 0x0F;
        quant[i].LH1 = (p[3] >> 4) & 0x0F;
        quant[i].HL1 = p[4] & 0x0F;
        quant[i].HH1 = (p[4] >> 4) & 0x0F;
    }
    
    return count * 5;
}

/**
 * Decode a simple tile (non-progressive) - thread-safe version
 */
static int decode_tile_simple(ProgressiveContext* ctx, RfxSurface* surface,
                              const uint8_t* data, size_t size) {
    if (size < 22) return -1;
    
    ThreadLocalBuffers* tls = get_thread_buffers();
    if (!tls->initialized) return -1;
    
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
    
    /* Get quantization values */
    RfxComponentCodecQuant* yQuant = &ctx->quantVals[quantIdxY];
    RfxComponentCodecQuant* cbQuant = &ctx->quantVals[quantIdxCb];
    RfxComponentCodecQuant* crQuant = &ctx->quantVals[quantIdxCr];
    
    /* Decode Y component - use thread-local buffers */
    rfx_rlgr_decode(yData, yLen, tls->yBuffer, TILE_PIXELS);
    rfx_dequantize(tls->yBuffer, yQuant);
    rfx_dwt_decode(tls->yBuffer, RFX_TILE_SIZE);
    
    /* Decode Cb component */
    rfx_rlgr_decode(cbData, cbLen, tls->cbBuffer, TILE_PIXELS);
    rfx_dequantize(tls->cbBuffer, cbQuant);
    rfx_dwt_decode(tls->cbBuffer, RFX_TILE_SIZE);
    
    /* Decode Cr component */
    rfx_rlgr_decode(crData, crLen, tls->crBuffer, TILE_PIXELS);
    rfx_dequantize(tls->crBuffer, crQuant);
    rfx_dwt_decode(tls->crBuffer, RFX_TILE_SIZE);
    
    /* Convert to RGBA */
    rfx_ycbcr_to_rgba(tls->yBuffer, tls->cbBuffer, tls->crBuffer,
                      tile->data, RFX_TILE_SIZE * 4);
    
    tile->pass = 1;
    tile->dirty = true;
    
    return 0;
}

/**
 * Decode first progressive tile - thread-safe version
 */
static int decode_tile_first(ProgressiveContext* ctx, RfxSurface* surface,
                             const uint8_t* data, size_t size) {
    if (size < 17) return -1;
    
    ThreadLocalBuffers* tls = get_thread_buffers();
    if (!tls->initialized) return -1;
    
    uint8_t quantIdxY = data[0];
    uint8_t quantIdxCb = data[1];
    uint8_t quantIdxCr = data[2];
    uint16_t xIdx = read_u16_le(data + 3);
    uint16_t yIdx = read_u16_le(data + 5);
    /* uint8_t flags = data[7]; */
    /* uint8_t quality = data[8]; */
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
    
    /* Store quantization indices for progressive refinement */
    tile->yQuant = ctx->quantVals[quantIdxY];
    tile->cbQuant = ctx->quantVals[quantIdxCb];
    tile->crQuant = ctx->quantVals[quantIdxCr];
    
    /* Decode Y component and store for refinement - use thread-local buffer */
    rfx_rlgr_decode(yData, yLen, tile->yData, TILE_PIXELS);
    memcpy(tls->yBuffer, tile->yData, TILE_PIXELS * sizeof(int16_t));
    rfx_dequantize(tls->yBuffer, &tile->yQuant);
    rfx_dwt_decode(tls->yBuffer, RFX_TILE_SIZE);
    
    /* Decode Cb component */
    rfx_rlgr_decode(cbData, cbLen, tile->cbData, TILE_PIXELS);
    memcpy(tls->cbBuffer, tile->cbData, TILE_PIXELS * sizeof(int16_t));
    rfx_dequantize(tls->cbBuffer, &tile->cbQuant);
    rfx_dwt_decode(tls->cbBuffer, RFX_TILE_SIZE);
    
    /* Decode Cr component */
    rfx_rlgr_decode(crData, crLen, tile->crData, TILE_PIXELS);
    memcpy(tls->crBuffer, tile->crData, TILE_PIXELS * sizeof(int16_t));
    rfx_dequantize(tls->crBuffer, &tile->crQuant);
    rfx_dwt_decode(tls->crBuffer, RFX_TILE_SIZE);
    
    /* Convert to RGBA */
    rfx_ycbcr_to_rgba(tls->yBuffer, tls->cbBuffer, tls->crBuffer,
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
    
    ThreadLocalBuffers* tls = get_thread_buffers();
    if (!tls->initialized) return -1;
    
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
    
    /* Reconstruct tile from updated coefficients - use thread-local buffers */
    memcpy(tls->yBuffer, tile->yData, TILE_PIXELS * sizeof(int16_t));
    rfx_dequantize(tls->yBuffer, &tile->yQuant);
    rfx_dwt_decode(tls->yBuffer, RFX_TILE_SIZE);
    
    memcpy(tls->cbBuffer, tile->cbData, TILE_PIXELS * sizeof(int16_t));
    rfx_dequantize(tls->cbBuffer, &tile->cbQuant);
    rfx_dwt_decode(tls->cbBuffer, RFX_TILE_SIZE);
    
    memcpy(tls->crBuffer, tile->crData, TILE_PIXELS * sizeof(int16_t));
    rfx_dequantize(tls->crBuffer, &tile->crQuant);
    rfx_dwt_decode(tls->crBuffer, RFX_TILE_SIZE);
    
    rfx_ycbcr_to_rgba(tls->yBuffer, tls->cbBuffer, tls->crBuffer,
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
    /* uint8_t flags = data[5]; */
    uint16_t numTiles = read_u16_le(data + 6);
    uint32_t tileDataSize = read_u32_le(data + 8);
    
    size_t offset = 12;
    
    /* Skip rectangles (8 bytes each) */
    offset += numRects * 8;
    if (size < offset) return -1;
    
    /* Parse quantization values */
    int quantBytes = parse_quant_vals(data + offset, size - offset, ctx->quantVals, numQuant);
    if (quantBytes < 0) return -1;
    offset += quantBytes;
    ctx->numQuant = numQuant;
    
    /* Skip progressive quant values for now (16 bytes each) */
    offset += numProgQuant * 16;
    if (size < offset) return -1;
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
    
    while (offset + 6 <= srcSize) {
        uint16_t blockType = read_u16_le(srcData + offset);
        uint32_t blockLen = read_u32_le(srcData + offset + 2);
        
        if (blockLen < 6 || offset + blockLen > srcSize) break;
        
        const uint8_t* blockData = srcData + offset + 6;
        size_t blockDataSize = blockLen - 6;
        
        switch (blockType) {
            case PROGRESSIVE_WBT_SYNC:
                /* Sync block - just validate */
                break;
                
            case PROGRESSIVE_WBT_FRAME_BEGIN:
                /* Frame begin - could extract frame info */
                break;
                
            case PROGRESSIVE_WBT_FRAME_END:
                /* Frame end */
                break;
                
            case PROGRESSIVE_WBT_CONTEXT:
                /* Context block - configure tile size etc */
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
        }
    }
    
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
    
    /* Skip progressive quant values for now (16 bytes each) */
    offset += numProgQuant * 16;
    if (size < offset) return -1;
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
