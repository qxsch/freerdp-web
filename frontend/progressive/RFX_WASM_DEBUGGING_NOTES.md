# RFX Progressive WASM Decoder - Debugging Notes

This document tracks issues discovered and fixes applied during the Progressive RFX codec WASM implementation.

---

## 🎉 PROGRESSIVE CODEC FIXED! (2026-01-06)

All major issues have been resolved. The progressive RFX codec now renders correctly without sparkles or artifacts.

---

## Current Status Summary (2026-01-06)

| Component | Status | Notes |
|-----------|--------|-------|
| RLGR1 Decoder | ✅ Verified | Sign conversion matches FreeRDP exactly |
| Subband Layout | ✅ Verified | Extrapolated 4096 coefficients, LL3@4015 |
| Quant Parsing | ✅ FIXED | Nibble order was wrong - now matches FreeRDP |
| Dequantization | ✅ FIXED | Progressive upgrade was double-dequantizing |
| DWT Inverse | ✅ Verified | Heap-allocated temp buffer, no race conditions |
| Color Conversion | ✅ Verified | Y_OFFSET=4096, correct coefficients |
| Block Parsing | ✅ Verified | SYNC, CONTEXT, FRAME_BEGIN/END all handled |
| Progressive Upgrade | ✅ FIXED | Now stores dequantized coefficients correctly |

---

## Issue #8: Progressive Upgrade Double-Dequantization (CRITICAL BUG - FIXED!)

### Symptom
- Blue dots at tile corners (RGB ≈ 13, 0, 255) repeating every 64 pixels
- Red lines at top edge transitioning from RGB(255,78,180) to RGB(255,0,0) across tiles
- Colors transitioning/accumulating across tiles (key diagnostic clue!)
- Extreme Cb values: 8573, 9216, 7936 (normal range: -2000 to +2000)

### Root Cause
The progressive codec has two passes:
1. **First pass** (`decode_tile_first`): RLGR decode → differential decode → dequantize → DWT
2. **Upgrade pass** (`decode_tile_upgrade`): SRL decode → add to coefficients → dequantize → DWT

The bug was in how coefficients were stored and processed between passes:

**BEFORE (BROKEN):**
```c
// decode_tile_first:
rfx_rlgr_decode(yData, yLen, tile->yData, TILE_PIXELS);  // UN-dequantized
rfx_differential_decode(&tile->yData[4015], 81);
memcpy(yBuffer, tile->yData, ...);                        // Copy UN-dequantized
rfx_dequantize_progressive(yBuffer, ...);                 // Dequantize working buffer
rfx_dwt_decode(yBuffer, ...);
// tile->yData still contains UN-dequantized values!

// decode_tile_upgrade:
rfx_srl_decode(..., tile->yData, ...);                    // Add to UN-dequantized
memcpy(yBuffer, tile->yData, ...);
rfx_dequantize_progressive(yBuffer, ...);                 // DOUBLE dequantize old values!
```

The problem: In upgrade pass, old coefficients (from first pass) got dequantized TWICE,
while new SRL coefficients only got dequantized ONCE. This caused:
- Old coefficients to be left-shifted twice (values explode: 100 → 100<<5<<5 = 102400!)
- Accumulating error across tiles (explains the "transitioning colors" symptom)

### Applied Fix

**AFTER (CORRECT):**
```c
// decode_tile_first:
rfx_rlgr_decode(yData, yLen, tile->yData, TILE_PIXELS);
rfx_differential_decode(&tile->yData[4015], 81);
memcpy(yBuffer, tile->yData, ...);
rfx_dequantize_progressive(yBuffer, ...);
memcpy(tile->yData, yBuffer, ...);                        // ← STORE DEQUANTIZED!
rfx_dwt_decode(yBuffer, ...);
// tile->yData now contains DEQUANTIZED values

// decode_tile_upgrade:
rfx_srl_decode(..., tile->yData, ...);                    // Add to DEQUANTIZED
memcpy(yBuffer, tile->yData, ...);
// NO rfx_dequantize_progressive() call!                  // ← Already dequantized!
rfx_dwt_decode(yBuffer, ...);
```

