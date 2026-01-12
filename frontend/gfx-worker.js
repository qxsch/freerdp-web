/**
 * GFX Compositor Worker
 * 
 * Handles off-main-thread rendering of RDP graphics:
 * - Progressive tile decoding via WASM (RFX Progressive codec)
 * - H.264 decoding via VideoDecoder API (AVC420/AVC444)
 * - WebP tile decoding via createImageBitmap
 * - Surface management and composition
 * - Frame lifecycle and acknowledgment
 * 
 * IMPORTANT: Strict message ordering is guaranteed by processing messages
 * sequentially through a queue. No message is processed until the previous
 * one completes (including async operations like H.264 decode).
 */

const BUILD_VERSION = '__BUILD_TIME__';
console.log(`[GFX-WORKER] ===== BUILD ${BUILD_VERSION} =====`);

import {
    Magic, matchMagic, parseMessage,
    readU16LE, readU32LE, buildFrameAck, buildBackpressure
} from './wire-format.js';

// ============================================================================
// Message Queue for Strict Ordering
// ============================================================================

/** @type {Array} Queue of pending messages */
const messageQueue = [];

/** @type {boolean} Whether we're currently processing a message */
let isProcessing = false;

/**
 * Add message to queue and process if not already processing.
 * Ensures strict sequential processing of all messages.
 */
async function enqueueMessage(msg) {
    messageQueue.push(msg);
    
    // If already processing, the current processor will pick up this message
    if (isProcessing) return;
    
    isProcessing = true;
    while (messageQueue.length > 0) {
        const next = messageQueue.shift();
        try {
            await processMessage(next);
        } catch (err) {
            console.error('[GFX Worker] Error processing message:', err);
        }
    }
    isProcessing = false;
}

// ============================================================================
// State
// ============================================================================

/** @type {Object|null} WASM module instance */
let wasmModule = null;

/** @type {number|null} Progressive decoder context pointer */
let progCtx = null;

/** @type {Object|null} ClearCodec WASM module instance */
let clearWasmModule = null;

/** @type {number|null} ClearCodec decoder context pointer */
let clearCtx = null;

/** @type {boolean} Whether ClearCodec WASM is ready */
let clearWasmReady = false;

/** @type {Map<number, Surface>} Surface ID → surface state */
const surfaces = new Map();

/** 
 * Cache slot → cached bitmap entry (for SurfaceToCache/CacheToSurface)
 * Enhanced with source tracking for debugging cache corruption issues
 */
const bitmapCache = new Map();

/** @type {OffscreenCanvas|null} Primary render target */
let primaryCanvas = null;

/** @type {OffscreenCanvasRenderingContext2D|null} Primary 2D context */
let primaryCtx = null;

/** @type {number|null} Primary surface ID (mapped to output) */
let primarySurfaceId = null;

/** @type {number|null} Current frame being processed */
let currentFrameId = null;

/** @type {number|null} Last completed frame ID for skip detection */
let lastCompletedFrameId = null;

/** @type {Set<number>} Surfaces updated in the current frame */
let frameUpdatedSurfaces = new Set();

/** @type {boolean} Whether WASM is ready */
let wasmReady = false;

/** @type {number} Pending operations count for backpressure */
let pendingOps = 0;

/** @type {number} Max pending ops before backpressure */
const MAX_PENDING_OPS = 64;

/** @type {number} Total frames decoded - sent in FACK for MS-RDPEGFX compliance */
let totalFramesDecoded = 0;

// ============================================================================
// Codec IDs (matching rdp_bridge.h RdpGfxCodecId enum)
// ============================================================================

const CODEC_ID = {
    UNCOMPRESSED: 0x0000,
    CLEARCODEC: 0x0003,
    PLANAR: 0x0004,
    AVC420: 0x0009,
    ALPHA: 0x000A,
    AVC444: 0x000B,
    PROGRESSIVE: 0x000C,
    PROGRESSIVE_V2: 0x000D,
    AVC444v2: 0x000E,
};

// ============================================================================
// H.264 decoder state
// ============================================================================

/** @type {VideoDecoder|null} H.264 video decoder */
let videoDecoder = null;

/** @type {boolean} Whether H.264 is initialized */
let h264Initialized = false;

/** @type {number} Configured width for H.264 */
let h264ConfiguredWidth = 0;

/** @type {number} Configured height for H.264 */
let h264ConfiguredHeight = 0;

/** @type {Array} Queue of decode metadata for frame output */
let h264DecodeQueue = [];

/** @type {boolean} Whether there was a decoder error */
let h264DecoderError = false;

/** @type {boolean} Whether we need a keyframe (after init/reset/error) */
let h264NeedsKeyframe = true;

// ============================================================================
// Surface management
// ============================================================================

/**
 * @typedef {Object} Surface
 * @property {number} id
 * @property {number} width
 * @property {number} height
 * @property {OffscreenCanvas} canvas
 * @property {OffscreenCanvasRenderingContext2D} ctx
 */

/**
 * Track last deleted surface info for potential preservation
 * Used to preserve WASM Progressive state when surface is immediately recreated
 */
let lastDeletedSurface = null;

/**
 * GFX Pixel Format constants (MS-RDPEGFX 2.2.3.1)
 */
const GFX_PIXEL_FORMAT = {
    XRGB_8888: 0x20,  // 32bpp RGB, no alpha
    ARGB_8888: 0x21,  // 32bpp ARGB, with alpha
};

/**
 * Get the name of a pixel format constant
 * 
 * @param {number} pixelFormat - Pixel format (0x20=XRGB_8888, 0x21=ARGB_8888)
 */
function getPixelFormatName(value) {
    return Object.keys(GFX_PIXEL_FORMAT).find(
        key => GFX_PIXEL_FORMAT[key] === value
    );
}

/**
 * Create a new surface
 * Per RFX/GFX protocol: Surface create always starts with fresh progressive state.
 * The server will send new TILE_FIRST data for any tiles it wants to render.
 * 
 * @param {number} surfaceId - Surface identifier
 * @param {number} width - Surface width in pixels
 * @param {number} height - Surface height in pixels  
 * @param {number} pixelFormat - Pixel format (0x20=XRGB_8888, 0x21=ARGB_8888)
 */
