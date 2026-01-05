/**
 * Wire Format Definitions for GFX Event Stream
 * 
 * All messages use 4-byte magic headers followed by binary payload.
 * Little-endian byte order throughout.
 */

// ============================================================================
// Magic codes (4 bytes each)
// ============================================================================

export const Magic = {
    // Surface management
    SURF: new Uint8Array([0x53, 0x55, 0x52, 0x46]),  // "SURF" - createSurface
    DELS: new Uint8Array([0x44, 0x45, 0x4C, 0x53]),  // "DELS" - deleteSurface
    
    // Frame lifecycle
    STFR: new Uint8Array([0x53, 0x54, 0x46, 0x52]),  // "STFR" - startFrame
    ENFR: new Uint8Array([0x45, 0x4E, 0x46, 0x52]),  // "ENFR" - endFrame
    
    // Tile codecs
    PROG: new Uint8Array([0x50, 0x52, 0x4F, 0x47]),  // "PROG" - progressive tile
    WEBP: new Uint8Array([0x57, 0x45, 0x42, 0x50]),  // "WEBP" - WebP tile
    TILE: new Uint8Array([0x54, 0x49, 0x4C, 0x45]),  // "TILE" - raw RGBA tile
    
    // Surface operations
    SFIL: new Uint8Array([0x53, 0x46, 0x49, 0x4C]),  // "SFIL" - solidFill
    S2SF: new Uint8Array([0x53, 0x32, 0x53, 0x46]),  // "S2SF" - surfaceToSurface
    C2SF: new Uint8Array([0x43, 0x32, 0x53, 0x46]),  // "C2SF" - cacheToSurface
    
    // Video
    H264: new Uint8Array([0x48, 0x32, 0x36, 0x34]),  // "H264" - H.264 NAL
    
    // Backchannel (browser → server)
    FACK: new Uint8Array([0x46, 0x41, 0x43, 0x4B]),  // "FACK" - frameAck
    BPRS: new Uint8Array([0x42, 0x50, 0x52, 0x53]),  // "BPRS" - backpressure
    
    // existing format compatibility
    DELT: new Uint8Array([0x44, 0x45, 0x4C, 0x54]),  // "DELT" - delta frame
    OPUS: new Uint8Array([0x4F, 0x50, 0x55, 0x53]),  // "OPUS" - Opus audio
    AUDI: new Uint8Array([0x41, 0x55, 0x44, 0x49]),  // "AUDI" - raw audio
};

// ============================================================================
// Message type detection
// ============================================================================

/**
 * Check if buffer starts with given magic bytes
 */
export function matchMagic(data, magic) {
    if (data.length < 4) return false;
    return data[0] === magic[0] && data[1] === magic[1] && 
           data[2] === magic[2] && data[3] === magic[3];
}

/**
 * Get message type from magic header
 */
export function getMessageType(data) {
    if (data.length < 4) return null;
    
    for (const [name, magic] of Object.entries(Magic)) {
        if (matchMagic(data, magic)) {
            return name;
        }
    }
    return null;
}

// ============================================================================
// Binary reading utilities
// ============================================================================

export function readU16LE(data, offset) {
    return data[offset] | (data[offset + 1] << 8);
}

export function readU32LE(data, offset) {
    return data[offset] | (data[offset + 1] << 8) | 
           (data[offset + 2] << 16) | (data[offset + 3] << 24);
}

export function readI16LE(data, offset) {
    const val = readU16LE(data, offset);
    return val > 0x7FFF ? val - 0x10000 : val;
}

// ============================================================================
// Binary writing utilities
// ============================================================================

export function writeU16LE(data, offset, value) {
    data[offset] = value & 0xFF;
    data[offset + 1] = (value >> 8) & 0xFF;
}

export function writeU32LE(data, offset, value) {
    data[offset] = value & 0xFF;
    data[offset + 1] = (value >> 8) & 0xFF;
    data[offset + 2] = (value >> 16) & 0xFF;
    data[offset + 3] = (value >> 24) & 0xFF;
}

// ============================================================================
// Message Parsers (server → browser)
// ============================================================================

/**
 * Parse createSurface message
 * Layout: SURF(4) + surfaceId(2) + width(2) + height(2) + format(2) = 12 bytes
 */
export function parseCreateSurface(data) {
    if (data.length < 12) return null;
    return {
        type: 'createSurface',
        surfaceId: readU16LE(data, 4),
        width: readU16LE(data, 6),
        height: readU16LE(data, 8),
        format: readU16LE(data, 10),
    };
}

/**
 * Parse deleteSurface message
 * Layout: DELS(4) + surfaceId(2) = 6 bytes
 */
