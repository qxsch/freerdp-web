/**
 * DWT (Discrete Wavelet Transform) Inverse Transform for Progressive Codec
 * Based on FreeRDP's progressive.c (Apache 2.0 License)
 * 
 * Performs 2D inverse DWT on extrapolated tiles (65x65) to reconstruct 64x64 image data.
 * 
 * CRITICAL: Progressive codec uses EXTRAPOLATED tile layout!
 * 
 * Extrapolated tiles have 4097 coefficients (not 4096) arranged as:
 * 
 *   Band      Offset    Dimensions   Size
 *   HL1       0         31x33        1023
 *   LH1       1023      33x31        1023
 *   HH1       2046      31x31        961
 *   HL2       3007      16x17        272
 *   LH2       3279      17x16        272
 *   HH2       3551      16x16        256
 *   HL3       3807      8x9          72
 *   LH3       3879      9x8          72
 *   HH3       3951      8x8          64
 *   LL3       4015      9x9          81
 *   Total:    4096 coefficients (indices 0-4095)
 */

#include "rfx_types.h"
#include <stdio.h>
#include <string.h>

/* NOTE: We no longer use a static temp buffer because Web Workers may share
 * the same WASM instance and corrupt each other's data. Instead, we allocate
 * the temp buffer on the stack within rfx_dwt_decode. */

/* Clamp value to int16 range */
static inline int16_t clampi16(int32_t val)
{
    if (val < -32768) return -32768;
    if (val > 32767) return 32767;
    return (int16_t)val;
}

/**
 * Get the number of low-frequency band samples for a given level
 * For extrapolated tiles:
 *   Level 1: (64 >> 1) + 1 = 33
 *   Level 2: (64 >> 2) + 1 = 17
 *   Level 3: (64 >> 3) + 1 = 9
 */
static inline size_t get_band_l_count(size_t level)
{
    return (64 >> level) + 1;
}

/**
 * Get the number of high-frequency band samples for a given level
 * For extrapolated tiles:
 *   Level 1: (64 >> 1) - 1 = 31
 *   Level 2: (64 + (1 << 1)) >> 2 = 16
 *   Level 3: (64 + (1 << 2)) >> 3 = 8
 */
static inline size_t get_band_h_count(size_t level)
{
    if (level == 1)
        return (64 >> 1) - 1;
    else
        return (64 + (1 << (level - 1))) >> level;
}

/**
 * Inverse DWT in horizontal direction
 * Combines LL+HL (or LH+HH) subbands into a single output row
 * 
 * Uses the lifting scheme from FreeRDP's progressive_rfx_idwt_x
 */
static void idwt_x(const int16_t* pLowBand, size_t nLowStep,
                   const int16_t* pHighBand, size_t nHighStep,
                   int16_t* pDstBand, size_t nDstStep,
                   size_t nLowCount, size_t nHighCount, size_t nDstCount)
{
    for (size_t i = 0; i < nDstCount; i++)
    {
        const int16_t* pL = pLowBand;
        const int16_t* pH = pHighBand;
        int16_t* pX = pDstBand;
        
        int16_t H0 = *pH++;
        int16_t L0 = *pL++;
        int16_t X0 = clampi16((int32_t)L0 - H0);
        int16_t X2 = clampi16((int32_t)L0 - H0);
        int16_t X1, H1;
        
        for (size_t j = 0; j < (nHighCount - 1); j++)
        {
            H1 = *pH++;
            L0 = *pL++;
            X2 = clampi16((int32_t)L0 - ((H0 + H1) / 2));
            X1 = clampi16((int32_t)((X0 + X2) / 2) + (2 * H0));
            pX[0] = X0;
            pX[1] = X1;
            pX += 2;
            X0 = X2;
            H0 = H1;
        }
        
        if (nLowCount <= (nHighCount + 1))
        {
            if (nLowCount <= nHighCount)
            {
                pX[0] = X2;
                pX[1] = clampi16((int32_t)X2 + (2 * H0));
            }
            else
            {
                L0 = *pL++;
                X0 = clampi16((int32_t)L0 - H0);
                pX[0] = X2;
                pX[1] = clampi16((int32_t)((X0 + X2) / 2) + (2 * H0));
                pX[2] = X0;
            }
        }
        else
        {
            L0 = *pL++;
            X0 = clampi16((int32_t)L0 - (H0 / 2));
            pX[0] = X2;
            pX[1] = clampi16((int32_t)((X0 + X2) / 2) + (2 * H0));
            pX[2] = X0;
            L0 = *pL++;
            pX[3] = clampi16((int32_t)(X0 + L0) / 2);
        }
        
        pLowBand += nLowStep;
        pHighBand += nHighStep;
        pDstBand += nDstStep;
    }
}