### Key Insight
FreeRDP uses separate buffers:
- `current` - stores DEQUANTIZED coefficients accumulated across passes
- `buffer` - working buffer for each pass's DWT

My implementation used `tile->yData` for both purposes incorrectly. The fix was to:
1. Store DEQUANTIZED coefficients back to `tile->yData` after first pass
2. Skip dequantization in upgrade pass (coefficients already dequantized)

### Files Changed
- `progressive_wasm.c`: 
  - `decode_tile_first()`: Added `memcpy(tile->yData, yBuffer, ...)` after dequantize
  - `decode_tile_upgrade()`: Removed `rfx_dequantize_progressive()` calls

---

## Issue 2: Quantization Value Parsing (CRITICAL BUG)

### Symptom
Extreme Y/Cb/Cr values after DWT causing massive sparkles and noise. Y values ranging from -726 to 9383 instead of expected 0-8192.

### Root Cause
The `parse_quant_vals` and `parse_prog_quant_vals` functions had incorrect nibble assignment order.

FreeRDP's byte order (from `progressive_component_codec_quant_read`):
```
byte 0: LL3 (low nibble), HL3 (high nibble)
byte 1: LH3 (low nibble), HH3 (high nibble)
byte 2: HL2 (low nibble), LH2 (high nibble)
byte 3: HH2 (low nibble), HL1 (high nibble)
byte 4: LH1 (low nibble), HH1 (high nibble)
```

Our code was incorrectly parsing as:
```
byte 0: LL3 (low), LH3 (high)   // WRONG! Should be HL3
byte 1: HL3 (low), HH3 (high)   // WRONG! LH3/HH3
... etc
```

This caused wrong shift amounts to be applied to each subband during dequantization, resulting in over-amplified or under-amplified coefficients.

### Applied Fix
Corrected nibble parsing in `parse_quant_vals` and `parse_prog_quant_vals` to match FreeRDP exactly.

---

## Issue 1: Color Conversion Coefficient Swap

### Symptom
Incorrect colors in decoded tiles - colors appeared shifted/wrong hue.

### Root Cause
In `rfx_decode.c`, the CbG and CrG coefficients were swapped in the YCbCr to RGB conversion formula.

The ITU-R BT.601 formula is:
```
G = Y - 0.343730 * Cb - 0.714401 * Cr
```

FreeRDP's `ycbcr_constants[16]` array is `{ 91916, 46819, 22527, 115992 }` where:
- Index 0 (CrR) = 91916 = 1.402525 × 65536
- Index 1 (CrG) = 46819 = 0.714401 × 65536
- Index 2 (CbG) = 22527 = 0.343730 × 65536
- Index 3 (CbB) = 115992 = 1.769905 × 65536

### Applied Fix
Corrected the coefficient assignment in both `rfx_ycbcr_to_bgra` and `rfx_ycbcr_to_rgba`:

```c
// CORRECT:
int64_t CbG = (int64_t)Cb * 22527;   /* constant[2] = 0.343730 * 65536 */
int64_t CrG = (int64_t)Cr * 46819;   /* constant[1] = 0.714401 * 65536 */

// WRONG (was):
int64_t CbG = (int64_t)Cb * 46819;   // These were swapped!
int64_t CrG = (int64_t)Cr * 22527;
```

---

## Issue 2: Static DWT Temp Buffer Race Condition

### Symptom
"Sparkles" and "snow" artifacts appearing randomly across tiles. Debug logs showed interleaved tile processing with inconsistent DWT values between tiles.

Example log showing interleaving:
```
[TILE 0,0] Y_LL3=-35,-39,-37,-38 DWT[0]=-2304 center=-2464 | Cb_LL3=13 DWT=896
[TILE 1,0] ...
[TILE 0,0] Cb DWT=2432  // Wrong! Different value for same tile!
```

### Root Cause
The `idwt_temp` buffer was declared as `static __thread` (thread-local) in `rfx_dwt.c`. However, in Emscripten/WASM with Web Workers, all workers may share the same WASM instance memory, causing the "thread-local" buffer to be shared and corrupted during concurrent tile decoding.

