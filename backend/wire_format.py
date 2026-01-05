"""
Wire Format Definitions for GFX Event Stream

All messages use 4-byte magic headers followed by binary payload.
Little-endian byte order throughout.

This module mirrors frontend/wire-format.js for consistent binary protocol.
"""

import struct
from typing import Optional

# ============================================================================
# Magic codes (4 bytes each)
# ============================================================================

class Magic:
    """4-byte magic headers for each message type"""
    
    # Surface management
    SURF = b'SURF'  # createSurface
    DELS = b'DELS'  # deleteSurface
    
    # Frame lifecycle
    STFR = b'STFR'  # startFrame
    ENFR = b'ENFR'  # endFrame
    
    # Tile codecs
    PROG = b'PROG'  # progressive tile
    WEBP = b'WEBP'  # WebP tile
    TILE = b'TILE'  # raw RGBA tile
    
    # Surface operations
    SFIL = b'SFIL'  # solidFill
    S2SF = b'S2SF'  # surfaceToSurface
    C2SF = b'C2SF'  # cacheToSurface
    
    # Video
    H264 = b'H264'  # H.264 NAL
    
    # Backchannel (browser → server)
    FACK = b'FACK'  # frameAck
    BPRS = b'BPRS'  # backpressure
    
    # Legacy format compatibility
    DELT = b'DELT'  # delta frame (existing format)
    OPUS = b'OPUS'  # Opus audio
    AUDI = b'AUDI'  # raw audio


# ============================================================================
# Message Builders (server → browser)
# ============================================================================

def build_create_surface(surface_id: int, width: int, height: int, 
                         pixel_format: int = 0x20) -> bytes:
    """
    Build createSurface message.
    
    Layout: SURF(4) + surfaceId(2) + width(2) + height(2) + format(2) = 12 bytes
    
    Args:
        surface_id: Surface identifier (0-65535)
        width: Surface width in pixels
        height: Surface height in pixels
        pixel_format: Pixel format (0x20 = BGRA32, 0x21 = RGBA32)
    
    Returns:
        Binary message ready to send via WebSocket
    """
    return struct.pack('<4sHHHH', 
                       Magic.SURF, 
                       surface_id, 
                       width, 
                       height, 
                       pixel_format)


def build_delete_surface(surface_id: int) -> bytes:
    """
    Build deleteSurface message.
    
    Layout: DELS(4) + surfaceId(2) = 6 bytes
    
    Args:
        surface_id: Surface identifier to delete
    
    Returns:
        Binary message ready to send via WebSocket
    """
    return struct.pack('<4sH', Magic.DELS, surface_id)


def build_start_frame(frame_id: int) -> bytes:
    """
    Build startFrame message.
    
    Layout: STFR(4) + frameId(4) = 8 bytes
    
    Args:
        frame_id: Frame sequence number
    
    Returns:
        Binary message ready to send via WebSocket
    """
    return struct.pack('<4sI', Magic.STFR, frame_id)


def build_end_frame(frame_id: int) -> bytes:
    """
    Build endFrame message.
    
    Layout: ENFR(4) + frameId(4) = 8 bytes
    
    Args:
        frame_id: Frame sequence number (must match startFrame)
    
    Returns:
        Binary message ready to send via WebSocket
    """
    return struct.pack('<4sI', Magic.ENFR, frame_id)


def build_progressive_tile(frame_id: int, surface_id: int,
                           x: int, y: int, w: int, h: int,
                           data: bytes) -> bytes:
    """
    Build progressive tile message.
    
    Layout: PROG(4) + frameId(4) + surfaceId(2) + x(2) + y(2) + w(2) + h(2) + 
            dataSize(4) + data
    Total header: 22 bytes
    
    Args:
        frame_id: Frame sequence number
        surface_id: Target surface
        x, y: Destination coordinates
        w, h: Tile dimensions
        data: Progressive codec compressed data
    
    Returns:
        Binary message ready to send via WebSocket
    """
    header = struct.pack('<4sIHHHHHI',
                         Magic.PROG,
                         frame_id,
                         surface_id,
                         x, y, w, h,
                         len(data))
    return header + data