export function parseDeleteSurface(data) {
    if (data.length < 6) return null;
    return {
        type: 'deleteSurface',
        surfaceId: readU16LE(data, 4),
    };
}

/**
 * Parse startFrame message
 * Layout: STFR(4) + frameId(4) = 8 bytes
 */
export function parseStartFrame(data) {
    if (data.length < 8) return null;
    return {
        type: 'startFrame',
        frameId: readU32LE(data, 4),
    };
}

/**
 * Parse endFrame message
 * Layout: ENFR(4) + frameId(4) = 8 bytes
 */
export function parseEndFrame(data) {
    if (data.length < 8) return null;
    return {
        type: 'endFrame',
        frameId: readU32LE(data, 4),
    };
}

/**
 * Parse progressive tile message
 * Layout: PROG(4) + frameId(4) + surfaceId(2) + x(2) + y(2) + w(2) + h(2) + dataSize(4) + data
 * Total header: 22 bytes
 */
export function parseProgressiveTile(data) {
    if (data.length < 22) return null;
    const dataSize = readU32LE(data, 18);
    if (data.length < 22 + dataSize) return null;
    
    return {
        type: 'tile',
        codec: 'progressive',
        frameId: readU32LE(data, 4),
        surfaceId: readU16LE(data, 8),
        x: readU16LE(data, 10),
        y: readU16LE(data, 12),
        w: readU16LE(data, 14),
        h: readU16LE(data, 16),
        payload: data.subarray(22, 22 + dataSize),
    };
}

/**
 * Parse WebP tile message
 * Layout: WEBP(4) + frameId(4) + surfaceId(2) + x(2) + y(2) + w(2) + h(2) + dataSize(4) + data
 * Total header: 22 bytes
 */
export function parseWebPTile(data) {
    if (data.length < 22) return null;
    const dataSize = readU32LE(data, 18);
    if (data.length < 22 + dataSize) return null;
    
    return {
        type: 'tile',
        codec: 'webp',
        frameId: readU32LE(data, 4),
        surfaceId: readU16LE(data, 8),
        x: readU16LE(data, 10),
        y: readU16LE(data, 12),
        w: readU16LE(data, 14),
        h: readU16LE(data, 16),
        payload: data.subarray(22, 22 + dataSize),
    };
}

/**
 * Parse raw RGBA tile message
 * Layout: TILE(4) + frameId(4) + surfaceId(2) + x(2) + y(2) + w(2) + h(2) + data
 * Header: 18 bytes, data = w * h * 4 bytes
 */
export function parseRawTile(data) {
    if (data.length < 18) return null;
    const w = readU16LE(data, 14);
    const h = readU16LE(data, 16);
    const expectedSize = 18 + w * h * 4;
    if (data.length < expectedSize) return null;
    
    return {
        type: 'tile',
        codec: 'raw',
        frameId: readU32LE(data, 4),
        surfaceId: readU16LE(data, 8),
        x: readU16LE(data, 10),
        y: readU16LE(data, 12),
        w: w,
        h: h,
        payload: data.subarray(18, expectedSize),
    };
}

/**
 * Parse solidFill message
 * Layout: SFIL(4) + frameId(4) + surfaceId(2) + x(2) + y(2) + w(2) + h(2) + color(4) = 22 bytes
 */
export function parseSolidFill(data) {
    if (data.length < 22) return null;
    return {
        type: 'solidFill',
        frameId: readU32LE(data, 4),
        surfaceId: readU16LE(data, 8),
        x: readU16LE(data, 10),
        y: readU16LE(data, 12),
        w: readU16LE(data, 14),
        h: readU16LE(data, 16),
        color: readU32LE(data, 18),  // BGRA32
    };
}

/**
 * Parse surfaceToSurface message
 * Layout: S2SF(4) + frameId(4) + srcSurfaceId(2) + dstSurfaceId(2) + 
 *         srcX(2) + srcY(2) + srcW(2) + srcH(2) + dstX(2) + dstY(2) = 24 bytes
 */
export function parseSurfaceToSurface(data) {
    if (data.length < 24) return null;
    return {
        type: 'surfaceToSurface',
        frameId: readU32LE(data, 4),
        srcSurfaceId: readU16LE(data, 8),
        dstSurfaceId: readU16LE(data, 10),
        srcX: readU16LE(data, 12),
        srcY: readU16LE(data, 14),
        srcW: readU16LE(data, 16),
        srcH: readU16LE(data, 18),
        dstX: readU16LE(data, 20),
        dstY: readU16LE(data, 22),
    };
}