/**
 * Inverse DWT in vertical direction
 * Combines L and H intermediate results into final output
 * 
 * Uses the lifting scheme from FreeRDP's progressive_rfx_idwt_y
 */
static void idwt_y(const int16_t* pLowBand, size_t nLowStep,
                   const int16_t* pHighBand, size_t nHighStep,
                   int16_t* pDstBand, size_t nDstStep,
                   size_t nLowCount, size_t nHighCount, size_t nDstCount)
{
    for (size_t i = 0; i < nDstCount; i++)
    {
        const int16_t* pL = pLowBand;
        const int16_t* pH = pHighBand;
        int16_t* pX = pDstBand;
        
        int16_t H0 = *pH;
        pH += nHighStep;
        int16_t L0 = *pL;
        pL += nLowStep;
        int16_t X0 = clampi16((int32_t)L0 - H0);
        int16_t X2 = clampi16((int32_t)L0 - H0);
        int16_t X1, H1;
        
        for (size_t j = 0; j < (nHighCount - 1); j++)
        {
            H1 = *pH;
            pH += nHighStep;
            L0 = *pL;
            pL += nLowStep;
            X2 = clampi16((int32_t)L0 - ((H0 + H1) / 2));
            X1 = clampi16((int32_t)((X0 + X2) / 2) + (2 * H0));
            *pX = X0;
            pX += nDstStep;
            *pX = X1;
            pX += nDstStep;
            X0 = X2;
            H0 = H1;
        }
        
        if (nLowCount <= (nHighCount + 1))
        {
            if (nLowCount <= nHighCount)
            {
                *pX = X2;
                pX += nDstStep;
                *pX = clampi16((int32_t)X2 + (2 * H0));
            }
            else
            {
                L0 = *pL;
                X0 = clampi16((int32_t)L0 - H0);
                *pX = X2;
                pX += nDstStep;
                *pX = clampi16((int32_t)((X0 + X2) / 2) + (2 * H0));
                pX += nDstStep;
                *pX = X0;
            }
        }
        else
        {
            L0 = *pL;
            pL += nLowStep;
            X0 = clampi16((int32_t)L0 - (H0 / 2));
            *pX = X2;
            pX += nDstStep;
            *pX = clampi16((int32_t)((X0 + X2) / 2) + (2 * H0));
            pX += nDstStep;
            *pX = X0;
            pX += nDstStep;
            L0 = *pL;
            *pX = clampi16((int32_t)(X0 + L0) / 2);
        }
        
        pLowBand++;
        pHighBand++;
        pDstBand++;
    }
}

/* Note: idwt_temp is declared at top of file as thread-local */

/**
 * 2D inverse DWT decode for a single level (extrapolated tiles)
 * 
 * @param buffer  Input/output coefficient buffer (subbands are stored linearly)
 * @param temp    Temporary buffer for intermediate results
 * @param level   DWT decomposition level (3, 2, or 1)
 */
