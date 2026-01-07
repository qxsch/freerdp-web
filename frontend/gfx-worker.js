/**
 * GFX Compositor Worker
 * 
 * Handles off-main-thread rendering of RDP graphics:
 * - Progressive tile decoding via WASM
 * - WebP tile decoding via createImageBitmap
 * - Surface management and composition
 * - Frame lifecycle and acknowledgment
 */

const BUILD_VERSION = '__BUILD_TIME__';
console.log(`[GFX-WORKER] ===== BUILD ${BUILD_VERSION} =====`);

import {
    Magic, matchMagic, parseMessage,
    readU16LE, readU32LE, buildFrameAck, buildBackpressure
} from './wire-format.js';

// ============================================================================
// State
// ============================================================================

/** @type {Object|null} WASM module instance */
let wasmModule = null;

/** @type {number|null} Progressive decoder context pointer */
let progCtx = null;

/** @type {Map<number, Surface>} Surface ID → surface state */
const surfaces = new Map();

/** @type {OffscreenCanvas|null} Primary render target */
let primaryCanvas = null;

/** @type {OffscreenCanvasRenderingContext2D|null} Primary 2D context */
let primaryCtx = null;

/** @type {number|null} Primary surface ID (mapped to output) */
let primarySurfaceId = null;

/** @type {number|null} Current frame being processed */
let currentFrameId = null;

/** @type {boolean} Whether WASM is ready */
let wasmReady = false;

/** @type {number} Pending operations count for backpressure */
let pendingOps = 0;

/** @type {number} Max pending ops before backpressure */
const MAX_PENDING_OPS = 64;

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
 * Create a new surface
 */
function createSurface(surfaceId, width, height) {
    // Delete existing if present
    if (surfaces.has(surfaceId)) {
        deleteSurface(surfaceId);
    }
    
    const canvas = new OffscreenCanvas(width, height);
    const ctx = canvas.getContext('2d', { 
        alpha: false,
        desynchronized: true  // Reduce latency
    });
    
    // Fill with black initially
    ctx.fillStyle = '#000000';
    ctx.fillRect(0, 0, width, height);
    
    surfaces.set(surfaceId, {
        id: surfaceId,
        width,
        height,
        canvas,
        ctx
    });
    
    // Create surface in WASM progressive decoder
    if (wasmReady && progCtx) {
        wasmModule._prog_create_surface(progCtx, surfaceId, width, height);
    }
    
    console.log(`[GFX Worker] Created surface ${surfaceId}: ${width}x${height}`);
}

/**
 * Delete a surface
 */
