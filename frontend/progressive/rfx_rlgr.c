/**
 * RLGR (Run-Length Golomb-Rice) Entropy Decoder
 * Based on FreeRDP's rfx_rlgr.c (Apache 2.0 License)
 * 
 * Decodes RLGR1 encoded wavelet coefficients used by RFX Progressive codec.
 */

#include "rfx_types.h"

/* Bit reader state */
typedef struct {
    const uint8_t* data;
    size_t size;
    size_t offset;       /* byte offset */
    int bits_left;       /* bits remaining in current byte */
    uint32_t accumulator;
} BitReader;

static void bit_reader_init(BitReader* br, const uint8_t* data, size_t size) {
    br->data = data;
    br->size = size;
    br->offset = 0;
    br->bits_left = 0;
    br->accumulator = 0;
}

static inline uint32_t bit_reader_peek(BitReader* br, int nbits) {
    while (br->bits_left < nbits && br->offset < br->size) {
        br->accumulator = (br->accumulator << 8) | br->data[br->offset++];
        br->bits_left += 8;
    }
    if (br->bits_left < nbits) {
        return 0;
    }
    return (br->accumulator >> (br->bits_left - nbits)) & ((1u << nbits) - 1);
}

static inline void bit_reader_skip(BitReader* br, int nbits) {
    br->bits_left -= nbits;
    if (br->bits_left < 0) {
        br->bits_left = 0;
    }
}

static inline uint32_t bit_reader_read(BitReader* br, int nbits) {
    uint32_t val = bit_reader_peek(br, nbits);
    bit_reader_skip(br, nbits);
    return val;
}

static inline bool bit_reader_eof(BitReader* br) {
    return br->offset >= br->size && br->bits_left == 0;
}

/* RLGR adaptive parameters */
#define RLGR_K_MIN 0
#define RLGR_K_MAX 15

/* Get the number of leading zeros */
static inline int count_leading_zeros(uint32_t val) {
    if (val == 0) return 32;
    int n = 0;
    if ((val & 0xFFFF0000) == 0) { n += 16; val <<= 16; }
    if ((val & 0xFF000000) == 0) { n += 8; val <<= 8; }
    if ((val & 0xF0000000) == 0) { n += 4; val <<= 4; }
    if ((val & 0xC0000000) == 0) { n += 2; val <<= 2; }
    if ((val & 0x80000000) == 0) { n += 1; }
    return n;
}

/* Update K parameter based on decoded value */
static inline void update_k(int* k, int val, int mode) {
    if (mode == 1) {
        /* RLGR1: For run length encoding */
        if (val == 0) {
            (*k)++;
            if (*k > RLGR_K_MAX) *k = RLGR_K_MAX;
        } else if (val > 0) {
            (*k) -= 2;
            if (*k < RLGR_K_MIN) *k = RLGR_K_MIN;
        }
    } else {
        /* RLGR3: For coefficient values */
        if (val == 0) {
            (*k)++;
            if (*k > RLGR_K_MAX) *k = RLGR_K_MAX;
        } else {
            int logval = 31 - count_leading_zeros((uint32_t)val + 1);
            (*k) -= logval;
            if (*k < RLGR_K_MIN) *k = RLGR_K_MIN;
        }
    }
}

/**
 * Decode RLGR1 encoded data
 * 
 * @param input     Input RLGR1 bitstream
 * @param inputSize Input size in bytes
 * @param output    Output coefficient buffer
 * @param outputSize Maximum output count
 * @return Number of coefficients decoded, or -1 on error
 */
int rfx_rlgr_decode(const uint8_t* input, size_t inputSize,
                    int16_t* output, size_t outputSize) {
    BitReader br;
    bit_reader_init(&br, input, inputSize);
    
    int kR = 1;   /* Run length K */
    int kGR = 1;  /* Golomb-Rice K for coefficients */
    
    size_t outIdx = 0;
    
    while (!bit_reader_eof(&br) && outIdx < outputSize) {
        /* Read unary code for quotient */
        int q = 0;
        while (bit_reader_peek(&br, 1) == 0 && !bit_reader_eof(&br)) {
            q++;
            bit_reader_skip(&br, 1);
            if (q > 1000) break; /* Sanity check */
        }
        
        if (bit_reader_eof(&br)) break;
        bit_reader_skip(&br, 1); /* Skip the terminating 1 */
        
        if (q == 0) {
            /* Zero run */
            uint32_t runLen = 1;
            if (kR > 0) {
                runLen += bit_reader_read(&br, kR);
            }
            
            update_k(&kR, 0, 1);
            
            for (uint32_t i = 0; i < runLen && outIdx < outputSize; i++) {
                output[outIdx++] = 0;
            }
        } else {
            /* Non-zero coefficient */
            uint32_t remainder = 0;
            if (kGR > 0) {
                remainder = bit_reader_read(&br, kGR);
            }
            
            uint32_t mag = ((q - 1) << kGR) + remainder;
            
            /* Sign bit (1 = negative) */
            int sign = bit_reader_read(&br, 1);
            
            int16_t val = (int16_t)(mag + 1);
            if (sign) val = -val;
            
            output[outIdx++] = val;
            
            update_k(&kGR, (int)mag, 0);
            update_k(&kR, 1, 1); /* Reset run K */
        }
    }
    
    /* Fill remaining with zeros */
    while (outIdx < outputSize) {
        output[outIdx++] = 0;
    }
    
    return (int)outIdx;
}

/**
 * Decode SRL (Subband Residual Layer) data for progressive upgrade
 * SRL uses a different encoding than initial RLGR
 */
int rfx_srl_decode(const uint8_t* input, size_t inputSize,
                   int16_t* current, int8_t* sign,
                   size_t coeffCount, int bitPos) {
    if (inputSize == 0 || coeffCount == 0) {
        return 0;
    }
    
    BitReader br;
    bit_reader_init(&br, input, inputSize);
    
    for (size_t i = 0; i < coeffCount && !bit_reader_eof(&br); i++) {
        if (current[i] == 0) {
            /* Check if this coefficient becomes non-zero */
            if (bit_reader_read(&br, 1)) {
                /* Read sign if this is first non-zero */
                if (sign[i] == 0) {
                    sign[i] = bit_reader_read(&br, 1) ? -1 : 1;
                }
                current[i] = (int16_t)(sign[i] * (1 << bitPos));
            }
        } else {
            /* Refine existing non-zero coefficient */
            if (bit_reader_read(&br, 1)) {
                if (current[i] > 0) {
                    current[i] += (1 << bitPos);
                } else {
                    current[i] -= (1 << bitPos);
                }
            }
        }
    }
    
    return 0;
}