function createSurface(surfaceId, width, height, pixelFormat = GFX_PIXEL_FORMAT.XRGB_8888) {
    // Delete existing if present
    if (surfaces.has(surfaceId)) {
        deleteSurface(surfaceId);
    }
    
    const canvas = new OffscreenCanvas(width, height);
    const ctx = canvas.getContext('2d', { 
        alpha: false,
        desynchronized: true,  // Reduce latency
        willReadFrequently: true  // Optimize for SurfaceToCache getImageData calls
    });
    
    // Per MS-RDPEGFX spec: compositor must use direct copy, no alpha blending
    ctx.globalCompositeOperation = 'copy';
    ctx.imageSmoothingEnabled = false;
    
    // Fill with black initially
    ctx.fillStyle = '#000000';
    ctx.fillRect(0, 0, width, height);
    
    // Restore to source-over for normal drawing operations (tiles use putImageData anyway)
    ctx.globalCompositeOperation = 'source-over';
    
    surfaces.set(surfaceId, {
        id: surfaceId,
        width,
        height,
        pixelFormat,  // Store for potential ClearCodec processing
        canvas,
        ctx
    });
    
    // Always create fresh WASM Progressive state for new surfaces
    // Per RFX/GFX protocol: Surface lifecycle = Progressive codec lifecycle
    // The server will send fresh TILE_FIRST data, not UPGRADE tiles expecting old state
    if (wasmReady && progCtx) {
        // Delete any existing WASM state (may exist from previous surface with same ID)
        wasmModule._prog_delete_surface(progCtx, surfaceId);
        wasmModule._prog_create_surface(progCtx, surfaceId, width, height);
        console.log(`[GFX Worker] Created surface ${surfaceId}: ${width}x${height}, pixelFormat=${getPixelFormatName(pixelFormat)} (fresh Progressive state)`);
    } else {
        console.log(`[GFX Worker] Created surface ${surfaceId}: ${width}x${height}, pixelFormat=${getPixelFormatName(pixelFormat)}`);
    }
    
    // Clear the last deleted surface info (no longer needed for preservation logic)
    lastDeletedSurface = null;
}

/**
 * Delete a surface
 * Per RFX/GFX protocol: When surface is deleted, BOTH the surface cache (JS canvas)
 * AND the progressive codec state (WASM) must be cleared together.
 * They are tied - you cannot have valid progressive state without a valid surface.
 */
function deleteSurface(surfaceId) {
    const surface = surfaces.get(surfaceId);
    if (!surface) return;
    
    // Store info about deleted surface (for logging/debugging purposes only)
    lastDeletedSurface = {
        id: surfaceId,
        width: surface.width,
        height: surface.height,
        timestamp: Date.now()
    };
    
    // Delete JS surface cache
    surfaces.delete(surfaceId);
    
    // Remove from frameUpdatedSurfaces tracking
    frameUpdatedSurfaces.delete(surfaceId);
    
    // NOTE: Do NOT evict bitmap cache entries here!
    // The RDP bitmap cache is independent of surfaces. Cache entries survive
    // surface delete/recreate. The server explicitly sends EvictCacheEntry PDU
    // when it wants to invalidate cache slots.
    
    // Delete WASM Progressive state - per protocol, both caches die together
    if (wasmReady && progCtx) {
        wasmModule._prog_delete_surface(progCtx, surfaceId);
    }
    
    if (primarySurfaceId === surfaceId) {
        primarySurfaceId = null;
    }
    
    console.log(`[GFX Worker] Deleted surface ${surfaceId} (both JS canvas and WASM Progressive state cleared)`);
}

/**
 * Map surface to primary output
 */
function mapSurfaceToPrimary(surfaceId) {
    primarySurfaceId = surfaceId;
}

// ============================================================================
// WASM initialization
// ============================================================================

/** @type {boolean} Whether parallel decompression is available */
let parallelDecompressAvailable = false;

/**
 * Initialize Progressive decoder WASM module
 * Supports pthreads for parallel tile decoding when available
 */
async function initWasm() {
    try {
        // Dynamic import of the WASM module (ES6 module output from Emscripten)
        const module = await import('./progressive/progressive_decoder.js');
        
        // Emscripten MODULARIZE + EXPORT_ES6 exports the factory as default
        const ModuleFactory = module.default;
        
        if (typeof ModuleFactory !== 'function') {
            throw new Error('WASM module factory not found in exports');
        }
        
        // Initialize WASM - pthreads require special locateFile for worker
        wasmModule = await ModuleFactory({
            // Help Emscripten find the pthread worker script
            locateFile: (path) => {
                if (path.endsWith('.worker.js')) {
                    return './progressive/' + path;
                }
                if (path.endsWith('.wasm')) {
                    return './progressive/' + path;
                }
                return path;
            },
            // Capture printf/stderr from WASM code
            print: (text) => console.log('[WASM]', text),
            printErr: (text) => console.warn('[WASM-ERR]', text)
        });
        
        // Create progressive context
        progCtx = wasmModule._prog_create();
        if (!progCtx) {
            throw new Error('Failed to create progressive decoder context');
        }
        
        // Check if parallel decompress is available
        parallelDecompressAvailable = typeof wasmModule._prog_decompress_parallel === 'function';
        
        // Create surfaces in WASM for any already-created surfaces
        for (const [id, surface] of surfaces) {
            wasmModule._prog_create_surface(progCtx, id, surface.width, surface.height);
        }
        
        wasmReady = true;
        console.log(`[GFX Worker] Progressive WASM decoder initialized (parallel: ${parallelDecompressAvailable})`);
        
    } catch (err) {
        console.warn('[GFX Worker] Progressive WASM not available:', err.message);
        console.warn('[GFX Worker] Progressive tiles will not be decoded');
        wasmReady = false;
    }
}

/**
 * Initialize ClearCodec decoder WASM module
 */
