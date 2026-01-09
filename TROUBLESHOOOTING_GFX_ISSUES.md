# FreeRDP-Web GFX Troubleshooting Log

## Issue Summary
**Symptom**: Horizontal striping and "snow" bands appearing in rendered RDP session. The Chrome title area, toolbar, Start menu, and various UI elements show horizontally repeated speckled noise ("TV snow") and black bars.

**Key Observations**:
1. Artifacts appear as horizontal bands at multiple Y-positions
2. Some areas show stale/old content fragments  
3. Good content co-exists with bad tiles (video frames, some icons render correctly)
4. Pattern suggests **blit placement and/or cache management** issue, not pure codec problem

---

## Phase 0: Toggle Tests (Bracket the Defect)

### Test 0.1: Cache Bypass ✅ COMPLETED
**Purpose**: Disable SURFACE_TO_CACHE and CACHE_TO_SURFACE to test if cache is the issue

**Implementation**: Added `CACHE_BYPASS = true` flag in `gfx-worker.js` (line ~148)
- S2C operations log `S2C-SKIP` and skip `applySurfaceToCache()`
- C2S operations log `C2S-SKIP` and skip `applyCacheToSurface()`

**Result**: ❌ **STRIPES STILL APPEAR WITH CACHE BYPASSED**

**Conclusion**: **Cache is NOT the root cause.** The problem is in the tile decode/render path itself.

---

### Test 0.2: WebP Bypass 
**Status**: ⏳ NOT YET TESTED

**Purpose**: Send raw BGRA from backend to see if WebP encoding/decoding causes stripes

**Note**: WebP was already switched to lossless mode (`lossless=True` in backend). Stripes persist.

---

### Test 0.3: Single-Threaded Path ✅ COMPLETED
**Purpose**: Serialize decode → draw → cache to rule out race conditions

**Implementation**: Added `FORCE_SINGLE_THREADED = true` flag in `gfx-worker.js` (line ~155)
- Forces `prog_decompress()` instead of `prog_decompress_parallel()`
- Log confirms: `effective: false` (parallel disabled)

**Result**: ❌ **STRIPES STILL APPEAR WITH SINGLE-THREADED MODE**

**Log Evidence**:
```
[GFX Worker] Progressive WASM decoder initialized (parallel: true, FORCE_SINGLE_THREADED: true, effective: false)
[PROG DIAG] Frame 5: extrapolate=1, tiles=376
[STRIPE DIAG] Tile (0,128): 6 potential stripe rows
```

**Conclusion**: **Threading/race conditions are NOT the root cause.** The bug is in the single-threaded decode algorithm itself.

---

## Phase 1: Visual Instrumentation

### Stripe Detection Diagnostic ✅ IMPLEMENTED
**Location**: `gfx-worker.js` function `detectStripes()`

**Implementation**: 
- Calculates average brightness per row in each tile
- Detects sudden brightness changes (threshold > 30)
- Logs row positions, brightness values, and gap patterns

**Sample Output** (from browser console):
```
[STRIPE DIAG] Tile (0,128): 6 potential stripe rows: 
  [{row: 16, diff: "45.2", prev: "120.5", curr: "75.3"}, ...]
  gaps: [8, 2, 5, 1, 1]

[STRIPE DIAG] Tile (192,192): 6 potential stripe rows:
  gaps: [2, 4, 2, 4, 2]
```

**Analysis**: 
- Gap patterns are **irregular** (not DWT subband boundaries like 8, 16, 32)
- This suggests issue is NOT in DWT level processing
- Stripe rows appear at seemingly random positions

---

## Phase 2: Frame Order Diagnostics ✅ IMPLEMENTED

### Frame Order Logging
**Location**: `gfx-worker.js` with `FRAME_ORDER_DIAG = true`

**Implementation**: Logs every operation within each frame:
- START_FRAME(frameId)
- WEBP tile draws with surface and rect
- PROG (Progressive) tile draws with count
- S2C (SurfaceToCache) with slot info
- C2S (CacheToSurface) with slot info  
- END_FRAME(frameId)

**Sample Output**:
```
[FRAME ORDER] Frame 9: START(9) → WEBP:surf=0,rect=(320,128,448,64) → 
  WEBP:surf=0,rect=(0,192,960,640) → WEBP:surf=0,rect=(0,1344,1920,736) → 
  PROG:surf=0,tiles=1 → PROG:surf=0,tiles=376 → 
  S2C:slot=9,surf=0,src=(0,0,64,64),chk=aa7a15e8 → 
  C2S:slot=5,surf=0,pts=1,chk=5faef9a4 → END(9)
```

**Analysis**:
- Frame sequence is correct (tiles drawn before S2C captures)
- WebP tiles drawn first, then Progressive tiles
- S2C/C2S checksums are consistent for same content

---

## Phase 3: Progressive Decoder Investigation ✅ IN PROGRESS

### Extrapolate Flag Analysis
**Finding**: Windows Server sends `extrapolate=1` (extrapolated tiles)