static void dwt_2d_decode_block(int16_t* buffer, int16_t* temp, size_t level)
{
    const int16_t* HL;
    const int16_t* LH;
    const int16_t* HH;
    int16_t* LL;
    int16_t* L;
    int16_t* H;
    int16_t* LLx;
    
    size_t nBandL = get_band_l_count(level);
    size_t nBandH = get_band_h_count(level);
    size_t nDstStepX = nBandL + nBandH;
    size_t nDstStepY = nBandL + nBandH;
    size_t offset = 0;
    
    /* Subband layout in buffer:
     * HL: offset 0, size nBandH * nBandL
     * LH: offset + nBandH*nBandL, size nBandL * nBandH
     * HH: offset + nBandH*nBandL + nBandL*nBandH, size nBandH * nBandH
     * LL: offset + all above, size nBandL * nBandL
     */
    HL = &buffer[offset];
    offset += (nBandH * nBandL);
    LH = &buffer[offset];
    offset += (nBandL * nBandH);
    HH = &buffer[offset];
    offset += (nBandH * nBandH);
    LL = &buffer[offset];
    
    /* Temp buffer layout:
     * L: temp[0], rows for low-pass output
     * H: temp[nBandL * nDstStepX], rows for high-pass output
     */
    offset = 0;
    L = &temp[offset];
    offset += (nBandL * nDstStepX);
    H = &temp[offset];
    
    /* Output will overwrite buffer starting at 0 */
    LLx = &buffer[0];
    
    /* Step 1: Horizontal inverse DWT */
    /* LL + HL -> L (low-frequency horizontal) */
    idwt_x(LL, nBandL, HL, nBandH, L, nDstStepX, nBandL, nBandH, nBandL);
    
    /* LH + HH -> H (high-frequency horizontal) */
    idwt_x(LH, nBandL, HH, nBandH, H, nDstStepX, nBandL, nBandH, nBandH);
    
    /* Step 2: Vertical inverse DWT */
    /* L + H -> output */
    idwt_y(L, nDstStepX, H, nDstStepX, LLx, nDstStepY, nBandL, nBandH, nBandL + nBandH);
}

/**
 * Full 2D inverse DWT for extrapolated tiles (3 levels)
 * 
 * Reconstructs a 64x64 tile from extrapolated wavelet coefficients.
 * 
 * Input coefficients must be in extrapolated subband layout:
 *   HL1@0, LH1@1023, HH1@2046, HL2@3007, LH2@3279, HH2@3551,
 *   HL3@3807, LH3@3879, HH3@3951, LL3@4015
 *
 * After decode, buffer[0..4095] contains the 64x64 spatial grid.
 */
void rfx_dwt_decode(int16_t* buffer, int size)
{
    (void)size; /* size is always 64, but kept for API compatibility */
    
    /* Allocate temp buffer on HEAP to avoid any stack sharing issues with pthreads.
     * Each DWT call gets its own buffer to avoid race conditions. */
    int16_t* idwt_temp = (int16_t*)calloc(65 * 65, sizeof(int16_t));
    if (!idwt_temp) {
        return;
    }
    
    /* Level 3: 9x9 LL3 + 8x9 HL3 + 9x8 LH3 + 8x8 HH3 -> 17x17 intermediate */
    /* Subbands start at buffer[3807] */
    dwt_2d_decode_block(&buffer[3807], idwt_temp, 3);
    
    /* Level 2: 17x17 LL2 + 16x17 HL2 + 17x16 LH2 + 16x16 HH2 -> 33x33 intermediate */
    /* Subbands start at buffer[3007] */
    dwt_2d_decode_block(&buffer[3007], idwt_temp, 2);
    
    /* Level 1: 33x33 LL1 + 31x33 HL1 + 33x31 LH1 + 31x31 HH1 -> 64x64 output */
    /* Subbands start at buffer[0] */
    dwt_2d_decode_block(&buffer[0], idwt_temp, 1);
    
    /* Free the heap-allocated temp buffer */
    free(idwt_temp);
}

/**
 * Apply differential decode to the LL3 subband
 * 
 * For extrapolated tiles, LL3 is at buffer[4015] with 81 coefficients (9x9).
 * For non-extrapolated tiles, LL3 is at buffer[4032] with 64 coefficients (8x8).
 * 
 * This must be called AFTER RLGR decode, BEFORE dequantization.
 */
