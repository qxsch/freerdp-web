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

### Issue 10: Full WebP Frames Flooding During GFX
**Symptom**: Many "Sending full frame" messages, WebP competing with H.264  
**Root Cause**: `gfx_on_end_frame()` was unconditionally setting `needs_full_frame = true`  
**Fix**: Removed that line. EndFrame now only logs, doesn't trigger full frames.

### Issue 11: Scrolling Leftovers / Missing Updates
**Symptom**: Visual artifacts during scrolling, content not updating properly  
**Root Cause**: Python code was skipping ALL WebP frames (including delta rects) once H.264 started. But `SolidFill`, `SurfaceToSurface`, `CacheToSurface` operations (used for scrolling, copy, fill) add dirty rects that are NOT H.264 encoded — they need to be sent as WebP delta frames!  
**Fix**: Modified Python streaming loop to:
1. Send H.264 frames (priority)
2. **Also send dirty rect delta frames** for non-H.264 operations
3. Only skip full WebP frames (entire screen)

Key insight: During scrolling:
- `SurfaceToSurface` copies existing content (registers dirty rect)
- `SolidFill` or `CacheToSurface` fills new areas (registers dirty rect)
- These are NOT H.264 — they're direct framebuffer operations
- Must send as WebP delta to frontend

### Issue 12: Visual Leftovers After Window De-Maximize (Within RDP Session)
**Symptom**: When maximizing then de-maximizing a window *inside the RDP session* (e.g., Explorer, Notepad), stale pixels remain in areas previously covered by the maximized window. The RDP desktop size stays constant — this is about the server's GFX repaint strategy.

**Root Cause**: With `GfxSmallCache = FALSE` (default), the RDP server aggressively uses `SurfaceToCache`/`CacheToSurface` for desktop repaints. When de-maximizing:
1. Server expects client to have cached desktop background tiles
2. Server sends `CacheToSurface` commands referencing previously cached tiles
3. If our cache is incomplete, stale, or the server's caching assumptions don't match our bridge architecture, repaints fail silently
4. Result: stale content from the maximized window remains visible

**Understanding GFX Cache Settings**:

| Flag | Meaning | Server Behavior | Recommendation |
|------|---------|-----------------|----------------|
| **GfxSmallCache** | "Use smaller tile cache" | Fewer `SurfaceToCache`/`CacheToSurface` ops, more direct `WireToSurface` updates | ✅ Enable for bridge architectures |
| **GfxThinClient** | "I am a low-power client" | Less AVC444/H.264, more ClearCodec, conservative encoding | ❌ Keep FALSE for quality |

**Fix Applied**: 
```c
freerdp_settings_set_bool(settings, FreeRDP_GfxSmallCache, TRUE);
freerdp_settings_set_bool(settings, FreeRDP_GfxThinClient, FALSE);
```

This tells the server:
- "Don't rely heavily on client-side tile cache" → more explicit redraws
- "But still give me full AVC444/H.264 quality" → no quality compromise

**Why This Works for Bridge Architectures**:
- WebSocket bridges forward frames to browsers — long-term tile caching adds complexity
- Browser clients decode and render independently; they don't share our C-side cache
- More direct `WireToSurface` updates = more predictable repaint behavior
- Slight bandwidth increase is negligible for local/LAN connections

**Additional Fixes Applied**:

1. **Clear primary_buffer on GFX Reset**: Before setting `needs_full_frame`, we now `memset()` the buffer to black. This prevents sending stale content from the old surface layout when a reset occurs.

2. **Increased RDP_MAX_DIRTY_RECTS from 64 to 256**: Complex repaint scenarios (like de-maximize) can generate hundreds of dirty rects. When the limit is hit, rects are dropped and regions aren't updated. Now we also set `needs_full_frame = true` as fallback when overflow occurs.

3. **Dirty rect overflow detection**: Added logging and fallback to full frame when dirty rect limit is exceeded.

