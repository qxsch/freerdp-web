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
- ✅ All codecs: ClearCodec, Planar, Uncompressed, Progressive
- ✅ Cache operations: SurfaceToCache, CacheToSurface
- ✅ Surface operations: SolidFill, SurfaceToSurface
- ✅ RDPDR disabled
- ✅ Per-surface buffer architecture
- ✅ Full surface buffer consistency (all ops write to both buffers)
- 🔄 H.264 (AVC420/AVC444) - NALs queued for browser WebCodecs decoding

---

## H.264 Streaming (Next Focus)

The H.264 path works differently:
1. Server sends AVC420/AVC444 frames via GFX SurfaceCommand
2. Bridge extracts raw NAL units from RDPGFX_AVC*_BITMAP_STREAM
3. NALs queued in ring buffer (`h264_frames[]`)
4. Python backend reads via `rdp_get_h264_frame()`
5. Sent to browser as binary WebSocket messages
6. Browser decodes with WebCodecs VideoDecoder
7. Rendered to canvas

### Potential H.264 Issues to Test:
- Frame ordering and timestamps
- IDR frame detection for decoder reset
- AVC444→AVC420 transcoding (if browser doesn't support 4:4:4)
- Latency optimization

---

## Remaining: Black Tiles

**Root Cause Found**: SolidFill and SurfaceToSurface were writing to `primary_buffer` only, not to surface buffers. This meant:
1. SolidFill fills primary but surface buffer stays uninitialized (black)
2. Later SurfaceToCache reads from surface buffer → caches black pixels
3. CacheToSurface draws cached black pixels

**Fix Applied**: 
- SolidFill now fills both surface buffer AND primary buffer
- SurfaceToSurface now copies within surface buffers AND to primary buffer

### Key Principle
All GFX operations work in **surface-local coordinates**. The workflow is:
1. Decode/fill/copy to **surface buffer** (no output offset)
2. If surface is mapped, copy to **primary buffer** (with output offset)
3. SurfaceToCache reads from **surface buffer**
4. CacheToSurface writes to **both surface buffer AND primary buffer**

### Issue 5: Stale Cache Tiles (Post-Progressive)
**Symptom**: Black/corrupted tiles appearing over time, correlated with `DeleteEncodingContext`  
**Root Cause**: `CacheToSurface` only wrote to `primary_buffer`, not surface buffer  
**Impact**: When server later calls `SurfaceToCache` on a region where `CacheToSurface` had drawn, it reads stale surface buffer content instead of the cached tile  
**Fix**: `CacheToSurface` now writes to both surface buffer AND primary buffer

---

## H.264 Streaming Implementation

### Architecture
```
Server → AVC420/AVC444 tile at (x,y,w,h)
    ↓
C: queue_h264_frame() → sets dest_rect from cmd->left/top/right/bottom
    ↓
Python: _send_h264_frame() → packs x,y,w,h into binary header
    ↓
JS: handleH264Frame() → parses destX,destY,destW,destH
    ↓
JS: decodeH264Frame() → pushes metadata to queue, VideoDecoder.decode()
    ↓
JS: output callback → shifts metadata, ctx.drawImage() at (destX,destY)
```

### Wire Format (Python → Browser)
```
[H264 magic (4)] [frame_id (4)] [surface_id (2)] [codec_id (2)]
[frame_type (1)] [x (2)] [y (2)] [w (2)] [h (2)]
[nal_size (4)] [chroma_nal_size (4)]
[nal_data...] [chroma_nal_data...]
```

### Issue 6: H.264 Tiles Drawn at Wrong Position
**Symptom**: All H.264 tiles rendered at (0,0) instead of correct screen position  
**Root Cause**: VideoDecoder `output` callback has no access to per-frame metadata  
**Fix**: 
1. Added `h264DecodeQueue[]` — FIFO queue tracking `{destX, destY, destW, destH}` per pending decode
2. Push metadata BEFORE `videoDecoder.decode()`
3. Shift metadata in `output` callback, use 9-arg `drawImage()` for tile compositing

### Issue 7: H.264 Decoder Stuck After Errors
**Symptom**: After network glitch or decode error, all subsequent frames fail  
**Root Cause**: VideoDecoder enters error state, P/B frames require prior reference frames  
**Fix**:
1. Track `h264DecoderError` flag, set in error callback
2. Skip P/B frames while error flag is set (wait for IDR)
3. On IDR frame + error flag: close decoder, reinitialize, clear queue
4. Clear error flag on successful decode

### Key Principle: Metadata Queue Sync
The WebCodecs `VideoDecoder` is asynchronous — `decode()` returns immediately but `output` callback fires later. The metadata queue MUST stay synchronized:
- Push to queue BEFORE `decode()`
- Shift from queue in `output` callback
- On error: clear the entire queue to prevent desync
- On decode failure exception: pop the just-pushed metadata

### AVC444 Transcoding
Browsers only support YUV 4:2:0, not 4:4:4. The C layer uses FFmpeg to transcode:
```c
transcode_avc444(luma_stream, chroma_stream) → single 4:2:0 stream
```
After transcoding, `codec_id` is changed to `AVC420` and chroma NAL is empty.

### Issue 8: Broken Pipe / Connection Reset with H.264
**Symptom**: `transport_default_write: BIO_should_retry returned a system error 32: Broken pipe`  
**Root Cause**: Duplicate frame acknowledgments — FreeRDP's GFX channel automatically sends `RDPGFX_FRAME_ACKNOWLEDGE_PDU` in its `EndFrame` handler. We were also sending acks from the browser via `rdp_ack_h264_frame()`, causing protocol errors.  
**Fix**: Disabled browser-side acks since FreeRDP handles them automatically.

To enable browser-controlled acks in the future (for better flow control):
1. Implement `OnOpen` callback: `gfx->OnOpen = gfx_on_open;`
2. Set `*do_frame_acks = FALSE` in the callback
3. Track which frames need acking
4. Send acks only after browser confirms decode

### Issue 9: H.264 Tiles Show Green with Small Video
**Symptom**: Video plays in tiny portion of rectangle, rest is bright green  
**Root Cause**: VideoDecoder configured with first tile's dimensions (e.g., 64x64), but subsequent tiles may be larger. Green = uninitialized YUV (Y=0, U=128, V=128).  
**Fix**:
1. Track `h264ConfiguredWidth/Height` 
2. Reconfigure decoder on IDR when dimensions change
3. Smart rendering based on decoded frame size vs destination rect:
   - If `displayWidth === destW`: tile mode, draw at position
   - If `displayWidth >= canvas.width`: full surface mode, draw fullscreen
   - Otherwise: scale to fit destination rect