function deleteSurface(surfaceId) {
    const surface = surfaces.get(surfaceId);
    if (!surface) return;
    
    surfaces.delete(surfaceId);
    
    // Delete from WASM
    if (wasmReady && progCtx) {
        wasmModule._prog_delete_surface(progCtx, surfaceId);
    }
    
    if (primarySurfaceId === surfaceId) {
        primarySurfaceId = null;
    }
    
    console.log(`[GFX Worker] Deleted surface ${surfaceId}`);
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
            }
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
    
    const surface = surfaces.get(msg.surfaceId);
    if (!surface) {
        console.warn(`[GFX Worker] Unknown surface ${msg.surfaceId}`);
        return;
    }
    
    const payload = msg.payload;
    
    // Allocate WASM memory for input
    const inputPtr = wasmModule._malloc(payload.byteLength);
    wasmModule.HEAPU8.set(payload, inputPtr);
    
    // Use parallel decompress when available for better performance with many tiles
    let result;
    if (parallelDecompressAvailable) {
        result = wasmModule._prog_decompress_parallel(
            progCtx, inputPtr, payload.byteLength, 
            msg.surfaceId, msg.frameId
        );
    } else {
        result = wasmModule._prog_decompress(
            progCtx, inputPtr, payload.byteLength, 
            msg.surfaceId, msg.frameId
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
            const tileDataPtr = wasmModule._prog_get_tile_data(progCtx, msg.surfaceId, xIdx, yIdx);
            if (!tileDataPtr) {
                continue;
            }
            
            // Calculate actual tile dimensions (may be clipped at edges)
            const tileW = Math.min(64, surface.width - tileX);
            const tileH = Math.min(64, surface.height - tileY);
            
            // Create ImageData from WASM memory
            const tileSize = 64 * 64 * 4;
            const pixelData = new Uint8ClampedArray(
                wasmModule.HEAPU8.buffer, tileDataPtr, tileSize
            );
            
            // Note: we need to copy because the WASM buffer may be detached/resized
            const imageData = new ImageData(
                new Uint8ClampedArray(pixelData), 64, 64
            );
            
            // Draw tile to surface
            surface.ctx.putImageData(imageData, tileX, tileY, 0, 0, tileW, tileH);
            tilesDrawn++;
        }
    } catch (e) {
        console.error(`[GFX Worker] Progressive tile loop error at tile ${tilesDrawn}:`, e);
    }
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
    
    pendingOps++;
    
    try {
        const blob = new Blob([msg.payload], { type: 'image/webp' });
        const bitmap = await createImageBitmap(blob);
        
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
        
        primaryCtx.drawImage(bitmap, 0, 0, primaryCanvas.width, primaryCanvas.height);
        bitmap.close();
        
    } catch (err) {
        console.warn('[GFX Worker] Legacy frame decode failed:', err);
    }
    
    pendingOps--;
    checkBackpressure();
}

/**
 * Decode and draw a legacy delta frame (multiple WebP tiles)
 * Used for backwards compatibility with DELT format
 */