**Debug Flags Added** (in rdp_bridge.c):
```c
#define DEBUG_GFX_CACHE 0  /* Log all SurfaceToCache/CacheToSurface operations */
#define DEBUG_GFX_FILL 0   /* Log all SolidFill operations */
#define DEBUG_GFX_COPY 0   /* Log all SurfaceToSurface operations */
```
Set to `1` and rebuild to trace cache operations if leftovers persist.

### Issue 12b: Stale Full Frame Sent After GFX Reset
**Symptom**: After GFX Reset (e.g., de-maximizing window), stale content from the old surface layout appears briefly, then gets partially overwritten by new content — leaving "leftovers" in regions not immediately repainted.

**Log Pattern**:
```
[rdp_bridge] GFX Reset: 2516x1162
...
Sending full frame: 2516x1162          ← STALE content sent here!
[rdp_bridge] GFX DeleteSurface: id=0
[rdp_bridge] GFX CreateSurface: id=0 2516x1162
[rdp_bridge] GFX MapSurfaceScaled: ...
EndFrame: frame_id=9 cmds=760          ← NEW content rendered here
```

**Root Cause**: The sequence is:
1. `gfx_on_reset_graphics()` sets `needs_full_frame = true`
2. Python layer sees `needs_full_frame` and sends `primary_buffer` as full WebP
3. But `primary_buffer` still contains OLD content from before the reset!
4. Only AFTER the full frame is sent does the server send new surface content
5. New content partially overwrites the stale full frame, but not all regions

**Fix**: Clear `primary_buffer` to black in `gfx_on_reset_graphics()` BEFORE setting `needs_full_frame`:
```c
/* In gfx_on_reset_graphics() */
rdpGdi* gdi = bctx->gdi;
if (gdi && gdi->primary_buffer) {
    memset(gdi->primary_buffer, 0, gdi->stride * gdi->height);
}
bctx->needs_full_frame = true;
```

### Issue 12c: Dirty Rect Overflow Causing Missed Updates
**Symptom**: During complex repaint scenarios (760 commands in one frame), only 64 dirty rects are tracked — the rest are silently dropped, leaving regions un-updated.

**Log Pattern**:
```
EndFrame: frame_id=9 cmds=760 h264=0 dirty_rects=64  ← 760 cmds but only 64 rects!
```

**Root Cause**: `RDP_MAX_DIRTY_RECTS` was set to 64. When exceeded, additional dirty rects were simply not recorded, causing those regions to never be sent to the frontend.

**Fix**:
1. Increased `RDP_MAX_DIRTY_RECTS` from 64 to 512 in `rdp_bridge.h`
2. Added overflow detection with fallback to `needs_full_frame = true` in ALL places that register dirty rects:
   - `SolidFill`
   - `SurfaceToSurface`
   - `CacheToSurface`
   - ClearCodec decoder
   - Uncompressed decoder
   - Progressive decoder
   - Planar decoder
```c
if (bctx->dirty_rect_count < RDP_MAX_DIRTY_RECTS) {
    /* Record dirty rect */
} else {
    /* Overflow - fall back to full frame */
    bctx->needs_full_frame = true;
}
```
### Issue 12d: Race Condition - Python Sending Mid-Frame Partial Buffers
**Symptom**: Despite all previous fixes, leftovers still occur. Logs show multiple "Sending full frame" events interleaved with GFX commands, and dirty rect counts fluctuate unexpectedly.

**Initial Theory**: Python's async frame streaming loop polls the C layer independently of GFX frame boundaries, potentially sending incomplete buffer state.

**Attempted Fix**: Added `gfx_frame_in_progress` flag set in StartFrame, cleared in EndFrame. Modified `rdp_get_dirty_rects()` and `rdp_needs_full_frame()` to return 0/false while flag is set.

**PROBLEM**: This caused a deadlock-like stall! When frames arrive faster than Python can consume them (continuous animation, scrolling), `gfx_frame_in_progress` is almost always true when Python checks. Python keeps getting 0 dirty rects and never sends any updates.