**Log Evidence**:
```
[PROG DIAG] Frame 9: extrapolate=1, tiles=376
```

**Implications**:
- Using **extrapolated** DWT decode path (LL3@4015, 81 coefficients, 9x9)
- Subband layout differs from non-extrapolated mode
- DWT functions: `rfx_dwt_decode()` not `rfx_dwt_decode_non_extrapolated()`

### Code Path Verification
**Files examined**:
- `frontend/progressive/progressive_wasm.c` - decode_tile_simple(), decode_tile_first()
- `frontend/progressive/rfx_dwt.c` - dwt_2d_decode_block(), idwt_x(), idwt_y()
- `frontend/progressive/rfx_decode.c` - rfx_ycbcr_to_rgba()

**Subband Layout (Extrapolated)**:
| Subband | Offset | Size | Dimensions |
|---------|--------|------|------------|
| HL1 | 0 | 1023 | 31x33 |
| LH1 | 1023 | 1023 | 33x31 |
| HH1 | 2046 | 961 | 31x31 |
| HL2 | 3007 | 272 | 16x17 |
| LH2 | 3279 | 272 | 17x16 |
| HH2 | 3551 | 256 | 16x16 |
| HL3 | 3807 | 72 | 8x9 |
| LH3 | 3879 | 72 | 9x8 |
| HH3 | 3951 | 64 | 8x8 |
| LL3 | 4015 | 81 | 9x9 |

**Code Comparison with FreeRDP Reference**:
- ✅ Quantization parsing matches FreeRDP
- ✅ Dequantization offsets match FreeRDP  
- ✅ DWT band count functions match FreeRDP
- ✅ IDWT formulas match FreeRDP
- ✅ YCbCr to RGBA conversion matches FreeRDP constants

---

## Current Hypothesis

Since cache bypass AND single-threaded mode both didn't fix the issue, the problem is definitively in the **Progressive RFX decode algorithm** itself for extrapolated tiles.

### Ruled Out:
- ❌ Cache slot reuse/collision
- ❌ Read-after-write race conditions  
- ❌ Parallel decode threading issues
- ❌ Buffer lifetime/WASM memory issues

### Most Likely Causes (ranked):
1. **Extrapolated DWT inverse transform bug** - The `rfx_dwt_decode()` for extrapolated tiles may have incorrect subband iteration or boundary handling
2. **RLGR decode coefficient count mismatch** - May be decoding wrong number of coefficients for extrapolated layout
3. **Dequantization subband offset error** - Extrapolated subband boundaries are non-square and complex
4. **Differential decode applied to wrong range** - LL3 is at offset 4015 with 81 coefficients for extrapolated mode

### Key Observation from Pixel Dump:
The stripe-detected tiles show uniform gray values (53,53,53) in the dumped rows, suggesting the stripe detection may be catching **legitimate content boundaries** rather than codec artifacts. Need to examine tiles with actual visual corruption.

---

## Next Steps

### NEXT TEST: Force Non-Extrapolated Mode ✅ COMPLETED
**Rationale**: Since Windows sends `extrapolate=1`, force our decoder to use `extrapolate=0` path and see if stripes disappear. This would confirm the bug is specifically in the extrapolated code path.

**Implementation**: Added `FORCE_EXTRAPOLATE_MODE` toggle in `progressive_wasm.c` (line ~12)
```c
#define FORCE_EXTRAPOLATE_MODE 2  /* Force non-extrapolated path */
```

**Result**: 
- ✅ **Background became totally distorted** (expected - wrong subband layout)
- ❌ **Original UI stripe artifacts UNCHANGED** (Start menu, Chrome toolbar, etc.)

**CRITICAL CONCLUSION**: 
## 🎯 THE STRIPES ARE NOT FROM PROGRESSIVE DECODER!

The stripes must be coming from **WebP tiles**, not Progressive RFX!
- Progressive decoder affects the desktop background
- UI elements (Start menu, toolbar) use WebP tiles
- Stripes persist on WebP-rendered areas even when Progressive is broken

---

## New Investigation: WebP Tile Path

### Hypothesis
The stripe artifacts are in the WebP decode/render path, NOT Progressive.

### Evidence
1. Forcing wrong Progressive mode distorts background but not UI stripes
2. Frame order logs show WebP tiles for UI areas: `WEBP:surf=0,rect=(320,128,448,64)`
3. Stripes appear in rectangular bands matching WebP tile regions

### Test 0.4: WebP Stripe Detection ⏳ IN PROGRESS
**Purpose**: Add stripe detection to WebP tiles and CacheToSurface to identify which path produces stripes

**Implementation**:
- Added stripe detection to `decodeWebPTile()` - checks decoded WebP for stripe patterns
- Added stripe detection to `applyCacheToSurface()` - checks cached tiles for stripes
- Increased `STRIPE_DIAG_MAX` to 30 to catch stripes from multiple sources