void rfx_differential_decode(int16_t* buffer, size_t size)
{
    for (size_t i = 1; i < size; i++)
    {
        buffer[i] = (int16_t)(buffer[i] + buffer[i - 1]);
    }
}

/**
 * Left shift for int16_t matching FreeRDP's behavior.
 * Uses unsigned arithmetic to avoid undefined behavior on overflow.
 */
static inline int16_t lshift16(int16_t val, int sh)
{
    return (int16_t)(((uint32_t)val << sh) & 0xFFFF);
}

/**
 * Apply dequantization to extrapolated tile coefficients
 * 
 * IMPORTANT: Uses EXTRAPOLATED subband layout!
 * 
 *   HL1: buffer[0..1022]     (31x33 = 1023 coefficients)
 *   LH1: buffer[1023..2045]  (33x31 = 1023)
 *   HH1: buffer[2046..3006]  (31x31 = 961)
 *   HL2: buffer[3007..3278]  (16x17 = 272)
 *   LH2: buffer[3279..3550]  (17x16 = 272)
 *   HH2: buffer[3551..3806]  (16x16 = 256)
 *   HL3: buffer[3807..3878]  (8x9 = 72)
 *   LH3: buffer[3879..3950]  (9x8 = 72)
 *   HH3: buffer[3951..4014]  (8x8 = 64)
 *   LL3: buffer[4015..4095]  (9x9 = 81)
 */
void rfx_dequantize(int16_t* buffer, const RfxComponentCodecQuant* quant)
{
    int i;
    int shift;
    
    /* HL1: buffer[0..1022] (1023 coefficients) */
    shift = quant->HL1 - 1;
    if (shift > 0 && shift < 16) {
        for (i = 0; i < 1023; i++) {
            buffer[i] = lshift16(buffer[i], shift);
        }
    }
    
    /* LH1: buffer[1023..2045] (1023 coefficients) */
    shift = quant->LH1 - 1;
    if (shift > 0 && shift < 16) {
        for (i = 1023; i < 2046; i++) {
            buffer[i] = lshift16(buffer[i], shift);
        }
    }
    
    /* HH1: buffer[2046..3006] (961 coefficients) */
    shift = quant->HH1 - 1;
    if (shift > 0 && shift < 16) {
        for (i = 2046; i < 3007; i++) {
            buffer[i] = lshift16(buffer[i], shift);
        }
    }
    
    /* HL2: buffer[3007..3278] (272 coefficients) */
    shift = quant->HL2 - 1;
    if (shift > 0 && shift < 16) {
        for (i = 3007; i < 3279; i++) {
            buffer[i] = lshift16(buffer[i], shift);
        }
    }
    
    /* LH2: buffer[3279..3550] (272 coefficients) */
    shift = quant->LH2 - 1;
    if (shift > 0 && shift < 16) {
        for (i = 3279; i < 3551; i++) {
            buffer[i] = lshift16(buffer[i], shift);
        }
    }
    
    /* HH2: buffer[3551..3806] (256 coefficients) */
    shift = quant->HH2 - 1;
    if (shift > 0 && shift < 16) {
        for (i = 3551; i < 3807; i++) {
            buffer[i] = lshift16(buffer[i], shift);
        }
    }
    
    /* HL3: buffer[3807..3878] (72 coefficients) */
    shift = quant->HL3 - 1;
    if (shift > 0 && shift < 16) {
        for (i = 3807; i < 3879; i++) {
            buffer[i] = lshift16(buffer[i], shift);
        }
    }
    
    /* LH3: buffer[3879..3950] (72 coefficients) */
    shift = quant->LH3 - 1;
    if (shift > 0 && shift < 16) {
        for (i = 3879; i < 3951; i++) {
            buffer[i] = lshift16(buffer[i], shift);
        }
    }
    
    /* HH3: buffer[3951..4014] (64 coefficients) */
    shift = quant->HH3 - 1;
    if (shift > 0 && shift < 16) {
        for (i = 3951; i < 4015; i++) {
            buffer[i] = lshift16(buffer[i], shift);
        }
    }
    
    /* LL3: buffer[4015..4095] (81 coefficients) */
    shift = quant->LL3 - 1;
    if (shift > 0 && shift < 16) {
        for (i = 4015; i < 4096; i++) {
            buffer[i] = lshift16(buffer[i], shift);
        }
    }
}