**Reverted Fix**: Removed the blocking behavior. The flag is still tracked (for debugging), but API functions no longer block on it. The mutex already ensures atomic access to dirty rects.

**Key Insight**: Reading dirty rects mid-frame is actually safe because:
1. Mutex protects all accesses
2. Dirty rects are only appended, never modified in place
3. Even if we read a partial frame's rects, we'll get more on the next read
4. The real consistency issue (if any) is in the primary_buffer, not the rect list

**Status**: This was NOT the root cause. See Issue 12e below.

### Issue 12e: Python GFX Path Never Checked needs_full_frame + Buffer Mismatch
**Symptom**: Logs show `dirty_rects=512` (overflow) repeatedly, but leftovers still appear. Full frames are sent but areas remain stale.

**Log Pattern**:
```
[rdp_bridge] EndFrame: frame_id=2 cmds=760 h264=0 dirty_rects=512
Sending full frame: 2516x1162, stride=10064
[rdp_bridge] EndFrame: frame_id=3 cmds=760 h264=0 dirty_rects=512  ← Still 512?!
```

**Root Cause #1 - GFX path skipped full frame check**:
The Python GFX code path (`if gfx_active:`) processed dirty rects and called `continue`, completely bypassing the `needs_full_frame` check which was only in the legacy GDI code path.

When C code set `needs_full_frame = true` due to overflow:
1. Python's GFX path called `rdp_get_dirty_rects()` → got 64 rects
2. Sent 64-rect delta frame
3. Called `rdp_clear_dirty_rects()` → cleared all 512 rects
4. Called `continue` → **never checked `needs_full_frame`!**
5. Flag remained true but was never acted upon

**Root Cause #2 - Python/C buffer size mismatch**:
- C code: `RDP_MAX_DIRTY_RECTS = 512`
- Python code: `max_rects = 64` (allocated buffer)

Even when C had 200 dirty rects, Python could only receive 64 per call!

**Fix**:
1. Added `needs_full_frame` check at START of GFX dirty rect processing:
```python
if result != 0 or h264_ever_received:
    needs_full = self._lib.rdp_needs_full_frame(self._session)
    if needs_full:
        # Send full frame instead of deltas
        await self._send_full_frame(...)
        self._lib.rdp_clear_dirty_rects(self._session)
        continue
    # ... rest of delta rect processing
```

2. Increased Python's dirty rect buffer to match C:
```python
max_rects = 512  # Must match RDP_MAX_DIRTY_RECTS in C
rects = (RdpRect * max_rects)()
```

**Key Lesson**: When Python and C code both have limits/buffers, they MUST be synchronized!

### Issue 12f: Black Frame Flash After GFX Reset
**Symptom**: After maximize/de-maximize or resolution change, a black frame is briefly visible before real content appears. Leftovers still occur.

**Log Pattern**:
```
[rdp_bridge] GFX Reset: 2516x1162
[rdp_bridge] GFX Reset: Cleared primary_buffer (2516x1162)
[rdp_bridge] StartFrame: frame_id=1 timestamp=0
[rdp_bridge] EndFrame: frame_id=1 cmds=0 h264=0 dirty_rects=0
Sending full frame (GFX overflow): 2516x1162   ← BLACK frame sent!
[rdp_bridge] GFX CreateSurface: id=0 2516x1162
[rdp_bridge] StartFrame: frame_id=2 timestamp=0
... 760 commands with real content ...
```

**Root Cause**: `gfx_on_reset_graphics()` was:
1. Clearing `primary_buffer` to black (correct - old content is invalid)
2. Setting `needs_full_frame = true` (WRONG - buffer is now black!)
3. Python sees the flag, sends BLACK full frame to client
4. Server then sends real content in next frame
5. Client sees black flash, and regions not fully repainted show leftovers

**Fix**: Removed `needs_full_frame = true` from `gfx_on_reset_graphics()`. Instead:
- The dirty rect overflow mechanism naturally triggers full frame when needed
- First real frame typically has 700+ commands → overflow → full frame with actual content
- Also clear dirty_rect_count in reset to avoid stale rects