def build_webp_tile(frame_id: int, surface_id: int,
                    x: int, y: int, w: int, h: int,
                    data: bytes) -> bytes:
    """
    Build WebP tile message.
    
    Layout: WEBP(4) + frameId(4) + surfaceId(2) + x(2) + y(2) + w(2) + h(2) + 
            dataSize(4) + data
    Total header: 22 bytes
    
    Args:
        frame_id: Frame sequence number
        surface_id: Target surface
        x, y: Destination coordinates
        w, h: Tile dimensions
        data: WebP compressed image data
    
    Returns:
        Binary message ready to send via WebSocket
    """
    header = struct.pack('<4sIHHHHHI',
                         Magic.WEBP,
                         frame_id,
                         surface_id,
                         x, y, w, h,
                         len(data))
    return header + data


def build_raw_tile(frame_id: int, surface_id: int,
                   x: int, y: int, w: int, h: int,
                   data: bytes) -> bytes:
    """
    Build raw RGBA tile message.
    
    Layout: TILE(4) + frameId(4) + surfaceId(2) + x(2) + y(2) + w(2) + h(2) + 
            dataSize(4) + data
    Total header: 22 bytes
    
    Args:
        frame_id: Frame sequence number
        surface_id: Target surface
        x, y: Destination coordinates
        w, h: Tile dimensions
        data: Raw BGRA/RGBA pixel data (w * h * 4 bytes)
    
    Returns:
        Binary message ready to send via WebSocket
    """
    header = struct.pack('<4sIHHHHHI',
                         Magic.TILE,
                         frame_id,
                         surface_id,
                         x, y, w, h,
                         len(data))
    return header + data


def build_solid_fill(frame_id: int, surface_id: int,
                     color: int, rects: list) -> bytes:
    """
    Build solidFill message.
    
    Layout: SFIL(4) + frameId(4) + surfaceId(2) + color(4) + rectCount(2) +
            [x(2) + y(2) + w(2) + h(2)] * rectCount
    Base header: 16 bytes + 8 bytes per rect
    
    Args:
        frame_id: Frame sequence number
        surface_id: Target surface
        color: Fill color (ARGB32)
        rects: List of (x, y, w, h) tuples
    
    Returns:
        Binary message ready to send via WebSocket
    """
    header = struct.pack('<4sIHIH',
                         Magic.SFIL,
                         frame_id,
                         surface_id,
                         color,
                         len(rects))
    
    rect_data = b''.join(
        struct.pack('<HHHH', r[0], r[1], r[2], r[3]) 
        for r in rects
    )
    
    return header + rect_data


def build_surface_to_surface(frame_id: int, 
                              src_surface_id: int, dst_surface_id: int,
                              src_x: int, src_y: int,
                              dst_x: int, dst_y: int,
                              w: int, h: int) -> bytes:
    """
    Build surfaceToSurface message.
    
    Layout: S2SF(4) + frameId(4) + srcSurfaceId(2) + dstSurfaceId(2) +
            srcX(2) + srcY(2) + dstX(2) + dstY(2) + w(2) + h(2) = 24 bytes
    
    Args:
        frame_id: Frame sequence number
        src_surface_id: Source surface
        dst_surface_id: Destination surface
        src_x, src_y: Source coordinates
        dst_x, dst_y: Destination coordinates
        w, h: Copy region dimensions
    
    Returns:
        Binary message ready to send via WebSocket
    """
    return struct.pack('<4sIHHHHHHHH',
                       Magic.S2SF,
                       frame_id,
                       src_surface_id,
                       dst_surface_id,
                       src_x, src_y,
                       dst_x, dst_y,
                       w, h)