```c
// PROBLEM: static buffer shared between concurrent workers
static __thread int16_t idwt_temp[65 * 65];
```

### Applied Fix
Changed the temp buffer to stack allocation within `rfx_dwt_decode()` so each DWT call gets its own isolated buffer:

```c
void rfx_dwt_decode(int16_t* buffer, int size)
{
    /* Allocate temp buffer on stack - each DWT call gets its own buffer
     * to avoid race conditions when multiple tiles are decoded concurrently
     * in Web Workers sharing the same WASM instance. */
    int16_t idwt_temp[65 * 65];
    
    /* CRITICAL: Zero the temp buffer to avoid stale stack data */
    memset(idwt_temp, 0, sizeof(idwt_temp));
    
    // ... rest of function
}
```

---

## Issue 3: Undefined Behavior in Left Shift Operations

### Symptom
Potential undefined behavior and incorrect coefficient scaling during dequantization.

### Root Cause
Left-shifting negative `int16_t` values is undefined behavior in C. The dequantization step left-shifts coefficients by `(quant - 1)` or `(quant + progQuant - 1)`, which could produce incorrect results.

```c
// PROBLEM: UB when buffer[i] is negative
buffer[i] = buffer[i] << shift;
```

### Applied Fix
Created a safe `lshift16` function that uses unsigned arithmetic:

```c
static inline int16_t lshift16(int16_t val, int sh)
{
    return (int16_t)(((uint32_t)val << sh) & 0xFFFF);
}
```

Applied to both `rfx_dequantize` and `rfx_dequantize_progressive`:

```c
buffer[i] = lshift16(buffer[i], shift);
```

---

## Issue 4: Missing Buffer Zeroing Before RLGR Decode

### Symptom
Garbage values in coefficient buffer causing incorrect DWT reconstruction.

### Root Cause
The tile coefficient buffers (`tile->yData`, `tile->cbData`, `tile->crData`) were not zeroed before RLGR decode. RLGR may not fill all 4096 coefficient slots, leaving uninitialized data.

### Applied Fix
Added `memset` before each RLGR decode in `decode_tile_first`:

```c
memset(tile->yData, 0, TILE_PIXELS * sizeof(int16_t));
rfx_rlgr_decode(yData, yLen, tile->yData, TILE_PIXELS);
```

---

## ~~Outstanding Issue: "Sparkles"~~ - RESOLVED! (2026-01-06)

### Original Symptom
- Blue dots at tile corners repeating every 64 pixels
- Red lines at edges with colors transitioning across tiles  
- Extreme Cb values (8573, 9216) causing color overflow

### Resolution
This was caused by the **double-dequantization bug** documented in Issue #8 above.

The "sparkles" were NOT legitimate clamping artifacts as previously theorized - they were caused by coefficient values exploding due to being left-shifted twice in the progressive upgrade path.

**Key diagnostic clue:** Colors were TRANSITIONING across tiles (e.g., red line going from pink to pure red). This indicated accumulating error, not random noise.

### See Issue #8 for full details and fix.

---