```c
/* In gfx_on_reset_graphics() - DON'T set needs_full_frame! */
pthread_mutex_lock(&bctx->rect_mutex);
bctx->frame_width = reset->width;
bctx->frame_height = reset->height;
/* bctx->needs_full_frame = true; -- causes black frame flash */
bctx->dirty_rect_count = 0;  /* Clear stale dirty rects */
pthread_mutex_unlock(&bctx->rect_mutex);
```

### Issue 12g: Delta Frames Not Being Sent - Conditional Block Skipped
**Symptom**: After a full frame is sent (GFX overflow), subsequent frames show dirty_rects > 0 in C logs but no corresponding delta frame sends in Python. Leftovers persist.

**Observation Pattern**:
```
[rdp_bridge] EndFrame: frame_id=2 cmds=760 h264=0 dirty_rects=271  ← 271 rects!
[rdp_bridge] EndFrame: frame_id=4 cmds=1 h264=0 dirty_rects=1
[rdp_bridge] EndFrame: frame_id=5 cmds=15 h264=0 dirty_rects=15
... (no "Sending delta frame" logs - all skipped!)
[rdp_bridge] EndFrame: frame_id=8 cmds=760 h264=0 dirty_rects=512
Sending full frame (GFX overflow): 2516x1162  ← Only overflow triggers send
```

**Root Cause**: The dirty rect processing block was guarded by a condition:
```python
if result != 0 or h264_ever_received:
    # Process dirty rects...
```

This condition requires EITHER:
1. `result != 0` - rdp_poll() returned a new event
2. `h264_ever_received == True` - H.264 was used at some point

**Problem**: Dirty rects accumulate in C via GFX callbacks INDEPENDENTLY of what rdp_poll() returns. When rdp_poll() returns 0 (no new network events) but GFX callbacks have already processed a frame and added dirty rects, the entire dirty rect block was SKIPPED.

Additionally, after fixing the overflow path, a `continue` statement made the delta rect code UNREACHABLE (dead code).

**Fix**: 
1. Removed the `if result != 0 or h264_ever_received:` condition - always check dirty rects when GFX is active
2. Fixed code flow so delta rect handling is reachable (not after a `continue`)
3. Changed delta frame log from `debug` to `info` level for visibility

```python
# GFX is active - ALWAYS check for dirty rects
# (they accumulate in C callbacks independently of rdp_poll result)

needs_full = self._lib.rdp_needs_full_frame(self._session)
if needs_full:
    # Overflow - send full frame
    await self._send_full_frame(...)
    self._lib.rdp_clear_dirty_rects(self._session)
    continue

# No overflow - check for normal dirty rects  
rect_count = self._lib.rdp_get_dirty_rects(...)
if rect_count > 0:
    logger.info(f"Sending delta frame with {rect_count} dirty rects")
    await self._send_delta_frame(...)
    self._lib.rdp_clear_dirty_rects(self._session)
```

**Key Lesson**: GFX callbacks run synchronously during rdp_poll() but dirty rects persist after poll returns. The poll return value doesn't indicate whether dirty rects are pending.

### Issue 12h: Partial Delta Frames Sent Mid-Frame
**Symptom**: Delta frames with few dirty rects (e.g., 4) are sent, immediately followed by a full frame due to overflow. Leftovers persist.

**Observation Pattern**:
```
2025-12-24 11:54:31,104 - Sending delta frame with 4 dirty rects   ← Partial!
[rdp_bridge] EndFrame: frame_id=2 cmds=532 h264=0 dirty_rects=512  ← 512 total
2025-12-24 11:54:31,114 - Sending full frame (GFX overflow)        ← Then full
```

**Root Cause**: Python was reading dirty rects DURING a GFX frame (between StartFrame and EndFrame callbacks). The sequence:

1. C: `StartFrame` begins, `gfx_frame_in_progress = true`
2. C: First few commands decoded, 4 dirty rects registered
3. **Python**: Checks dirty rects, sees 4, sends delta frame ← TOO EARLY!
4. C: More commands decoded, 508 more dirty rects registered → overflow triggered
5. C: `EndFrame` completes, `needs_full_frame = true`, `dirty_rect_count = 512`
6. Python: Sees `needs_full_frame`, sends full frame (redundant)

The 4-rect delta contained incomplete data for a frame that would eventually have 512 rects.

**Fix**: Check `rdp_gfx_frame_in_progress()` before reading dirty rects. If a frame is being built, skip dirty rect processing this iteration and wait for the frame to complete:

```python
# ISSUE 12h: Check if a GFX frame is in progress (between StartFrame/EndFrame).
# If so, skip dirty rect processing this iteration.
if self._lib.rdp_gfx_frame_in_progress(self._session):
    # Frame still being built - wait for it to complete
    await asyncio.sleep(0.001)
    continue

# Now safe to read dirty rects - frame is complete
needs_full = self._lib.rdp_needs_full_frame(self._session)
```

**Key Insight**: Unlike Issue 12d (blocking approach that deadlocked), this is a non-blocking skip. If a frame is in progress, we just don't read dirty rects this iteration - the main loop continues and tries again on the next poll. This avoids both partial reads AND deadlocks.

**STATUS: REVERTED** - This fix caused freezes during startup. The frame-in-progress check AFTER poll return seemed to interfere with processing. Reverted to investigate further.

### Issue 12i: Frame-Based Dirty Rect Processing
**Symptom**: Same as 12h - delta frames sent mid-frame with partial dirty rects, then full frame on overflow.

**Root Cause Analysis**: The Issue 12h approach (checking `gfx_frame_in_progress`) caused freezes because it just checked the flag without guaranteeing frame completion. The fundamental problem is that `rdp_poll()` can return mid-frame due to its 16ms timeout.

**New Approach**: Track completed frame IDs instead of in-progress flag.

1. Added `last_completed_frame_id` field in C, updated in `gfx_on_end_frame()`
2. Added `rdp_gfx_get_last_completed_frame()` API function
3. Python tracks `last_processed_frame_id` - only reads dirty rects when a NEW frame completes

**Flow**:
```
Python                                  C (during rdp_poll)
------                                  -------------------
last_processed_frame_id = 0            
                                        StartFrame id=1
poll() returns mid-frame                
completed_frame_id = 0                  
0 == 0 → skip, sleep, continue          ...more decodes...
                                        EndFrame id=1 → last_completed_frame_id = 1
poll() returns                          
completed_frame_id = 1                  
1 != 0 → process dirty rects!           
last_processed_frame_id = 1
send delta frame
```

**Key Insight**: This approach doesn't block or cause deadlocks because we simply skip processing when no new frame has completed. We're not waiting for anything - just checking if there's new work to do.

**Fix Details**:
- `rdp_bridge.c`: Added `last_completed_frame_id` field, updated in EndFrame, added API function
- `rdp_bridge.h`: Added `rdp_gfx_get_last_completed_frame()` declaration
- `rdp_bridge.py`: Track `last_processed_frame_id`, only process dirty rects when completed_frame_id changes

**STATUS: REVERTED** - This fix also caused freezes, same as Issue 12h. Both approaches that wait for frame completion cause FreeRDP to freeze mid-frame. The root cause is unclear - possibly FreeRDP expects continuous polling without delays, or there's a timeout issue when we don't consume data fast enough.

The C code additions (last_completed_frame_id field and API) are harmless and left in place for potential future use. Only the Python check was reverted.

### Issue 12j: Accumulation-Based Delta Stabilization
**Symptom**: Same as 12h/12i - delta frames sent mid-frame with partial dirty rects.

**Approach**: Track rect count changes between polls. Only send when count is "stable" for 2+ polls.

**STATUS: INEFFECTIVE** - The "stable count" approach doesn't work because decode can pause briefly between batches, making count appear stable even mid-frame. Logs showed 86 rects sent as "stable" before frame completed with 512. The `rdp_peek_dirty_rect_count()` function added is kept for potential future use.