async function initClearCodecWasm() {
    try {
        // Dynamic import of the ClearCodec WASM module
        const module = await import('./clearcodec/clearcodec_decoder.js');
        
        // Emscripten MODULARIZE exports the factory as default
        const ModuleFactory = module.default;
        
        if (typeof ModuleFactory !== 'function') {
            throw new Error('ClearCodec WASM module factory not found in exports');
        }
        
        // Initialize WASM
        clearWasmModule = await ModuleFactory({
            // Help Emscripten find the wasm file
            locateFile: (path) => {
                if (path.endsWith('.wasm')) {
                    return './clearcodec/' + path;
                }
                return path;
            },
            // Capture printf/stderr from WASM code
            print: (text) => console.log('[CLEAR-WASM]', text),
            printErr: (text) => console.warn('[CLEAR-WASM-ERR]', text)
        });
        
        // Create ClearCodec context (session-level, shared across all surfaces)
        clearCtx = clearWasmModule._clear_create();
        if (!clearCtx) {
            throw new Error('Failed to create ClearCodec decoder context');
        }
        
        clearWasmReady = true;
        console.log('[GFX Worker] ClearCodec WASM decoder initialized');
        
    } catch (err) {
        console.warn('[GFX Worker] ClearCodec WASM not available:', err.message);
        console.warn('[GFX Worker] ClearCodec tiles will not be decoded');
        clearWasmReady = false;
    }
}

// ============================================================================
// Tile decoding
// ============================================================================

/**
 * Decode and draw a progressive tile
 * Uses batch-oriented API: decodes all tiles, then renders after frame complete
 */
function decodeProgressiveTile(msg) {
    if (!wasmReady || !progCtx) {
        console.warn('[GFX Worker] Progressive decoder not ready, skipping tile');
        return;
    }
    
    let surface = surfaces.get(msg.surfaceId);
    let actualSurfaceId = msg.surfaceId;
    
    if (!surface) {
        // Surface was deleted - discard stale data for it
        // Do NOT apply to a different surface (would cause wrong content like login screen on desktop)
        console.warn(`[GFX Worker] Discarding Progressive data for deleted surface ${msg.surfaceId}`);
        return;
    }
    
    // Track that this surface was updated in the current frame
    frameUpdatedSurfaces.add(actualSurfaceId);
    
    const payload = msg.payload;
    
    // Allocate WASM memory for input
    const inputPtr = wasmModule._malloc(payload.byteLength);
    wasmModule.HEAPU8.set(payload, inputPtr);
    
    // Use parallel decompress when available for better performance
    let result;
    if (parallelDecompressAvailable) {
        result = wasmModule._prog_decompress_parallel(
            progCtx, inputPtr, payload.byteLength, 
            actualSurfaceId, msg.frameId
        );
    } else {
        result = wasmModule._prog_decompress(
            progCtx, inputPtr, payload.byteLength, 
            actualSurfaceId, msg.frameId
        );
    }
    
    wasmModule._free(inputPtr);
    
    if (result !== 0) {
        console.warn(`[GFX Worker] Progressive decompress failed: ${result}`);
        return;
    }
    
    // Check frame state - only render when frame is complete
    const frameComplete = wasmModule._prog_is_frame_complete(progCtx);
    
    // Use new batch tile tracking API for efficient iteration
    const updatedCount = wasmModule._prog_get_updated_tile_count(progCtx);
    
    // Skip rendering if no tiles updated
    if (updatedCount === 0) {
        return;
    }
    
    let tilesDrawn = 0;
    
    // Get surface grid dimensions for index-to-coordinates conversion
    const gridWidth = Math.ceil(surface.width / 64);
    
    try {
        // Iterate using batch tile tracking (more efficient than scanning dirty flags)
        for (let i = 0; i < updatedCount; i++) {
            const tileIdx = wasmModule._prog_get_updated_tile_index(progCtx, i);
            if (tileIdx === 0xFFFFFFFF) {
                continue;
            }
            
            // Convert grid index to coordinates
            const xIdx = tileIdx % gridWidth;
            const yIdx = Math.floor(tileIdx / gridWidth);
            const tileX = xIdx * 64;
            const tileY = yIdx * 64;
            
            // Get tile pixel data
            const tileDataPtr = wasmModule._prog_get_tile_data(progCtx, actualSurfaceId, xIdx, yIdx);
            if (!tileDataPtr) {
                continue;
            }
            
            // Calculate actual tile dimensions (may be clipped at edges)
            const tileW = Math.min(64, surface.width - tileX);
            const tileH = Math.min(64, surface.height - tileY);
            
            // Create ImageData from WASM memory
            // IMPORTANT: Use wasmModule.HEAPU8.buffer directly - Emscripten updates this after memory growth
            // We must copy the data because WASM memory can be resized/detached
            const tileSize = 64 * 64 * 4;
            
            // Safety check: ensure pointer is within WASM memory bounds
            const wasmBuffer = wasmModule.HEAPU8.buffer;
            if (tileDataPtr + tileSize > wasmBuffer.byteLength) {
                console.error(`[GFX Worker] Tile data pointer out of bounds: ${tileDataPtr} + ${tileSize} > ${wasmBuffer.byteLength}`);
                continue;
            }
            
            // Create a fresh view and immediately copy to avoid stale buffer issues
            const pixelData = new Uint8ClampedArray(wasmBuffer, tileDataPtr, tileSize);
            const copiedData = new Uint8ClampedArray(tileSize);
            copiedData.set(pixelData);
            
            const imageData = new ImageData(copiedData, 64, 64);
            
            // Draw tile to surface
            surface.ctx.putImageData(imageData, tileX, tileY, 0, 0, tileW, tileH);
            
            tilesDrawn++;
        }
        
        if (updatedCount > 0 && tilesDrawn === 0) {
            console.warn(`[GFX Worker] Progressive: 0/${updatedCount} tiles drawn to surface ${msg.surfaceId}`);
        }
    } catch (e) {
        console.error(`[GFX Worker] Progressive tile loop error at tile ${tilesDrawn}:`, e);
    }
}

/**
 * Decode and draw a ClearCodec tile using WASM decoder
 */