/**
 * Parse cacheToSurface message
 * Layout: C2SF(4) + frameId(4) + surfaceId(2) + cacheId(2) + dstX(2) + dstY(2) = 16 bytes
 */
export function parseCacheToSurface(data) {
    if (data.length < 16) return null;
    return {
        type: 'cacheToSurface',
        frameId: readU32LE(data, 4),
        surfaceId: readU16LE(data, 8),
        cacheId: readU16LE(data, 10),
        dstX: readU16LE(data, 12),
        dstY: readU16LE(data, 14),
    };
}

/**
 * Parse H.264 video frame (legacy format from existing implementation)
 * Layout: H264(4) + frameId(4) + surfaceId(2) + codecId(2) + frameType(1) + 
 *         destX(2) + destY(2) + destW(2) + destH(2) + nalSize(4) + chromaNalSize(4) + data
 */
export function parseH264Frame(data) {
    if (data.length < 29) return null;
    const nalSize = readU32LE(data, 21);
    const chromaNalSize = readU32LE(data, 25);
    if (data.length < 29 + nalSize + chromaNalSize) return null;
    
    return {
        type: 'videoFrame',
        frameId: readU32LE(data, 4),
        surfaceId: readU16LE(data, 8),
        codecId: readU16LE(data, 10),
        frameType: data[12],
        destX: readI16LE(data, 13),
        destY: readI16LE(data, 15),
        destW: readU16LE(data, 17),
        destH: readU16LE(data, 19),
        nalSize: nalSize,
        chromaNalSize: chromaNalSize,
        nalData: data.subarray(29, 29 + nalSize),
        chromaNalData: chromaNalSize > 0 ? data.subarray(29 + nalSize, 29 + nalSize + chromaNalSize) : null,
    };
}

// ============================================================================
// Message Builders (browser → server)
// ============================================================================

/**
 * Build frameAck message
 * Layout: FACK(4) + frameId(4) = 8 bytes
 */
export function buildFrameAck(frameId) {
    const data = new Uint8Array(8);
    data.set(Magic.FACK, 0);
    writeU32LE(data, 4, frameId);
    return data;
}

/**
 * Build backpressure message
 * Layout: BPRS(4) + level(1) = 5 bytes
 */
export function buildBackpressure(level) {
    const data = new Uint8Array(5);
    data.set(Magic.BPRS, 0);
    data[4] = level;
    return data;
}

// ============================================================================
// Legacy delta frame parser
// ============================================================================

/**
 * Parse legacy DELT frame
 * Layout: DELT(4) + jsonLength(4) + json + webp tiles
 * @returns {Object} { type: 'deltaFrame', rects: [{x, y, size, data}...] }
 */
export function parseDeltaFrame(data) {
    if (data.length < 8) return null;
    
    const jsonLength = readU32LE(data, 4);
    if (jsonLength <= 0 || jsonLength > data.length - 8) return null;
    
    try {
        const jsonStr = new TextDecoder().decode(data.subarray(8, 8 + jsonLength));
        const metadata = JSON.parse(jsonStr);
        
        if (!metadata.rects) return null;
        
        const tiles = [];
        let offset = 8 + jsonLength;
        
        for (const rect of metadata.rects) {
            if (offset + rect.size > data.length) break;
            
            tiles.push({
                x: rect.x,
                y: rect.y,
                w: rect.w || rect.width,
                h: rect.h || rect.height,
                data: data.subarray(offset, offset + rect.size),
            });
            offset += rect.size;
        }
        
        return { type: 'deltaFrame', tiles };
    } catch (e) {
        return null;
    }
}

// ============================================================================
// Unified message parser
// ============================================================================

/**
 * Parse any incoming message based on magic header
 * @param {Uint8Array} data - Raw message bytes
 * @returns {Object|null} Parsed message or null if unknown
 */
export function parseMessage(data) {
    const type = getMessageType(data);
    
    switch (type) {
        case 'SURF': return parseCreateSurface(data);
        case 'DELS': return parseDeleteSurface(data);
        case 'STFR': return parseStartFrame(data);
        case 'ENFR': return parseEndFrame(data);
        case 'PROG': return parseProgressiveTile(data);
        case 'WEBP': return parseWebPTile(data);
        case 'TILE': return parseRawTile(data);
        case 'SFIL': return parseSolidFill(data);
        case 'S2SF': return parseSurfaceToSurface(data);
        case 'C2SF': return parseCacheToSurface(data);
        case 'H264': return parseH264Frame(data);
        case 'DELT': return parseDeltaFrame(data);
        default: return null;
    }
}