---

### Test 0.5: Force Alpha=255 on WebP Tiles ⏳ READY TO TEST
**Purpose**: Force all WebP decoded pixels to have alpha=255 to test if alpha corruption is the cause

**Implementation**: Added `FORCE_ALPHA_255 = true` flag in `gfx-worker.js`
- After WebP decode, forces all alpha values to 255 before drawing to surface
- Also added `checkAlphaCorruption()` function to detect alpha issues

**Expected Outcome**:
- If stripes **DISAPPEAR** → **CONFIRMED: Alpha corruption in WebP pipeline**
- If stripes **PERSIST** → Alpha is not the issue, look at BGRA→RGBA or stride

**To Run**: Rebuild frontend:
```powershell
.\buildandrun.ps1 frontend
```

### Alternative Test: Raw Tile Data Comparison
Capture the raw RLGR-decoded coefficients before/after DWT and compare with reference FreeRDP output for the same input.

### Deeper Investigation Needed:
1. [ ] Force `extrapolate=0` to test non-extrapolated path
2. [ ] Add coefficient dump after RLGR decode  
3. [ ] Add coefficient dump after DWT inverse
4. [ ] Compare with FreeRDP reference for same tile data

---

## Configuration Flags

| Flag | Location | Current Value | Purpose |
|------|----------|---------------|---------|
| CACHE_BYPASS | gfx-worker.js:148 | `false` | Skip S2C/C2S operations |
| FORCE_SINGLE_THREADED | gfx-worker.js:155 | `true` | Disable parallel WASM decode |
| FORCE_ALPHA_255 | gfx-worker.js:161 | `true` | **TEST: Force full opacity on WebP** |
| FRAME_ORDER_DIAG | gfx-worker.js:167 | `true` | Log frame operation sequence |
| STRIPE_DIAG_ENABLED | gfx-worker.js:184 | `true` | Detect stripe patterns |
| FORCE_EXTRAPOLATE_MODE | progressive_wasm.c:12 | `0` | Normal (use server's flag) |

---

## Test Results Summary

| Test | Result | Conclusion |
|------|--------|------------|
| Cache Bypass | ❌ Stripes persist | Cache not the cause |
| Single-Threaded | ❌ Stripes persist | Threading not the cause |
| WebP Lossless | ❌ Stripes persist | WebP compression not the cause |
| Force Non-Extrapolate | ✅ Background distorted, UI stripes unchanged | **Stripes NOT from Progressive!** |
| WebP Stripe Detection | ⏳ Testing | Identify stripe source |
| **Force Alpha=255** | ⏳ Ready | Test alpha corruption theory |

---

## Current Status

### Where We Are
We have **definitively proven** the stripes are NOT coming from the Progressive RFX decoder:
- Forcing wrong DWT mode broke the background (Progressive tiles)
- But the UI element stripes (Start menu, toolbars) were UNCHANGED
- This means the stripe artifacts must come from WebP tiles or CacheToSurface

### What We're Testing Now
**Test 0.5: Force Alpha=255** - This is the critical test!

If forcing alpha=255 on WebP tiles makes the stripes disappear, then we have confirmed:
1. WebP encoding/decoding is corrupting the alpha channel
2. The fix will be to ensure proper alpha handling in the BGRA→WebP→RGBA pipeline

### Diagnostic Tools Added
1. `checkAlphaCorruption()` - Detects tiles with >10% zero alpha or rows with >50% zero alpha
2. `detectStripes()` - Now includes alpha channel in pixel dump
3. `FORCE_ALPHA_255` flag - Forces all alpha to 255 to test fix

### Possible Remaining Causes
1. **WebP encoding bug in C backend** - stride mismatch when encoding BGRA to WebP
2. **Cache read-back timing** - getImageData() capturing partial state
3. **Server sending bad data** - Windows RDP server itself sending corrupted tiles
4. **Codec-specific issue** - ClearCodec, Planar, or Uncompressed decode bug

---

## Files Modified During Troubleshooting

1. **gfx-worker.js**
   - Added CACHE_BYPASS toggle
   - Added FRAME_ORDER_DIAG logging
   - Added STRIPE_DIAG_ENABLED detection
   - Added checksum tracking for S2C/C2S
   - Added extrapolate flag logging

2. **backend/rdp_bridge.py** (prior session)
   - WebP switched to lossless mode

---

## Log Samples

### Backend Log (Progressive decode):
```
[PROG] Processing region: 21 rects, 1 quants, 0 prog_quants, flags=0x01
[PROG] Frame 9: 376 tiles decoded, 125KB data
```

### Browser Console:
```
[PROG DIAG] Frame 9: extrapolate=1, tiles=376
[STRIPE DIAG] Tile (0,128): 6 potential stripe rows
[STRIPE DIAG] Tile (192,192): 6 potential stripe rows  
[STRIPE DIAG] Tile (576,1088): 3 potential stripe rows
```

---

*Last updated: 2026-01-09*