function decodeClearCodecTile(msg) {
    if (!clearWasmReady || !clearCtx) {
        console.warn('[GFX Worker] ClearCodec decoder not ready, skipping tile');
        return;
    }
    
    const surface = surfaces.get(msg.surfaceId);
    if (!surface) {
        console.warn(`[GFX Worker] ClearCodec: Unknown surface ${msg.surfaceId}`);
        return;
    }
    
    // Track that this surface was updated in the current frame
    frameUpdatedSurfaces.add(msg.surfaceId);
    
    const payload = msg.payload;
    const w = msg.w;
    const h = msg.h;
    
    // Allocate WASM memory for input
    const inputPtr = clearWasmModule._malloc(payload.byteLength);
    clearWasmModule.HEAPU8.set(payload, inputPtr);
    
    // Allocate output buffer (RGBA = 4 bytes per pixel)
    const outputSize = w * h * 4;
    const outputPtr = clearWasmModule._clear_alloc_output(w, h);
    
    if (!outputPtr) {
        console.error('[GFX Worker] ClearCodec: Failed to allocate output buffer');
        clearWasmModule._free(inputPtr);
        return;
    }
    
    // Decode ClearCodec to RGBA output
    // clear_decompress(ctx, srcData, srcSize, nWidth, nHeight, dstData, dstStep, nXDst, nYDst, dstWidth, dstHeight)
    const result = clearWasmModule._clear_decompress(
        clearCtx,
        inputPtr, payload.byteLength,
        w, h,
        outputPtr, w * 4,  // dstStep = width * 4
        0, 0,              // decode at origin of output buffer
        w, h               // output buffer dimensions = tile dimensions
    );
    
    clearWasmModule._free(inputPtr);
    
    if (result < 0) {
        console.warn(`[GFX Worker] ClearCodec decode failed: ${result}`);
        clearWasmModule._clear_free_output(outputPtr);
        return;
    }
    
    // Copy decoded pixels from WASM memory
    const wasmBuffer = clearWasmModule.HEAPU8.buffer;
    if (outputPtr + outputSize > wasmBuffer.byteLength) {
        console.error(`[GFX Worker] ClearCodec output pointer out of bounds`);
        clearWasmModule._clear_free_output(outputPtr);
        return;
    }
    
    const pixelData = new Uint8ClampedArray(wasmBuffer, outputPtr, outputSize);
    const copiedData = new Uint8ClampedArray(outputSize);
    copiedData.set(pixelData);
    
    // Free WASM output buffer
    clearWasmModule._clear_free_output(outputPtr);
    
    // Create ImageData and draw to surface
    const imageData = new ImageData(copiedData, w, h);
    surface.ctx.putImageData(imageData, msg.x, msg.y);
    
}

/**
 * Decode and draw a WebP tile
 */
async function decodeWebPTile(msg) {
    const surface = surfaces.get(msg.surfaceId);
    if (!surface) {
        console.warn(`[GFX Worker] Unknown surface ${msg.surfaceId}`);
        return;
    }
    
    // Track that this surface was updated in the current frame
    frameUpdatedSurfaces.add(msg.surfaceId);
    
    pendingOps++;
    
    try {
        const blob = new Blob([msg.payload], { type: 'image/webp' });
        const bitmap = await createImageBitmap(blob);
        
        // Ensure copy mode for WebP tiles
        surface.ctx.globalCompositeOperation = 'copy';
        surface.ctx.drawImage(bitmap, msg.x, msg.y, msg.w, msg.h);
        bitmap.close();
        
    } catch (err) {
        console.warn('[GFX Worker] WebP decode failed:', err);
    }
    
    pendingOps--;
    checkBackpressure();
}

/**
 * Draw a raw RGBA tile
 */
function drawRawTile(msg) {
    const surface = surfaces.get(msg.surfaceId);
    if (!surface) return;
    
    // Track that this surface was updated in the current frame
    frameUpdatedSurfaces.add(msg.surfaceId);
    
    const imageData = new ImageData(
        new Uint8ClampedArray(msg.payload.buffer, msg.payload.byteOffset, msg.payload.byteLength),
        msg.w, msg.h
    );
    
    surface.ctx.putImageData(imageData, msg.x, msg.y);
    
}

/**
 * Draw a legacy full-frame image (WebP or JPEG without wire format wrapper)
 * Used for backwards compatibility with older backend messages
 */
async function drawLegacyFullFrame(bytes, mimeType) {
    if (!primaryCanvas || !primaryCtx) {
        console.warn('[GFX Worker] No primary canvas for legacy frame');
        return;
    }
    
    pendingOps++;
    
    try {
        const blob = new Blob([bytes], { type: mimeType });
        const bitmap = await createImageBitmap(blob);
        
        // Ensure copy mode for legacy frames
        primaryCtx.globalCompositeOperation = 'copy';
        primaryCtx.drawImage(bitmap, 0, 0, primaryCanvas.width, primaryCanvas.height);
        bitmap.close();
        
    } catch (err) {
        console.warn('[GFX Worker] Legacy frame decode failed:', err);
    }
    
    pendingOps--;
    checkBackpressure();
}

// ============================================================================
// H.264 decoding
// ============================================================================

/**
 * Initialize H.264 VideoDecoder
 */
async function initH264(width, height) {
    if (typeof VideoDecoder === 'undefined') {
        console.warn('[GFX Worker] VideoDecoder not available');
        return false;
    }
    
    try {
        const config = {
            codec: 'avc1.64001f',
            codedWidth: width,
            codedHeight: height,
            optimizeForLatency: true,
            hardwareAcceleration: 'prefer-hardware',
        };
        
        const support = await VideoDecoder.isConfigSupported(config);
        if (!support.supported) {
            console.warn('[GFX Worker] H.264 config not supported');
            return false;
        }
        
        videoDecoder = new VideoDecoder({
            output: (frame) => {
                const meta = h264DecodeQueue.shift();
                if (meta && primaryCtx) {
                    // Ensure copy mode for H.264 frames
                    primaryCtx.globalCompositeOperation = 'copy';
                    primaryCtx.drawImage(frame, 
                        meta.destX, meta.destY, meta.destW, meta.destH,
                        meta.destX, meta.destY, meta.destW, meta.destH);
                    // Resolve the promise to signal decode complete
                    if (meta.resolve) meta.resolve();
                }
                frame.close();
                h264DecoderError = false;
                pendingOps--;
                checkBackpressure();
            },
            error: (e) => {
                console.error('[GFX Worker] H.264 decoder error callback:', e);
                h264DecoderError = true;
                h264NeedsKeyframe = true;  // Need keyframe after error
                // Reject all pending decodes
                while (h264DecodeQueue.length > 0) {
                    const meta = h264DecodeQueue.shift();
                    if (meta.reject) meta.reject(e);
                    pendingOps--;
                }
            }
        });
        
        await videoDecoder.configure(config);
        h264Initialized = true;
        h264NeedsKeyframe = true;  // Must receive keyframe after configure
        h264ConfiguredWidth = width;
        h264ConfiguredHeight = height;
        console.log(`[GFX Worker] H.264 decoder initialized: ${width}x${height}`);
        return true;
    } catch (e) {
        console.warn('[GFX Worker] H.264 init failed:', e);
        return false;
    }
}