### Issue 12k: Frame-In-Progress Guard (Non-Blocking)
**Symptom**: Same as 12h/12i/12j - delta frames sent mid-frame with partial dirty rects.

**Root Cause Analysis**: Issue 12h checked `frame_in_progress` but then did `continue` which skipped the ENTIRE loop iteration including the poll. This starved FreeRDP of poll calls, causing freezes.

**New Approach**: Check `frame_in_progress` ONLY for the delta send decision, not for the poll loop. The key insight is:
- We MUST call `rdp_poll()` continuously to drive FreeRDP
- We can CHOOSE whether to send deltas based on frame state
- Rects accumulate safely until EndFrame, then we send them all

**Implementation**:
```python
frame_in_progress = self._lib.rdp_gfx_frame_in_progress(self._session)

if not frame_in_progress:
    # Frame complete - safe to send accumulated rects
    rect_count = self._lib.rdp_get_dirty_rects(...)
    if rect_count > 0:
        send_delta_frame(rect_count)
# else: frame building, rects accumulate, will send on next poll after EndFrame
```

**STATUS: WORKING** - Leftovers that appeared during login gradually cleared up as the session progressed and more frames were processed. The fix correctly synchronizes delta sends with frame completion.

### Issue 12l: Stale Dirty Rects After Surface Deletion
**Symptom**: Logs show frames with 0 commands but >0 dirty_rects.

**Root Cause**: When a surface is deleted (`DeleteSurface`), dirty rects pointing to that surface's old content persist.

**Fix**: Clear dirty rects when the primary surface is deleted:
```c
if (bctx->primary_surface_id == del->surfaceId) {
    bctx->primary_surface_id = 0;
    pthread_mutex_lock(&bctx->rect_mutex);
    bctx->dirty_rect_count = 0;  // Clear stale rects
    pthread_mutex_unlock(&bctx->rect_mutex);
}
```

**STATUS: WORKING** - Combined with Issue 12k, leftovers gradually clear up during the session.

### Issue 12m: Race Condition Between Read and Clear of Dirty Rects
**Symptom**: Delta sends without corresponding EndFrame logs; rects from new frames being cleared prematurely.

**Log Pattern**:
```
[rdp_bridge] EndFrame: frame_id=26 cmds=1 h264=0 dirty_rects=1
Sending delta frame with 1 dirty rects
Sending delta frame with 4 dirty rects   ← No EndFrame before this!
Sending delta frame with 3 dirty rects   ← Or this!
```

**Root Cause**: Race condition between Python's read and clear:
1. EndFrame N sets `frame_in_progress = false`, dirty_rect_count = 1
2. Python reads 1 rect, sends delta
3. StartFrame N+1 begins, adds 4 new rects
4. Python calls `clear_dirty_rects()` → clears ALL rects including N+1's!

Frame N+1's rects are now lost, causing incomplete updates.

**Fix**: Make `rdp_get_dirty_rects()` atomic - it reads AND clears in one mutex-protected operation:
```c
int rdp_get_dirty_rects(...) {
    pthread_mutex_lock(&ctx->rect_mutex);
    
    // Copy rects to caller's buffer
    int count = ctx->dirty_rect_count;
    for (int i = 0; i < count; i++) {
        rects[i] = ctx->dirty_rects[i];
    }
    
    // Atomically clear what we read (shift remaining down)
    ctx->dirty_rect_count = 0;  // or shift if partial read
    
    pthread_mutex_unlock(&ctx->rect_mutex);
    return count;
}
```

Python no longer calls `rdp_clear_dirty_rects()` after delta sends - the get operation handles it atomically.

**Additional Discovery**: With atomic read-clear, the Issue 12k `frame_in_progress` guard became HARMFUL. It was causing us to skip reading rects from completed frames just because a new frame had started. This caused 143+ rects to be lost. The guard was removed - atomic operations are sufficient to prevent races.

**STATUS: WORKING** - Atomic read-clear prevents races without blocking frame sends.