/**
 * Apply progressive dequantization to extrapolated tile coefficients
 * 
 * For progressive tiles, the shift is (quant + progQuant - 1) for each subband.
 * This accounts for the progressive quantization quality settings.
 * 
 * IMPORTANT: Uses EXTRAPOLATED subband layout!
 */
void rfx_dequantize_progressive(int16_t* buffer, 
                                 const RfxComponentCodecQuant* quant,
                                 const RfxComponentCodecQuant* progQuant)
{
    int i;
    int shift;
    
    /* HL1: buffer[0..1022] (1023 coefficients) */
    shift = quant->HL1 + progQuant->HL1 - 1;
    if (shift > 0 && shift < 16) {
        for (i = 0; i < 1023; i++) {
            buffer[i] = lshift16(buffer[i], shift);
        }
    }
    
    /* LH1: buffer[1023..2045] (1023 coefficients) */
    shift = quant->LH1 + progQuant->LH1 - 1;
    if (shift > 0 && shift < 16) {
        for (i = 1023; i < 2046; i++) {
            buffer[i] = lshift16(buffer[i], shift);
        }
    }
    
    /* HH1: buffer[2046..3006] (961 coefficients) */
    shift = quant->HH1 + progQuant->HH1 - 1;
    if (shift > 0 && shift < 16) {
        for (i = 2046; i < 3007; i++) {
            buffer[i] = lshift16(buffer[i], shift);
        }
    }
    
    /* HL2: buffer[3007..3278] (272 coefficients) */
    shift = quant->HL2 + progQuant->HL2 - 1;
    if (shift > 0 && shift < 16) {
        for (i = 3007; i < 3279; i++) {
            buffer[i] = lshift16(buffer[i], shift);
        }
    }
    
    /* LH2: buffer[3279..3550] (272 coefficients) */
    shift = quant->LH2 + progQuant->LH2 - 1;
    if (shift > 0 && shift < 16) {
        for (i = 3279; i < 3551; i++) {
            buffer[i] = lshift16(buffer[i], shift);
        }
    }
    
    /* HH2: buffer[3551..3806] (256 coefficients) */
    shift = quant->HH2 + progQuant->HH2 - 1;
    if (shift > 0 && shift < 16) {
        for (i = 3551; i < 3807; i++) {
            buffer[i] = lshift16(buffer[i], shift);
        }
    }
    
    /* HL3: buffer[3807..3878] (72 coefficients) */
    shift = quant->HL3 + progQuant->HL3 - 1;
    if (shift > 0 && shift < 16) {
        for (i = 3807; i < 3879; i++) {
            buffer[i] = lshift16(buffer[i], shift);
        }
    }
    
    /* LH3: buffer[3879..3950] (72 coefficients) */
    shift = quant->LH3 + progQuant->LH3 - 1;
    if (shift > 0 && shift < 16) {
        for (i = 3879; i < 3951; i++) {
            buffer[i] = lshift16(buffer[i], shift);
        }
    }
    
    /* HH3: buffer[3951..4014] (64 coefficients) */
    shift = quant->HH3 + progQuant->HH3 - 1;
    if (shift > 0 && shift < 16) {
        for (i = 3951; i < 4015; i++) {
            buffer[i] = lshift16(buffer[i], shift);
        }
    }
    
    /* LL3: buffer[4015..4095] (81 coefficients) */
    shift = quant->LL3 + progQuant->LL3 - 1;
    
    if (shift > 0 && shift < 16) {
        for (i = 4015; i < 4096; i++) {
            buffer[i] = lshift16(buffer[i], shift);
        }
    }
}