/**
 * Decode H.264 video frame
 */
async function decodeH264Frame(msg) {
    if (!primaryCtx) {
        console.warn('[GFX Worker] No primary context for H.264');
        return;
    }
    
    const width = primaryCanvas?.width || 1920;
    const height = primaryCanvas?.height || 1080;
    
    // Initialize decoder if needed
    if (!h264Initialized) {
        const success = await initH264(width, height);
        if (!success) return;
    }
    
    // Reinitialize if dimensions changed
    if (h264ConfiguredWidth !== width || h264ConfiguredHeight !== height) {
        if (videoDecoder) {
            videoDecoder.close();
            h264Initialized = false;
        }
        const success = await initH264(width, height);
        if (!success) return;
    }
    
    if (h264DecoderError) {
        // Reset decoder on next keyframe
        if (msg.frameType === 0) {
            videoDecoder.reset();
            await videoDecoder.configure({
                codec: 'avc1.64001f',
                codedWidth: width,
                codedHeight: height,
                optimizeForLatency: true,
                hardwareAcceleration: 'prefer-hardware',
            });
            h264DecoderError = false;
            h264NeedsKeyframe = true;  // Need keyframe after reset
        } else {
            return; // Wait for keyframe
        }
    }
    
    // After init/reset, we must receive a keyframe before delta frames
    if (h264NeedsKeyframe) {
        if (msg.frameType !== 0) {
            // Skip delta frames until we get a keyframe
            return;
        }
        h264NeedsKeyframe = false;
        console.log('[GFX Worker] H.264 received first keyframe');
    }
    
    pendingOps++;
    
    // Create a promise that resolves when this specific frame is decoded
    const decodePromise = new Promise((resolve, reject) => {
        h264DecodeQueue.push({
            destX: msg.destX,
            destY: msg.destY,
            destW: msg.destW,
            destH: msg.destH,
            resolve,
            reject,
        });
    });
    
    try {
        const chunk = new EncodedVideoChunk({
            type: msg.frameType === 0 ? 'key' : 'delta',
            timestamp: performance.now() * 1000,
            data: msg.nalData,
        });
        
        videoDecoder.decode(chunk);
        
        // Wait for the output callback to fire for this frame.
        // Unlike flush(), this preserves decoder state for delta frames.
        await decodePromise;
    } catch (e) {
        console.error('[GFX Worker] H.264 decode error:', e);
        // Remove our entry from the queue if decode failed
        const idx = h264DecodeQueue.findIndex(m => m.resolve);
        if (idx !== -1) h264DecodeQueue.splice(idx, 1);
        pendingOps--;
    }
}

// ============================================================================
// Surface operations
// ============================================================================

/**
 * Solid fill operation
 */
function applySolidFill(msg) {
    const surface = surfaces.get(msg.surfaceId);
    if (!surface) return;
    
    // Track that this surface was updated in the current frame
    frameUpdatedSurfaces.add(msg.surfaceId);
    
    // Extract BGRA components
    const b = (msg.color >> 0) & 0xFF;
    const g = (msg.color >> 8) & 0xFF;
    const r = (msg.color >> 16) & 0xFF;
    
    surface.ctx.fillStyle = `rgb(${r},${g},${b})`;
    surface.ctx.fillRect(msg.x, msg.y, msg.w, msg.h);
}

/**
 * Surface-to-surface blit (for scrolling)
 */
function applySurfaceToSurface(msg) {
    const srcSurface = surfaces.get(msg.srcSurfaceId);
    const dstSurface = surfaces.get(msg.dstSurfaceId);
    if (!srcSurface || !dstSurface) return;
    
    // Track that destination surface was updated in the current frame
    frameUpdatedSurfaces.add(msg.dstSurfaceId);
    
    // Ensure copy mode for surface-to-surface blit
    dstSurface.ctx.globalCompositeOperation = 'copy';
    dstSurface.ctx.drawImage(
        srcSurface.canvas,
        msg.srcX, msg.srcY, msg.srcW, msg.srcH,
        msg.dstX, msg.dstY, msg.srcW, msg.srcH
    );
}

/**
 * Surface-to-cache: extract region from surface and store in local cache
 */
function applySurfaceToCache(msg) {
    const surface = surfaces.get(msg.surfaceId);
    if (!surface) {
        console.warn(`[CACHE] SurfaceToCache: unknown surface ${msg.surfaceId}`);
        return;
    }
    
    // Extract pixel data from surface
    const imageData = surface.ctx.getImageData(msg.x, msg.y, msg.w, msg.h);
    
    // Store in cache
    const entry = {
        imageData: imageData,
        sourceSurface: msg.surfaceId,
        sourceRect: { x: msg.x, y: msg.y, w: msg.w, h: msg.h },
        frameId: currentFrameId
    };
    bitmapCache.set(msg.cacheSlot, entry);
}

/**
 * Cache-to-surface blit: retrieve cached bitmap and draw to surface
 */
function applyCacheToSurface(msg) {
    const surface = surfaces.get(msg.surfaceId);
    if (!surface) {
        console.warn(`[CACHE] CacheToSurface: unknown surface ${msg.surfaceId}`);
        return;
    }
    
    const entry = bitmapCache.get(msg.cacheSlot);
    if (!entry) {
        console.error(`[CACHE] C2S: MISS slot=${msg.cacheSlot} -> surface=${msg.surfaceId}(${msg.dstX},${msg.dstY}) frame=${currentFrameId}`);
        return;
    }
    
    // Track that this surface was updated in the current frame
    frameUpdatedSurfaces.add(msg.surfaceId);
    
    // Draw cached bitmap to surface at destination position
    surface.ctx.putImageData(entry.imageData, msg.dstX, msg.dstY);
}

/**
 * Evict a cache slot (server tells us it's no longer valid)
 * Per RDPGFX protocol, the server sends EvictCacheEntry PDU when it
 * wants to invalidate a cache slot.
 */
function applyEvictCache(msg) {
    bitmapCache.delete(msg.cacheSlot);
}

/**
 * Reset all cache state (called on session reset/reconnect)
 */