async function decodeDeltaFrame(msg) {
    if (!primaryCanvas || !primaryCtx) {
        console.warn('[GFX Worker] No primary canvas for delta frame');
        return;
    }
    
    if (!msg.tiles || msg.tiles.length === 0) {
        return;
    }
    
    pendingOps += msg.tiles.length;
    
    const decodePromises = msg.tiles.map(async (tile) => {
        try {
            const blob = new Blob([tile.data], { type: 'image/webp' });
            const bitmap = await createImageBitmap(blob);
            
            primaryCtx.drawImage(bitmap, tile.x, tile.y);
            bitmap.close();
            
        } catch (err) {
            console.warn('[GFX Worker] Delta tile decode failed:', err);
        }
        
        pendingOps--;
    });
    
    await Promise.all(decodePromises);
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
                    primaryCtx.drawImage(frame, 
                        meta.destX, meta.destY, meta.destW, meta.destH,
                        meta.destX, meta.destY, meta.destW, meta.destH);
                }
                frame.close();
                h264DecoderError = false;
                pendingOps--;
                checkBackpressure();
            },
            error: (e) => {
                console.error('[GFX Worker] H.264 decode error:', e);
                h264DecoderError = true;
                h264DecodeQueue = [];
                pendingOps--;
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
    
    // Queue metadata for output callback
    h264DecodeQueue.push({
        destX: msg.destX,
        destY: msg.destY,
        destW: msg.destW,
        destH: msg.destH,
    });
    
    try {
        const chunk = new EncodedVideoChunk({
            type: msg.frameType === 0 ? 'key' : 'delta',
            timestamp: performance.now() * 1000,
            data: msg.nalData,
        });
        
        videoDecoder.decode(chunk);
    } catch (e) {
        console.error('[GFX Worker] H.264 decode error:', e);
        h264DecodeQueue.pop();
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
    
    dstSurface.ctx.drawImage(
        srcSurface.canvas,
        msg.srcX, msg.srcY, msg.srcW, msg.srcH,
        msg.dstX, msg.dstY, msg.srcW, msg.srcH
    );
}

/**
 * Cache-to-surface blit
 * Note: Cache management would need additional state tracking
 */
function applyCacheToSurface(msg) {
    // TODO: Implement bitmap cache
    console.warn('[GFX Worker] CacheToSurface not yet implemented');
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
    
    // Draw the surface to the primary canvas
    primaryCtx.drawImage(surface.canvas, 0, 0);
}

/**
 * Start a new frame
 */
function startFrame(frameId) {
    currentFrameId = frameId;
}

/**
 * End frame and send acknowledgment
 */
function endFrame(frameId) {
    if (currentFrameId !== frameId) {
        console.warn(`[GFX Worker] Frame mismatch: expected ${currentFrameId}, got ${frameId}`);
    }
    
    // Composite primary surface to output canvas if needed
    if (primarySurfaceId !== null && primaryCanvas && primaryCtx) {
        const surface = surfaces.get(primarySurfaceId);
        if (surface) {
            primaryCtx.drawImage(surface.canvas, 0, 0);
        }
    }
    
    currentFrameId = null;
    
    // Send frame acknowledgment
    const ackMsg = buildFrameAck(frameId);
    self.postMessage({ type: 'frameAck', frameId, data: ackMsg.buffer }, [ackMsg.buffer]);
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
        return false;
    }
    
    switch (msg.type) {
        case 'createSurface':
            createSurface(msg.surfaceId, msg.width, msg.height);
            break;
            
        case 'deleteSurface':
            deleteSurface(msg.surfaceId);
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
            }
            break;
            
        case 'solidFill':
            applySolidFill(msg);
            break;
            
        case 'surfaceToSurface':
            applySurfaceToSurface(msg);
            break;
            
        case 'cacheToSurface':
            applyCacheToSurface(msg);
            break;
            
        case 'deltaFrame':
            await decodeDeltaFrame(msg);
            break;
            
        case 'videoFrame':
            // Route by codec ID: Progressive goes to WASM decoder, H.264 to VideoDecoder
            if (msg.codecId === CODEC_ID.PROGRESSIVE || msg.codecId === CODEC_ID.PROGRESSIVE_V2) {
                // Convert H264 frame format to progressive tile format for decoder
                decodeProgressiveTile({
                    surfaceId: msg.surfaceId,
                    frameId: msg.frameId,
                    x: msg.destX,
                    y: msg.destY,
                    w: msg.destW,
                    h: msg.destH,
                    payload: msg.nalData,  // Progressive raw data passed as nalData
                });
                
                // Progressive frames via H264 queue need manual compositing and ack
                // (normal GFX flow would have StartFrame/EndFrame messages)
                compositeSurfaceToPrimary(msg.surfaceId);
                
                // Send frame acknowledgment for Progressive
                const ackMsg = buildFrameAck(msg.frameId);
                self.postMessage({ type: 'frameAck', frameId: msg.frameId, data: ackMsg.buffer }, [ackMsg.buffer]);
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

self.onmessage = async (event) => {
    const { type, data } = event.data;
    
    switch (type) {
        case 'init':
            // Initialize with primary canvas
            primaryCanvas = data.canvas;
            primaryCtx = primaryCanvas.getContext('2d', { 
                alpha: false,
                desynchronized: true
            });
            
            // Create primary surface matching canvas size
            createSurface(0, data.width, data.height);
            mapSurfaceToPrimary(0);
            
            // WASM already loaded during worker startup
            self.postMessage({ type: 'ready', wasmReady });
            break;
            
        case 'binary':
            // Binary message from WebSocket
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
                primaryCanvas.width = data.width;
                primaryCanvas.height = data.height;
            }
            if (primarySurfaceId !== null) {
                createSurface(primarySurfaceId, data.width, data.height);
            }
            break;
            
        case 'mapSurface':
            // Map a surface to primary output
            mapSurfaceToPrimary(data.surfaceId);
            break;
            
        default:
            console.warn('[GFX Worker] Unknown message type:', type);
    }
};

// ============================================================================
// Worker startup - initialize WASM before reporting ready
// ============================================================================

(async () => {
    // Load WASM at worker startup so we know immediately if it works
    await initWasm();
    
    // Report that worker is loaded with WASM status
    self.postMessage({ type: 'loaded', wasmReady });
})();
