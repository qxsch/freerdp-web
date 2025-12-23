# FreeRDP GFX Bridge - Debugging Notes

## Overview
Native FreeRDP → WebSocket bridge using RDP GFX (RDPEGFX) with H.264/AVC444 for low-latency remote desktop streaming.

---

## Key Architecture Decisions

### 1. Pure GFX Mode (No GDI Chaining)
- All graphics arrive via RDPEGFX callbacks, not legacy GDI
- No `gdi_graphics_pipeline_init()` - we register our own callbacks directly
- Primary buffer updated only when surfaces are mapped

### 2. Per-Surface Buffers
- Each GFX surface has its own BGRA32 buffer (`surface_buffers[]`)
- Required for proper `SurfaceToCache` → `CacheToSurface` workflow
- Codec output goes to surface buffer first, then to primary if mapped

### 3. Session-Level Caches
- **GFX Bitmap Cache**: 4096 slots, persists across surface resets
- **ClearCodec Decoder**: Single instance per session, internal caches (VBar, ShortVBar, Glyph) must NOT be reset

---

## Issues & Solutions

### Issue 1: Missing Tiles During Animations
**Symptom**: Tiles not painted, visual corruption during scrolling/animations  
**Cause**: `SolidFill`, `SurfaceToSurface`, `SurfaceToCache`, `CacheToSurface` were stub implementations  
**Fix**: Implemented actual pixel operations reading/writing surface buffers

### Issue 2: RDPDR Error 1359
**Symptom**: `[ERROR] RDPDR Failed to create device with error: 1359`  
**Cause**: Device redirection channels enabled but not handled  
**Fix**: Disabled all device redirection settings:
```c
freerdp_settings_set_bool(settings, FreeRDP_DeviceRedirection, FALSE);
freerdp_settings_set_bool(settings, FreeRDP_RedirectDrives, FALSE);
// etc.
```

### Issue 3: ClearCodec Decode Failures
**Symptom**: Many `ClearCodec decode failed` errors, tile corruption  
**Root Cause**: Wrong parameters passed to `clear_decompress()`:
```c
// WRONG - passing update region size
clear_decompress(..., nWidth, nHeight, NULL);

// CORRECT - passing full destination buffer dimensions
clear_decompress(..., surfWidth, surfHeight, NULL);
```
**Explanation**: The `nDstWidth`/`nDstHeight` parameters are for bounds checking against the destination buffer, not the update region size.

### Issue 4: ClearCodec Cache Corruption After Reset
**Symptom**: Decode failures after GFX Reset or surface deletion  
**Cause**: Incorrectly destroying/recreating ClearCodec decoder  
**Fix**: ClearCodec decoder is **session-level** - never reset it. Its internal caches are referenced by subsequent frames.

---

## GFX Callback Flow

```
Server sends GFX command
    ↓
gfx_on_wire_to_surface (codec decode)
    ↓
Decode to surface_buffer[surfaceId]
    ↓
If surface mapped to output → copy to primary_buffer
    ↓
Send dirty region via WebSocket
```

## Codec Handling Pattern

```c
// Get surface dimensions for destination buffer
UINT32 surfWidth = context->surfaces[cmd->surfaceId].width;
UINT32 surfHeight = context->surfaces[cmd->surfaceId].height;
BYTE* surfBuffer = context->surface_buffers[cmd->surfaceId];

// Decode to surface buffer (pass FULL buffer dimensions)
codec_decompress(..., surfBuffer, PIXEL_FORMAT_BGRA32,
                 surfWidth * 4,        // stride
                 surfX, surfY,         // position in buffer
                 surfWidth, surfHeight // BUFFER dimensions, not region
                 );

// Copy to primary if mapped
if (surface mapped) {
    copy_region_to_primary(...
```

---

## Current Status
- ✅ GFX negotiation (v10.7 with H.264)
- ✅ All codecs: H.264, ClearCodec, Planar, Uncompressed, Progressive
- ✅ Cache operations: SurfaceToCache, CacheToSurface
- ✅ Surface operations: SolidFill, SurfaceToSurface
- ✅ RDPDR disabled
- ✅ ClearCodec parameter fix applied
- 🔄 Black tiles issue - investigation ongoing

---

## Remaining: Black Tiles

Possible causes to investigate:
1. **Cache miss**: Cache slot empty when `CacheToSurface` called
2. **Surface buffer not allocated**: Check `gfx_on_create_surface`
3. **Zero-initialized regions**: SolidFill with black or uninitialized memory
4. **Codec failure path**: Decode fails silently, leaves black pixels