function resetCacheState() {
    const cacheSize = bitmapCache.size;
    bitmapCache.clear();
    if (cacheSize > 0) {
        console.log(`[GFX Worker] Reset: cleared ${cacheSize} cache entries`);
    }
}

/**
 * Handle resetGraphics from server
 * Reset surfaces and progressive state, but NOT the bitmap cache.
 * Per RDPGFX protocol, bitmap cache persists across reset graphics.
 * Server sends new dimensions; browser should resize accordingly.
 */
function applyResetGraphics(msg) {
    const surfaceCount = surfaces.size;
    
    // Delete all surfaces (this also clears WASM progressive state per surface)
    for (const surfaceId of surfaces.keys()) {
        deleteSurface(surfaceId);
    }
    
    // NOTE: Do NOT clear bitmap cache here!
    // The GFX bitmap cache is session-level and persists across surface resets.
    // Only explicit EvictCacheEntry PDU should remove cache entries.
    
    // Reset frame tracking
    currentFrameId = null;
    frameUpdatedSurfaces.clear();
    
    // Reset H.264 decoder state
    if (videoDecoder && h264Initialized) {
        try {
            videoDecoder.reset();
            h264NeedsKeyframe = true;
        } catch (e) {
            // Ignore reset errors
        }
    }
    h264DecoderError = false;
    h264DecodeQueue = [];
    
    // Reset ClearCodec sequence number (caches are NOT reset per MS-RDPEGFX)
    if (clearWasmReady && clearCtx) {
        clearWasmModule._clear_context_reset(clearCtx);
    }
    
    // Update primary canvas size if needed
    if (primaryCanvas && (primaryCanvas.width !== msg.width || primaryCanvas.height !== msg.height)) {
        primaryCanvas.width = msg.width;
        primaryCanvas.height = msg.height;
        
        // Per MS-RDPEGFX spec: re-apply compositor settings after resize
        // (canvas resize clears the context state)
        primaryCtx.globalCompositeOperation = 'copy';
        primaryCtx.imageSmoothingEnabled = false;
        primaryCtx.fillStyle = '#000000';
        primaryCtx.fillRect(0, 0, msg.width, msg.height);
    }
    
    console.log(`[GFX Worker] ResetGraphics: cleared ${surfaceCount} surfaces, new size ${msg.width}x${msg.height}`);
}

// ============================================================================
// Capability Confirmation
// ============================================================================

// GFX Version constants (MS-RDPEGFX 2.2.3.1)
const RDPGFX_CAPVERSION = {
    8:    0x00080004,   // Version 8.0
    81:   0x00080105,   // Version 8.1
    10:   0x000A0002,   // Version 10.0
    101:  0x000A0100,   // Version 10.1
    102:  0x000A0200,   // Version 10.2
    103:  0x000A0301,   // Version 10.3
    104:  0x000A0400,   // Version 10.4
    105:  0x000A0502,   // Version 10.5
    106:  0x000A0600,   // Version 10.6
    107:  0x000A0701,   // Version 10.7
};

// GFX Capability flags (MS-RDPEGFX 2.2.3.1)
const RDPGFX_CAPS_FLAG = {
    THINCLIENT:      0x00000001,
    SMALL_CACHE:     0x00000002,
    AVC420_ENABLED:  0x00000010,
    AVC_DISABLED:    0x00000020,
    AVC_THINCLIENT:  0x00000040,
};

/**
 * Handle capsConfirm from server
 * Log the negotiated GFX capabilities in a human-readable format
 */
function applyCapsConfirm(msg) {
    const version = msg.version;
    const flags = msg.flags;
    
    // Decode version to string
    let versionStr = 'Unknown';
    let h264Supported = false;
    switch (version) {
        case RDPGFX_CAPVERSION[8]:   versionStr = '8.0'; break;
        case RDPGFX_CAPVERSION[81]:  versionStr = '8.1'; break;
        case RDPGFX_CAPVERSION[10]:  versionStr = '10.0'; h264Supported = true; break;
        case RDPGFX_CAPVERSION[101]: versionStr = '10.1'; h264Supported = true; break;
        case RDPGFX_CAPVERSION[102]: versionStr = '10.2'; h264Supported = true; break;
        case RDPGFX_CAPVERSION[103]: versionStr = '10.3'; h264Supported = true; break;
        case RDPGFX_CAPVERSION[104]: versionStr = '10.4'; h264Supported = true; break;
        case RDPGFX_CAPVERSION[105]: versionStr = '10.5'; h264Supported = true; break;
        case RDPGFX_CAPVERSION[106]: versionStr = '10.6'; h264Supported = true; break;
        case RDPGFX_CAPVERSION[107]: versionStr = '10.7'; h264Supported = true; break;
    }
    
    // Decode flags
    const thinClient = (flags & RDPGFX_CAPS_FLAG.THINCLIENT) ? 'Active' : 'Inactive';
    const smallCache = (flags & RDPGFX_CAPS_FLAG.SMALL_CACHE) ? 'Active' : 'Inactive';
    const avc420Enabled = (flags & RDPGFX_CAPS_FLAG.AVC420_ENABLED) ? 'Enabled' : 'Disabled';
    const avcDisabled = (flags & RDPGFX_CAPS_FLAG.AVC_DISABLED) ? 'YES!' : 'No';
    const avcThinClient = (flags & RDPGFX_CAPS_FLAG.AVC_THINCLIENT) ? 'Active' : 'Inactive';
    
    // Codec availability
    const progressiveSupported = version >= RDPGFX_CAPVERSION[81];
    const clearCodecSupported = version >= RDPGFX_CAPVERSION[8];
    const h264Available = h264Supported && !(flags & RDPGFX_CAPS_FLAG.AVC_DISABLED);
    
    // Log in box format similar to backend
    console.log(`
┌──────────────────────────────────────────────────────────────┐
│ Server CapsConfirm (Frontend)                                │
├──────────────────────────────────────────────────────────────┤
│   Version: ${versionStr.padEnd(8)} (0x${version.toString(16).padStart(8, '0').toUpperCase()})                             │
│   Flags:   0x${flags.toString(16).padStart(8, '0').toUpperCase()}                                        │
├──────────────────────────────────────────────────────────────┤
│ Flag Breakdown                                               │
│   Thin Client Mode:   ${thinClient.padEnd(8)}  (limited graphics if Active) │
│   Small Cache:        ${smallCache.padEnd(8)}  (reduced tile cache)         │
│   H.264 AVC420:       ${avc420Enabled.padEnd(8)}  (4:2:0 chroma subsampling)   │
│   H.264 Blocked:      ${avcDisabled.padEnd(8)}  (AVC_DISABLED flag)          │
│   AVC Thin Client:    ${avcThinClient.padEnd(8)}  (reduced H.264 quality)      │
├──────────────────────────────────────────────────────────────┤
│ Codec Availability                                           │
│   H.264/AVC:   ${(h264Available ? 'YES' : 'NO').padEnd(6)}   AVC420:      ${(flags & RDPGFX_CAPS_FLAG.AVC420_ENABLED ? 'YES' : 'NO').padEnd(6)}                  │
│   Progressive: ${(progressiveSupported ? 'YES' : 'NO').padEnd(6)}   ClearCodec:  ${(clearCodecSupported ? 'YES' : 'NO').padEnd(6)}                  │
└──────────────────────────────────────────────────────────────┘
`);
    
    // Store for potential future use
    self.gfxCaps = { version, versionStr, flags, h264Supported, h264Available, progressiveSupported, clearCodecSupported };
}