## Verified Investigation Areas ✅
1. ✅ **RLGR Decode**: Sign conversion verified correct (see Issue #7)
2. ✅ **Differential Decode**: LL3 at offset 4015 with 81 coefficients - correct
3. ✅ **Subband Layout**: Verified against FreeRDP progressive.c lines 818-827
4. ✅ **DWT Algorithm**: Structure matches FreeRDP
5. ✅ **Dequantization**: Uses left-shift by (quant-1) - matches FreeRDP
6. ✅ **Progressive Upgrade**: Fixed double-dequantization bug (Issue #8)

---

## Reference: YCbCr to RGB Conversion Formula

From FreeRDP's `prim_colors.c` with `divisor = 16`:

```c
const INT32 Y = (INT32)((UINT32)((*pY++) + 4096) << 16);
const INT32 Cb = (*pCb++);
const INT32 Cr = (*pCr++);
const INT32 CrR = Cr * 91916;   // 1.402525 * 65536
const INT32 CrG = Cr * 46819;   // 0.714401 * 65536
const INT32 CbG = Cb * 22527;   // 0.343730 * 65536
const INT32 CbB = Cb * 115992;  // 1.769905 * 65536
const INT32 R = (CrR + Y) >> 21;       // >> (16 + 5)
const INT32 G = (Y - CbG - CrG) >> 21;
const INT32 B = (CbB + Y) >> 21;
```

The `+ 4096` offset corresponds to Y level shift (128 in 8-bit terms, but DWT output is scaled).

---

## Reference: Dequantization Formula

From MS-RDPEGFX spec:
```
Scale_value = (1 << (quantization_factor - 6))
```

FreeRDP combines with a factor of 5 from color conversion:
```c
shift = quant + progQuant - 1;  // -6 + 5 = -1 offset
```

---

## Reference: Inverse DWT Lifting Equations

5/3 DWT inverse (from FreeRDP `progressive_rfx_idwt_x`):

```c
X0 = L0 - H0;                           // First sample
X2 = L0 - (H0 + H1) / 2;                // Even samples (low-pass contribution)
X1 = (X0 + X2) / 2 + 2 * H0;            // Odd samples (high-pass contribution)
```

Boundary handling uses mirroring for edge coefficients.

---

## Issue #5: Batch API vs Per-Tile API Architectural Mismatch

**Status: UNDER INVESTIGATION - SUSPECTED ROOT CAUSE**

### Observation

Our WASM implementation processes tiles individually as they are parsed, while FreeRDP uses a batch-oriented pipeline that collects all tiles first, then processes them as a group.

### FreeRDP's Batch Processing Flow (from progressive.c)

```
progressive_decompress() [line 2317]
├── Loop: progressive_parse_block() [line 2163]
│   ├── PROGRESSIVE_WBT_SYNC → progressive_wb_sync()
│   ├── PROGRESSIVE_WBT_CONTEXT → progressive_wb_context()
│   ├── PROGRESSIVE_WBT_FRAME_BEGIN → progressive_wb_frame_begin()
│   ├── PROGRESSIVE_WBT_REGION → progressive_wb_region()
│   │   └── progressive_process_tiles() [line 1598]
│   │       ├── PASS 1: Read ALL tiles from stream into region->tiles[]
│   │       └── PASS 2: Process ALL tiles (optional thread pool)
│   │           └── progressive_process_tiles_tile_work_callback()
│   └── PROGRESSIVE_WBT_FRAME_END → progressive_wb_frame_end()
└── update_tiles() [line 2232] - Copy decoded tiles to destination surface
```

Key characteristics of FreeRDP's approach:
1. **Two-pass tile processing**: First reads all tile headers/data, then processes
2. **Deferred surface writing**: All tiles written to surface at END of frame via `update_tiles()`
3. **Thread pool option**: Tiles can be processed in parallel because they're independent after parsing
4. **State isolation**: Region context (quant tables, prog quant tables) fully parsed before any tile decode

### Our WASM Implementation (progressive_wasm.c)

```
prog_decompress() [line 855]
├── Loop: Parse blocks inline
│   ├── PROGRESSIVE_WBT_SYNC → (empty handler)
│   ├── PROGRESSIVE_WBT_CONTEXT → (empty handler)
│   ├── PROGRESSIVE_WBT_FRAME_BEGIN → (empty handler)
│   ├── PROGRESSIVE_WBT_REGION → decode_region() [line 789]
│   │   ├── Parse quant tables
│   │   └── Loop: Process tiles IMMEDIATELY
│   │       ├── decode_tile_simple()
│   │       ├── decode_tile_first()
│   │       └── decode_tile_upgrade()
│   └── PROGRESSIVE_WBT_FRAME_END → (empty handler)
└── (No explicit surface write - tiles marked dirty)
```

### Potential Consequences

1. **RLGR/DWT State Issues**: If any global state is used during decode, per-tile processing could leave it in inconsistent state
2. **Parameter Scoping**: Quant tables parsed correctly, but are they stable across all tile decodes?
3. **Incomplete Tile Group Commits**: No atomic batch commit - partial frame visible?
4. **Missing Block Handlers**: SYNC, CONTEXT, FRAME_BEGIN, FRAME_END blocks have empty handlers

### Investigation Needed

- [ ] Verify SYNC block processing (magic validation)
- [ ] Verify CONTEXT block processing (tile size configuration)
- [ ] Verify FRAME_BEGIN/END semantics (frame boundaries)
- [ ] Compare tile accumulation patterns

---

## Issue #6: Surface Writing Behavior Differences

**Status: UNDER INVESTIGATION**

### Observation

FreeRDP writes decoded tiles to the final surface buffer in a dedicated `update_tiles()` function called AFTER all block processing completes. Our WASM marks tiles as "dirty" inline during decode.

### FreeRDP's update_tiles() (lines 2232-2317)

```c
static BOOL update_tiles(PROGRESSIVE_CONTEXT* progressive, 
                         PROGRESSIVE_SURFACE_CONTEXT* surface,
                         BYTE* pDstData, UINT32 DstFormat, UINT32 nDstStep,
                         UINT32 nXDst, UINT32 nYDst, const REGION16* pClipRegion)
{
    // Iterates over surface->numUpdatedTiles
    // Uses surface->updatedTileIndices[] to track which tiles changed
    // Calls freerdp_image_copy_no_overlap() for each tile → destination
    // Applies region clipping
}
```

Key characteristics:
1. **Deferred write**: Surface only updated after ALL tiles decoded
2. **Index tracking**: `surface->numUpdatedTiles` and `surface->updatedTileIndices[]`
3. **Clipping**: Region clipping applied during surface write
4. **No-overlap copy**: Uses `freerdp_image_copy_no_overlap()` for safety

### Symptom

"Delayed black tiles during mouse movement" - suggests surface ops or tile buffer reuse timing issues.

### Our WASM Approach

- Tiles marked `tile->dirty = true` during decode
- JavaScript polls `prog_get_tile_data()` and `prog_get_dirty_tile_count()`
- Surface compositing happens in JavaScript/Canvas
- No frame-level atomic commit

### Potential Consequences

1. **Race conditions**: If JS reads tiles while still being decoded
2. **Partial frame rendering**: Dirty tiles visible before frame complete
3. **Buffer reuse**: Tile buffers might be reused before JS consumes them
4. **Missing synchronization**: No frame begin/end signals to JS

---

## Verified Checks (All Match FreeRDP Exactly)

The following components have been exhaustively verified against FreeRDP's implementation:

### ✅ RLGR1 Entropy Decoder (rfx_rlgr.c)

| Component | FreeRDP | Our Implementation | Status |
|-----------|---------|-------------------|--------|
| Initial k | 1 | 1 | ✅ Match |
| Initial kp | 8 | 8 | ✅ Match |
| Initial kr | 1 | 1 | ✅ Match |
| Initial krp | 8 | 8 | ✅ Match |
| RL sign handling | `sign ? -(code+1) : (code+1)` | Same | ✅ Match |
| GR sign handling | `(code & 1) ? -((code+1)>>1) : (code>>1)` | Same | ✅ Match |
| Bitstream | 32-bit acc + prefetch | Same | ✅ Match |

### ✅ Extrapolated Subband Layout (rfx_dwt.c)

Verified against FreeRDP progressive.c lines 818-827:

| Subband | Offset | Size | Dimensions | FreeRDP Reference |
|---------|--------|------|------------|-------------------|
| HL1 | 0 | 1023 | 31×33 | buffer[0], 1023 |
| LH1 | 1023 | 1023 | 33×31 | buffer[1023], 1023 |
| HH1 | 2046 | 961 | 31×31 | buffer[2046], 961 |
| HL2 | 3007 | 272 | 16×17 | buffer[3007], 272 |
| LH2 | 3279 | 272 | 17×16 | buffer[3279], 272 |
| HH2 | 3551 | 256 | 16×16 | buffer[3551], 256 |
| HL3 | 3807 | 72 | 8×9 | buffer[3807], 72 |
| LH3 | 3879 | 72 | 9×8 | buffer[3879], 72 |
| HH3 | 3951 | 64 | 8×8 | buffer[3951], 64 |
| **LL3** | **4015** | **81** | **9×9** | buffer[4015], 81 |
| **Total** | - | **4096** | 65×65 extrapolated | ✅ Matches |

### ✅ Dequantization Formula

```c
shift = quant + progQuant - 1;
value <<= shift;
```

### ✅ Block Structure Parsing

| Block Type | Value | Handler |
|------------|-------|---------|
| PROGRESSIVE_WBT_SYNC | 0xCCC0 | ✅ Validates magic 0xCACCACCA, version 0x0100 |
| PROGRESSIVE_WBT_FRAME_BEGIN | 0xCCC1 | ✅ Parses frameIndex, regionCount |
| PROGRESSIVE_WBT_FRAME_END | 0xCCC2 | ✅ Sets FLAG_WBT_FRAME_END |
| PROGRESSIVE_WBT_CONTEXT | 0xCCC3 | ✅ Parses ctxId, tileSize (validates =64), flags |
| PROGRESSIVE_WBT_REGION | 0xCCC4 | decode_region() |
| PROGRESSIVE_WBT_TILE_SIMPLE | 0xCCC5 | decode_tile_simple() |
| PROGRESSIVE_WBT_TILE_FIRST | 0xCCC6 | decode_tile_first() |
| PROGRESSIVE_WBT_TILE_UPGRADE | 0xCCC7 | decode_tile_upgrade() |

### ✅ Tile Header Sizes

- SIMPLE tile: 16-byte header
- FIRST tile: 17-byte header (extra byte for quantProgIdx)
- UPGRADE tile: 17-byte header

---

## Implementation Progress

### ✅ Block Handlers (Implemented 2026-01-06)

All block handlers now implemented matching FreeRDP's validation:
- **SYNC**: Validates magic=0xCACCACCA, version=0x0100, blockLen=12
- **FRAME_BEGIN**: Parses frameIndex, regionCount, sets FLAG_WBT_FRAME_BEGIN
- **FRAME_END**: Sets FLAG_WBT_FRAME_END, clears FRAME_BEGIN for next frame
- **CONTEXT**: Parses ctxId, tileSize (validates =64), flags

### ✅ Tile Index Tracking (Implemented 2026-01-06)

Added FreeRDP-style tile tracking:
- `ctx->numUpdatedTiles` - count of tiles updated in current frame
- `ctx->updatedTileIndices[]` - array of tile grid indices
- New APIs for JavaScript:
  - `prog_get_updated_tile_count()` - returns numUpdatedTiles
  - `prog_get_updated_tile_index(listIndex)` - returns tile index at position
  - `prog_get_frame_state()` - returns state flags bitmask
  - `prog_is_frame_complete()` - returns 1 if FRAME_END received

### Next Steps

1. **JavaScript Integration**
   - [ ] Update rdp-client.js to use new batch tile APIs
   - [ ] Wait for `prog_is_frame_complete()` before rendering
   - [ ] Use `prog_get_updated_tile_count/index()` for efficient tile iteration

2. **Surface Write Synchronization**
   - [ ] Ensure JS only reads tiles after frame complete
   - [ ] Consider double-buffering for smoother updates

3. **Completed Investigations** ✅
   - [x] RLGR1 RL mode sign conversion - verified matches FreeRDP
   - [x] RLGR1 GR mode sign conversion - verified matches FreeRDP (see Issue #7)
   - [x] Subband layout for extrapolated tiles - verified correct
   - [x] Dequantization formula (left shift by quant-1) - verified correct
   - [x] Y_OFFSET_FP = 4096 (128 << 5) - verified matches FreeRDP
   - [x] Differential decode at buffer[4015] for LL3 - verified correct

4. **Resolved: Sparkle Investigation**
   - [x] Analyzed edge-console.log "bad pixel" reports
   - [x] Confirmed sparkles are color-space clamping, not decode errors
   - [x] Verified RGB output values are correct (e.g., [13,71,104] for dark blue desktop)

---

## Issue #8: Progressive Upgrade Double-Dequantization Bug (2026-01-06)

### Symptom
In progressive refinement (second pass onward), tiles show extreme coefficient values:
- `Cb=8573`, `Cb=9216`, `Cb=7936` (normal range: -2000 to +2000)
- Colors transition across tiles (e.g., blue dots fading from white to blue across tiles)
- Red lines transitioning from (255,78,180) to (255,0,0) per tile

### Root Cause
**CRITICAL BUG**: In `decode_tile_upgrade()`, the code calls `rfx_dequantize_progressive()` on the ENTIRE coefficient buffer, but the buffer ALREADY contains dequantized coefficients from the previous pass!

FreeRDP's architecture:
1. `current` buffer stores **dequantized** coefficients between passes
2. During upgrade, new SRL bits are shifted by NEW shift value and ADDED to `current`
3. DWT is performed on the combined result

Our buggy implementation:
1. `tile->yData` stores **un-dequantized** RLGR output
2. SRL adds values to un-dequantized coefficients  
3. Then we call `rfx_dequantize_progressive` which shifts ALL coefficients AGAIN

This causes:
- Old coefficients (from pass 1) get shifted TWICE → values explode
- Each subsequent pass multiplies old values by 2^shift

### Example Math
Pass 1: coefficient = 10, shift = 6 → stored value = 10 << 6 = 640
Pass 2: buggy code does (640 + new_value) << 5 = 640 << 5 = 20480 ← WRONG!
Correct: 640 + (new_value << 5)

### Fix Required
Change coefficient storage to match FreeRDP:
1. In first pass: RLGR → differential → dequantize → store in tile->yData (DEQUANTIZED)
2. In upgrade: Read SRL values, shift by NEW shift, ADD to tile->yData → DWT
3. Do NOT call rfx_dequantize_progressive in upgrade path

---

## Issue #7: RLGR1 Sign Conversion - VERIFIED CORRECT (2026-01-06)

### Investigation Summary

Extensively verified RLGR1 sign handling against FreeRDP's `rfx_rlgr.c`. Both RL mode and GR mode sign conversions match exactly.

### RL Mode Sign Conversion

**Our Code (rfx_rlgr.c lines 386-389):**
```c
if (sign)
    mag = (int16_t)(-((int32_t)code + 1));
else
    mag = (int16_t)(code + 1);
```

**FreeRDP (rfx_rlgr.c lines 353-358):**
```c
if (sign)
    mag = WINPR_ASSERTING_INT_CAST(int16_t, (code + 1)) * -1;
else
    mag = WINPR_ASSERTING_INT_CAST(int16_t, code + 1);
```

**Result:** ✅ EQUIVALENT - Both produce `sign ? -(code+1) : (code+1)`

### GR Mode Sign Conversion (RLGR1)

**Our Code (rfx_rlgr.c lines 503-507):**
```c
if (code & 1)
    mag = (int16_t)(-((int32_t)(code + 1) >> 1));
else
    mag = (int16_t)(code >> 1);
```

**FreeRDP (rfx_rlgr.c lines 481-488):**
```c
if (code & 1)
    mag = WINPR_ASSERTING_INT_CAST(INT16, (code + 1) >> 1) * -1;
else
    mag = WINPR_ASSERTING_INT_CAST(INT16, code >> 1);
```

**Result:** ✅ EQUIVALENT - Both decode `code = 2*|mag| - sign` correctly

### Mathematical Proof

RLGR1 GR mode encodes: `code = 2 * |magnitude| - sign` where sign=1 for negative

| Original | Encoded | code&1 | Decoded |
|----------|---------|--------|----------|
| +1 | 2*1-0=2 | 0 | 2>>1 = +1 ✅ |
| -1 | 2*1-1=1 | 1 | -((1+1)>>1) = -1 ✅ |
| +2 | 2*2-0=4 | 0 | 4>>1 = +2 ✅ |
| -2 | 2*2-1=3 | 1 | -((3+1)>>1) = -2 ✅ |
| +3 | 2*3-0=6 | 0 | 6>>1 = +3 ✅ |
| -3 | 2*3-1=5 | 1 | -((5+1)>>1) = -3 ✅ |

### Conclusion

**DO NOT MODIFY** - The RLGR sign conversion is verified correct and should not be changed.