def build_cache_to_surface(frame_id: int, surface_id: int, cache_slot: int,
                            dst_x: int, dst_y: int,
                            w: int, h: int) -> bytes:
    """
    Build cacheToSurface message.
    
    Layout: C2SF(4) + frameId(4) + surfaceId(2) + cacheSlot(2) +
            dstX(2) + dstY(2) + w(2) + h(2) = 20 bytes
    
    Args:
        frame_id: Frame sequence number
        surface_id: Target surface
        cache_slot: Source cache slot index
        dst_x, dst_y: Destination coordinates
        w, h: Region dimensions
    
    Returns:
        Binary message ready to send via WebSocket
    """
    return struct.pack('<4sIHHHHHH',
                       Magic.C2SF,
                       frame_id,
                       surface_id,
                       cache_slot,
                       dst_x, dst_y,
                       w, h)


def build_h264_frame(frame_id: int, surface_id: int, codec_id: int,
                     frame_type: int, x: int, y: int, w: int, h: int,
                     nal_data: bytes, chroma_data: bytes = b'') -> bytes:
    """
    Build H.264 frame message.
    
    Layout: H264(4) + frameId(4) + surfaceId(2) + codecId(2) + 
            frameType(1) + x(2) + y(2) + w(2) + h(2) +
            nalSize(4) + chromaNalSize(4) + nalData + chromaNalData
    Header: 25 bytes
    
    Args:
        frame_id: Frame sequence number
        surface_id: Target surface
        codec_id: H.264 codec variant (AVC420=0x08020, AVC444=0x08400)
        frame_type: I-frame=0, P-frame=1
        x, y: Destination coordinates
        w, h: Frame dimensions
        nal_data: H.264 NAL unit data
        chroma_data: Optional AVC444 chroma NAL data
    
    Returns:
        Binary message ready to send via WebSocket
    """
    header = struct.pack('<4sIHHBhhHHII',
                         Magic.H264,
                         frame_id,
                         surface_id,
                         codec_id,
                         frame_type,
                         x, y, w, h,
                         len(nal_data),
                         len(chroma_data))
    return header + nal_data + chroma_data


# ============================================================================
# Message Parsers (browser → server)
# ============================================================================

def parse_frame_ack(data: bytes) -> Optional[dict]:
    """
    Parse frameAck message from browser.
    
    Layout: FACK(4) + frameId(4) = 8 bytes
    
    Args:
        data: Binary message from WebSocket
    
    Returns:
        Parsed message dict or None if invalid
    """
    if len(data) < 8:
        return None
    if data[:4] != Magic.FACK:
        return None
    
    frame_id = struct.unpack('<I', data[4:8])[0]
    return {
        'type': 'frameAck',
        'frame_id': frame_id
    }


def parse_backpressure(data: bytes) -> Optional[dict]:
    """
    Parse backpressure message from browser.
    
    Layout: BPRS(4) + queueDepth(4) = 8 bytes
    
    Args:
        data: Binary message from WebSocket
    
    Returns:
        Parsed message dict or None if invalid
    """
    if len(data) < 8:
        return None
    if data[:4] != Magic.BPRS:
        return None
    
    queue_depth = struct.unpack('<I', data[4:8])[0]
    return {
        'type': 'backpressure',
        'queue_depth': queue_depth
    }


def get_message_type(data: bytes) -> Optional[str]:
    """
    Get message type from magic header.
    
    Args:
        data: Binary message
    
    Returns:
        Message type string or None if unrecognized
    """
    if len(data) < 4:
        return None
    
    magic = data[:4]
    
    # Map magic to type name
    type_map = {
        Magic.SURF: 'createSurface',
        Magic.DELS: 'deleteSurface',
        Magic.STFR: 'startFrame',
        Magic.ENFR: 'endFrame',
        Magic.PROG: 'progressiveTile',
        Magic.WEBP: 'webpTile',
        Magic.TILE: 'rawTile',
        Magic.SFIL: 'solidFill',
        Magic.S2SF: 'surfaceToSurface',
        Magic.C2SF: 'cacheToSurface',
        Magic.H264: 'h264Frame',
        Magic.FACK: 'frameAck',
        Magic.BPRS: 'backpressure',
        Magic.DELT: 'deltaFrame',
        Magic.OPUS: 'opusAudio',
        Magic.AUDI: 'rawAudio',
    }
    
    return type_map.get(magic)