// ============================================================================
// Frame lifecycle
// ============================================================================

/**
 * Composite a surface to the primary canvas
 * Used for Progressive frames that come via H264 queue (no StartFrame/EndFrame)
 */
function compositeSurfaceToPrimary(surfaceId) {
    if (!primaryCanvas || !primaryCtx) return;
    
    const surface = surfaces.get(surfaceId);
    if (!surface) return;
    
    // Ensure copy mode for surface-to-primary composition
    primaryCtx.globalCompositeOperation = 'copy';
    // Draw the surface to the primary canvas
    primaryCtx.drawImage(surface.canvas, 0, 0);
}

/**
 * Start a new frame
 */
function startFrame(frameId) {
    currentFrameId = frameId;
    frameUpdatedSurfaces.clear();
}

/**
 * End frame and send acknowledgment
 */
function endFrame(frameId) {
    if (currentFrameId !== frameId) {
        console.warn(`[GFX Worker] Frame mismatch: expected ${currentFrameId}, got ${frameId}`);
    }
    
    // Composite all surfaces that were updated in this frame to primary canvas
    if (primaryCanvas && primaryCtx) {
        // Ensure copy mode for surface-to-primary composition (no alpha blending)
        primaryCtx.globalCompositeOperation = 'copy';
        
        // First try primarySurfaceId if set and was updated
        if (primarySurfaceId !== null && frameUpdatedSurfaces.has(primarySurfaceId)) {
            const surface = surfaces.get(primarySurfaceId);
            if (surface) {
                primaryCtx.drawImage(surface.canvas, 0, 0);
            }
        } else if (frameUpdatedSurfaces.size > 0) {
            // Fallback: composite any updated surfaces (in order of surface ID)
            const sortedSurfaces = Array.from(frameUpdatedSurfaces).sort((a, b) => a - b);
            for (const surfaceId of sortedSurfaces) {
                const surface = surfaces.get(surfaceId);
                if (surface) {
                    primaryCtx.drawImage(surface.canvas, 0, 0);
                } else {
                    console.warn(`[GFX Worker] EndFrame: surface ${surfaceId} was updated but no longer exists`);
                }
            }
        }
    } else {
        console.warn(`[GFX Worker] EndFrame: No primary canvas! primaryCanvas=${!!primaryCanvas} primaryCtx=${!!primaryCtx}`);
    }
    
    // Track last completed frame for skip detection
    lastCompletedFrameId = frameId;
    
    currentFrameId = null;
    frameUpdatedSurfaces.clear();
    
    // Increment decoded counter and send frame acknowledgment
    totalFramesDecoded++;
    const ackMsg = buildFrameAck(frameId, totalFramesDecoded);
    self.postMessage({ type: 'frameAck', frameId, totalFramesDecoded, data: ackMsg.buffer }, [ackMsg.buffer]);
}

// ============================================================================
// Backpressure management
// ============================================================================

let lastBackpressureLevel = 0;

function checkBackpressure() {
    let level = 0;
    
    if (pendingOps > MAX_PENDING_OPS / 2) {
        level = 1;
    }
    if (pendingOps > MAX_PENDING_OPS) {
        level = 2;
    }
    
    if (level !== lastBackpressureLevel) {
        lastBackpressureLevel = level;
        const msg = buildBackpressure(level);
        self.postMessage({ type: 'backpressure', level, data: msg.buffer }, [msg.buffer]);
    }
}

// ============================================================================
// Message handling
// ============================================================================

/**
 * Handle binary message from main thread
 */
async function handleBinaryMessage(data) {
    const bytes = new Uint8Array(data);
    const msg = parseMessage(bytes);
    
    if (!msg) {
        // Unknown message type - check for legacy WebP/JPEG full frame
        // WebP starts with RIFF, JPEG starts with FFD8
        if (bytes.length > 12) {
            const isWebP = bytes[0] === 0x52 && bytes[1] === 0x49 && 
                           bytes[2] === 0x46 && bytes[3] === 0x46; // "RIFF"
            const isJpeg = bytes[0] === 0xFF && bytes[1] === 0xD8;
            
            if (isWebP || isJpeg) {
                await drawLegacyFullFrame(bytes, isWebP ? 'image/webp' : 'image/jpeg');
                return true;
            }
        }
        // Debug: log unknown message magic
        const magic = String.fromCharCode(bytes[0], bytes[1], bytes[2], bytes[3]);
        console.warn(`[GFX Worker] Unknown message magic: "${magic}" (${bytes[0].toString(16)} ${bytes[1].toString(16)} ${bytes[2].toString(16)} ${bytes[3].toString(16)})`);
        return false;
    }
    
    // Frame lifecycle is now tracked internally - no per-frame logging needed
    
    // Cache operations are now logged in applySurfaceToCache/applyCacheToSurface with enhanced debugging
    
    switch (msg.type) {
        case 'createSurface':
            createSurface(msg.surfaceId, msg.width, msg.height, msg.format);
            break;
            
        case 'deleteSurface':
            deleteSurface(msg.surfaceId);
            break;
        
        case 'mapSurface':
            console.log(`[GFX Worker] MapSurfaceToOutput: surfaceId=${msg.surfaceId}, outputX=${msg.outputX}, outputY=${msg.outputY}`);
            mapSurfaceToPrimary(msg.surfaceId);
            break;
            
        case 'startFrame':
            startFrame(msg.frameId);
            break;
            
        case 'endFrame':
            endFrame(msg.frameId);
            break;
            
        case 'tile':
            if (msg.codec === 'progressive') {
                decodeProgressiveTile(msg);
            } else if (msg.codec === 'webp') {
                await decodeWebPTile(msg);
            } else if (msg.codec === 'raw') {
                drawRawTile(msg);
            } else if (msg.codec === 'clearcodec') {
                decodeClearCodecTile(msg);
            } else {
                console.warn(`[GFX Worker] Unknown tile codec: "${msg.codec}", surfaceId=${msg.surfaceId}, x=${msg.x}, y=${msg.y}`);
            }
            break;
            
        case 'solidFill':
            applySolidFill(msg);
            break;
            
        case 'surfaceToSurface':
            applySurfaceToSurface(msg);
            break;
            
        case 'surfaceToCache':
            applySurfaceToCache(msg);
            break;
            
        case 'cacheToSurface':
            applyCacheToSurface(msg);
            break;
            
        case 'evictCache':
            applyEvictCache(msg);
            break;
            
        case 'resetGraphics':
            applyResetGraphics(msg);
            break;
            
        case 'capsConfirm':
            applyCapsConfirm(msg);
            break;
            
        case 'videoFrame':
            // Route by codec ID: Progressive goes to WASM decoder, H.264 to VideoDecoder
            if (msg.codecId === CODEC_ID.PROGRESSIVE || msg.codecId === CODEC_ID.PROGRESSIVE_V2) {
                // Progressive/RFX: synchronous WASM decode - strict ordering maintained
                decodeProgressiveTile({
                    surfaceId: msg.surfaceId,
                    frameId: msg.frameId,
                    x: msg.destX,
                    y: msg.destY,
                    w: msg.destW,
                    h: msg.destH,
                    payload: msg.nalData,  // Progressive raw data passed as nalData
                });
                
                // Track updated surface for endFrame composition
                frameUpdatedSurfaces.add(msg.surfaceId);
                currentFrameId = msg.frameId;
            } else {
                // H.264/AVC420/AVC444 - decode in worker using VideoDecoder
                await decodeH264Frame(msg);
            }
            break;
            
        default:
            console.warn('[GFX Worker] Unhandled message type:', msg.type);
            return false;
    }
    
    return true;
}

// ============================================================================
// Worker entry point
// ============================================================================

/**
 * Process a single message (called sequentially from queue).
 * This function handles the actual message processing.
 */
async function processMessage(event) {
    const { type, data } = event;
    
    switch (type) {
        case 'init':
            // Initialize with primary canvas (sync, no queue needed)
            primaryCanvas = data.canvas;
            primaryCtx = primaryCanvas.getContext('2d', { 
                alpha: false,
                desynchronized: true,
                willReadFrequently: true  // For getImageData operations
            });
            
            // Per MS-RDPEGFX spec: compositor must use direct copy for final output
            primaryCtx.globalCompositeOperation = 'copy';
            primaryCtx.imageSmoothingEnabled = false;
            
            // Fill with black initially
            primaryCtx.fillStyle = '#000000';
            primaryCtx.fillRect(0, 0, primaryCanvas.width, primaryCanvas.height);
            
            // Don't pre-create surface 0 here - let the server create surfaces
            // via CreateSurface messages. Pre-creating causes conflicts when 
            // server sends its own CreateSurface(0) which then gets deleted.
            // primarySurfaceId will be set when server maps a surface to output.
            
            // WASM already loaded during worker startup
            self.postMessage({ type: 'ready', wasmReady });
            break;
            
        case 'binary':
            // Binary message from WebSocket - process in order
            if (data instanceof ArrayBuffer) {
                const handled = await handleBinaryMessage(data);
                if (!handled) {
                    // Forward unhandled to main thread
                    self.postMessage({ type: 'unhandled', data }, [data]);
                }
            }
            break;
            
        case 'resize':
            // Handle canvas/surface resize
            if (primaryCanvas) {
                // Only resize if dimensions actually changed
                if (primaryCanvas.width !== data.width || primaryCanvas.height !== data.height) {
                    primaryCanvas.width = data.width;
                    primaryCanvas.height = data.height;
                    
                    // Per MS-RDPEGFX spec: re-apply compositor settings after resize
                    // (canvas resize clears the context state)
                    primaryCtx.globalCompositeOperation = 'copy';
                    primaryCtx.imageSmoothingEnabled = false;
                    primaryCtx.fillStyle = '#000000';
                    primaryCtx.fillRect(0, 0, data.width, data.height);
                    
                    console.log(`[GFX Worker] Primary canvas resized to ${data.width}x${data.height}`);
                }
            }
            if (primarySurfaceId !== null) {
                const existing = surfaces.get(primarySurfaceId);
                if (!existing || existing.canvas.width !== data.width || existing.canvas.height !== data.height) {
                    createSurface(primarySurfaceId, data.width, data.height);
                }
            }
            break;
            
        case 'mapSurface':
            // Map a surface to primary output
            mapSurfaceToPrimary(data.surfaceId);
            break;
            
        case 'reset':
            // Reset all state (e.g., on reconnect or session reset)
            resetCacheState();
            // Clear all surfaces
            for (const surfaceId of surfaces.keys()) {
                deleteSurface(surfaceId);
            }
            console.log('[GFX Worker] Session reset complete');
            break;
            
        default:
            console.warn('[GFX Worker] Unknown message type:', type);
    }
}

/**
 * Worker onmessage handler - enqueues messages for strict sequential processing.
 * This ensures that async operations (H.264 decode, WebP decode) complete
 * before the next message is processed.
 */
self.onmessage = (event) => {
    // Enqueue message for sequential processing
    enqueueMessage(event.data);
};

// ============================================================================
// Worker startup - initialize WASM before reporting ready
// ============================================================================

(async () => {
    // Load WASM decoders at worker startup so we know immediately if they work
    await initWasm();
    await initClearCodecWasm();
    
    // Report that worker is loaded with WASM status
    self.postMessage({ type: 'loaded', wasmReady, clearWasmReady });
})();
