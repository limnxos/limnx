#include "libc.h"

/*
 * GGML dequantization — convert quantized blocks to F32.
 *
 * Supports: F32, F16, Q4_0, Q4_1, Q5_0, Q5_1, Q8_0,
 *           Q2_K, Q3_K, Q4_K, Q5_K, Q6_K
 */

/* GGML type IDs */
#define GGML_TYPE_F32   0
#define GGML_TYPE_F16   1
#define GGML_TYPE_Q4_0  2
#define GGML_TYPE_Q4_1  3
#define GGML_TYPE_Q5_0  6
#define GGML_TYPE_Q5_1  7
#define GGML_TYPE_Q8_0  8
#define GGML_TYPE_Q2_K  10
#define GGML_TYPE_Q3_K  11
#define GGML_TYPE_Q4_K  12
#define GGML_TYPE_Q5_K  13
#define GGML_TYPE_Q6_K  14

/* --- F16 → F32 conversion (bit manipulation) --- */

static float f16_to_f32(uint16_t h) {
    uint32_t sign = ((uint32_t)(h & 0x8000)) << 16;
    uint32_t exp  = (h >> 10) & 0x1F;
    uint32_t mant = h & 0x3FF;

    if (exp == 0) {
        if (mant == 0) {
            /* Signed zero */
            union { uint32_t u; float f; } r;
            r.u = sign;
            return r.f;
        }
        /* Subnormal: normalize */
        while (!(mant & 0x400)) {
            mant <<= 1;
            exp--;
        }
        exp++;
        mant &= 0x3FF;
        exp += 112; /* bias adjust: 127 - 15 */
    } else if (exp == 31) {
        exp = 255; /* inf/nan */
    } else {
        exp += 112; /* bias adjust: 127 - 15 */
    }

    uint32_t bits = sign | (exp << 23) | (mant << 13);
    union { uint32_t u; float f; } r;
    r.u = bits;
    return r.f;
}

/* --- Q8_0: 34 bytes → 32 floats --- */
/* Layout: f16 scale (2B) + int8_t quants[32] (32B) */

static void dequant_q8_0_block(const void *src, float *dst) {
    const uint8_t *p = (const uint8_t *)src;
    uint16_t scale_h;
    memcpy(&scale_h, p, 2);
    float scale = f16_to_f32(scale_h);
    const int8_t *quants = (const int8_t *)(p + 2);
    for (int i = 0; i < 32; i++)
        dst[i] = (float)quants[i] * scale;
}

/* --- Q4_0: 18 bytes → 32 floats --- */
/* Layout: f16 scale (2B) + uint8_t quants[16] (16B, 2 nibbles per byte) */

static void dequant_q4_0_block(const void *src, float *dst) {
    const uint8_t *p = (const uint8_t *)src;
    uint16_t scale_h;
    memcpy(&scale_h, p, 2);
    float scale = f16_to_f32(scale_h);
    const uint8_t *quants = p + 2;

    for (int i = 0; i < 16; i++) {
        int lo = (quants[i] & 0x0F) - 8;
        int hi = (quants[i] >> 4) - 8;
        dst[i * 2]     = (float)lo * scale;
        dst[i * 2 + 1] = (float)hi * scale;
    }
}

/* --- Q4_1: 20 bytes → 32 floats --- */
/* Layout: f16 scale (2B) + f16 min (2B) + uint8_t quants[16] (16B) */

static void dequant_q4_1_block(const void *src, float *dst) {
    const uint8_t *p = (const uint8_t *)src;
    uint16_t scale_h, min_h;
    memcpy(&scale_h, p, 2);
    memcpy(&min_h, p + 2, 2);
    float scale = f16_to_f32(scale_h);
    float min_val = f16_to_f32(min_h);
    const uint8_t *quants = p + 4;

    for (int i = 0; i < 16; i++) {
        int lo = quants[i] & 0x0F;
        int hi = quants[i] >> 4;
        dst[i * 2]     = (float)lo * scale + min_val;
        dst[i * 2 + 1] = (float)hi * scale + min_val;
    }
}

/* --- Q5_0: 22 bytes → 32 floats --- */
/* Layout: f16 scale (2B) + uint8_t qh[4] (4B, high bits) + uint8_t quants[16] (16B) */

static void dequant_q5_0_block(const void *src, float *dst) {
    const uint8_t *p = (const uint8_t *)src;
    uint16_t scale_h;
    memcpy(&scale_h, p, 2);
    float scale = f16_to_f32(scale_h);
    const uint8_t *qh = p + 2;      /* 4 bytes = 32 bits for high bit */
    const uint8_t *qs = p + 6;      /* 16 bytes for low 4 bits */

    uint32_t hmask;
    memcpy(&hmask, qh, 4);

    for (int i = 0; i < 16; i++) {
        int lo = (qs[i] & 0x0F);
        int hi = (qs[i] >> 4);
        /* Add 5th bit from hmask */
        lo |= ((hmask >> (i * 2)) & 1) << 4;
        hi |= ((hmask >> (i * 2 + 1)) & 1) << 4;
        dst[i * 2]     = (float)(lo - 16) * scale;
        dst[i * 2 + 1] = (float)(hi - 16) * scale;
    }
}

/* --- Q5_1: 24 bytes → 32 floats --- */
/* Layout: f16 scale (2B) + f16 min (2B) + uint8_t qh[4] (4B) + uint8_t quants[16] (16B) */

static void dequant_q5_1_block(const void *src, float *dst) {
    const uint8_t *p = (const uint8_t *)src;
    uint16_t scale_h, min_h;
    memcpy(&scale_h, p, 2);
    memcpy(&min_h, p + 2, 2);
    float scale = f16_to_f32(scale_h);
    float min_val = f16_to_f32(min_h);
    const uint8_t *qh = p + 4;
    const uint8_t *qs = p + 8;

    uint32_t hmask;
    memcpy(&hmask, qh, 4);

    for (int i = 0; i < 16; i++) {
        int lo = (qs[i] & 0x0F);
        int hi = (qs[i] >> 4);
        lo |= ((hmask >> (i * 2)) & 1) << 4;
        hi |= ((hmask >> (i * 2 + 1)) & 1) << 4;
        dst[i * 2]     = (float)lo * scale + min_val;
        dst[i * 2 + 1] = (float)hi * scale + min_val;
    }
}

/* --- Q2_K: 84 bytes → 256 floats --- */
/* Layout: uint8_t scales[16] + uint8_t quants[64] + f16 d + f16 dmin */

static void dequant_q2_k_block(const void *src, float *dst) {
    const uint8_t *p = (const uint8_t *)src;
    const uint8_t *scales = p;          /* 16 bytes: 4-bit scale + 4-bit min per sub-block */
    const uint8_t *quants = p + 16;     /* 64 bytes: 2 bits per value, 4 values per byte */
    uint16_t d_h, dmin_h;
    memcpy(&d_h, p + 80, 2);
    memcpy(&dmin_h, p + 82, 2);
    float d = f16_to_f32(d_h);
    float dmin = f16_to_f32(dmin_h);

    /* 16 sub-blocks of 16 values each */
    for (int sb = 0; sb < 16; sb++) {
        float sc = d * (float)(scales[sb] & 0x0F);
        float mn = dmin * (float)(scales[sb] >> 4);
        /* Each sub-block: 4 bytes = 16 values (2 bits each) */
        const uint8_t *qb = quants + sb * 4;
        for (int j = 0; j < 4; j++) {
            uint8_t q = qb[j];
            dst[sb * 16 + j * 4 + 0] = sc * (float)((q >> 0) & 3) - mn;
            dst[sb * 16 + j * 4 + 1] = sc * (float)((q >> 2) & 3) - mn;
            dst[sb * 16 + j * 4 + 2] = sc * (float)((q >> 4) & 3) - mn;
            dst[sb * 16 + j * 4 + 3] = sc * (float)((q >> 6) & 3) - mn;
        }
    }
}

/* --- Q3_K: 110 bytes → 256 floats --- */
/* Layout: uint8_t hmasks[32] + uint8_t quants[64] + uint8_t scales[12] + f16 d */

static void dequant_q3_k_block(const void *src, float *dst) {
    const uint8_t *p = (const uint8_t *)src;
    const uint8_t *hmasks = p;          /* 32 bytes: high bit per value */
    const uint8_t *quants = p + 32;     /* 64 bytes: low 2 bits, 4 per byte */
    const uint8_t *sc_raw = p + 96;     /* 12 bytes: packed scales */
    uint16_t d_h;
    memcpy(&d_h, p + 108, 2);
    float d = f16_to_f32(d_h);

    /* Decode 16 6-bit scales from 12 bytes */
    uint32_t sc32[16];
    {
        /* bytes 0..3 → low 4 bits of scales 0..7 */
        for (int i = 0; i < 4; i++) {
            sc32[2*i]   = sc_raw[i] & 0x0F;
            sc32[2*i+1] = sc_raw[i] >> 4;
        }
        /* bytes 4..7 → low 4 bits of scales 8..15 */
        for (int i = 0; i < 4; i++) {
            sc32[8+2*i]   = sc_raw[4+i] & 0x0F;
            sc32[8+2*i+1] = sc_raw[4+i] >> 4;
        }
        /* bytes 8..11 → high 2 bits of scales 0..15 */
        for (int i = 0; i < 4; i++) {
            sc32[4*i+0] |= ((sc_raw[8+i] >> 0) & 3) << 4;
            sc32[4*i+1] |= ((sc_raw[8+i] >> 2) & 3) << 4;
            sc32[4*i+2] |= ((sc_raw[8+i] >> 4) & 3) << 4;
            sc32[4*i+3] |= ((sc_raw[8+i] >> 6) & 3) << 4;
        }
    }

    /* 16 sub-blocks of 16 values each */
    for (int sb = 0; sb < 16; sb++) {
        float sc = d * ((int32_t)sc32[sb] - 32);
        for (int j = 0; j < 16; j++) {
            int idx = sb * 16 + j;
            /* Low 2 bits from quants */
            int qbyte_idx = idx / 4;
            int qbit_shift = (idx % 4) * 2;
            int q_lo = (quants[qbyte_idx] >> qbit_shift) & 3;
            /* High bit from hmasks */
            int hbyte_idx = idx / 8;
            int hbit = (hmasks[hbyte_idx] >> (idx % 8)) & 1;
            int q = q_lo | (hbit << 2);
            dst[idx] = sc * ((float)q - 4.0f);
        }
    }
}

/* --- Q4_K: 144 bytes → 256 floats --- */
/* Layout: f16 d (2B) + f16 dmin (2B) + uint8_t scales[12] (12B) + uint8_t quants[128] (128B) */

static void dequant_q4_k_block(const void *src, float *dst) {
    const uint8_t *p = (const uint8_t *)src;
    uint16_t d_h, dmin_h;
    memcpy(&d_h, p, 2);
    memcpy(&dmin_h, p + 2, 2);
    float d = f16_to_f32(d_h);
    float dmin = f16_to_f32(dmin_h);
    const uint8_t *sc_raw = p + 4;     /* 12 bytes */
    const uint8_t *quants = p + 16;    /* 128 bytes */

    /* Decode 8 6-bit scale/min pairs from 12 bytes.
     * 8 sub-blocks of 32 values each.
     * Encoding: bytes 0..3 have low 6 bits of scales[0..3] and mins[0..3]
     *           bytes 4..7 have low 6 bits of scales[4..7] and mins[4..7]
     *           bytes 8..11 have high 2 bits */
    uint8_t sc[8], mn[8];
    /* Low 4 bits */
    sc[0] = sc_raw[0] & 0x3F;  mn[0] = sc_raw[0] >> 6 | ((sc_raw[1] & 0x0F) << 2);
    sc[1] = (sc_raw[1] >> 4) | ((sc_raw[2] & 0x03) << 4);
    mn[1] = (sc_raw[2] >> 2);
    sc[2] = sc_raw[3] & 0x3F;  mn[2] = sc_raw[3] >> 6 | ((sc_raw[4] & 0x0F) << 2);
    sc[3] = (sc_raw[4] >> 4) | ((sc_raw[5] & 0x03) << 4);
    mn[3] = (sc_raw[5] >> 2);
    sc[4] = sc_raw[6] & 0x3F;  mn[4] = sc_raw[6] >> 6 | ((sc_raw[7] & 0x0F) << 2);
    sc[5] = (sc_raw[7] >> 4) | ((sc_raw[8] & 0x03) << 4);
    mn[5] = (sc_raw[8] >> 2);
    sc[6] = sc_raw[9] & 0x3F;  mn[6] = sc_raw[9] >> 6 | ((sc_raw[10] & 0x0F) << 2);
    sc[7] = (sc_raw[10] >> 4) | ((sc_raw[11] & 0x03) << 4);
    mn[7] = (sc_raw[11] >> 2);

    /* 8 sub-blocks of 32 values, 4 bits per value */
    for (int sb = 0; sb < 8; sb++) {
        float scale = d * (float)sc[sb];
        float min_val = dmin * (float)mn[sb];
        const uint8_t *qb = quants + sb * 16;
        for (int j = 0; j < 16; j++) {
            int lo = qb[j] & 0x0F;
            int hi = qb[j] >> 4;
            dst[sb * 32 + j * 2]     = scale * (float)lo - min_val;
            dst[sb * 32 + j * 2 + 1] = scale * (float)hi - min_val;
        }
    }
}

/* --- Q5_K: 176 bytes → 256 floats --- */
/* Layout: f16 d (2B) + f16 dmin (2B) + uint8_t scales[12] (12B) + uint8_t qh[32] (32B) + uint8_t ql[128] (128B) */

static void dequant_q5_k_block(const void *src, float *dst) {
    const uint8_t *p = (const uint8_t *)src;
    uint16_t d_h, dmin_h;
    memcpy(&d_h, p, 2);
    memcpy(&dmin_h, p + 2, 2);
    float d = f16_to_f32(d_h);
    float dmin = f16_to_f32(dmin_h);
    const uint8_t *sc_raw = p + 4;     /* 12 bytes */
    const uint8_t *qh = p + 16;        /* 32 bytes: high bits */
    const uint8_t *ql = p + 48;        /* 128 bytes: low 4 bits */

    /* Same scale encoding as Q4_K */
    uint8_t sc[8], mn[8];
    sc[0] = sc_raw[0] & 0x3F;  mn[0] = sc_raw[0] >> 6 | ((sc_raw[1] & 0x0F) << 2);
    sc[1] = (sc_raw[1] >> 4) | ((sc_raw[2] & 0x03) << 4);
    mn[1] = (sc_raw[2] >> 2);
    sc[2] = sc_raw[3] & 0x3F;  mn[2] = sc_raw[3] >> 6 | ((sc_raw[4] & 0x0F) << 2);
    sc[3] = (sc_raw[4] >> 4) | ((sc_raw[5] & 0x03) << 4);
    mn[3] = (sc_raw[5] >> 2);
    sc[4] = sc_raw[6] & 0x3F;  mn[4] = sc_raw[6] >> 6 | ((sc_raw[7] & 0x0F) << 2);
    sc[5] = (sc_raw[7] >> 4) | ((sc_raw[8] & 0x03) << 4);
    mn[5] = (sc_raw[8] >> 2);
    sc[6] = sc_raw[9] & 0x3F;  mn[6] = sc_raw[9] >> 6 | ((sc_raw[10] & 0x0F) << 2);
    sc[7] = (sc_raw[10] >> 4) | ((sc_raw[11] & 0x03) << 4);
    mn[7] = (sc_raw[11] >> 2);

    /* 8 sub-blocks of 32 values */
    for (int sb = 0; sb < 8; sb++) {
        float scale = d * (float)sc[sb];
        float min_val = dmin * (float)mn[sb];
        const uint8_t *qb = ql + sb * 16;
        const uint8_t *hb = qh + sb * 4;  /* 4 bytes = 32 high bits */
        for (int j = 0; j < 16; j++) {
            int lo = qb[j] & 0x0F;
            int hi = qb[j] >> 4;
            /* High bit from qh */
            lo |= ((hb[j / 4] >> ((j % 4) * 2)) & 1) << 4;
            hi |= ((hb[j / 4] >> ((j % 4) * 2 + 1)) & 1) << 4;
            dst[sb * 32 + j * 2]     = scale * (float)lo - min_val;
            dst[sb * 32 + j * 2 + 1] = scale * (float)hi - min_val;
        }
    }
}

/* --- Q6_K: 210 bytes → 256 floats --- */
/* Layout: uint8_t ql[128] + uint8_t qh[64] + int8_t scales[16] + f16 d */

static void dequant_q6_k_block(const void *src, float *dst) {
    const uint8_t *p = (const uint8_t *)src;
    const uint8_t *ql = p;              /* 128 bytes: low 4 bits */
    const uint8_t *qh = p + 128;       /* 64 bytes: high 2 bits */
    const int8_t  *scales = (const int8_t *)(p + 192);  /* 16 bytes */
    uint16_t d_h;
    memcpy(&d_h, p + 208, 2);
    float d = f16_to_f32(d_h);

    /* 16 sub-blocks of 16 values each */
    for (int sb = 0; sb < 16; sb++) {
        float sc = d * (float)scales[sb];
        for (int j = 0; j < 16; j++) {
            int idx = sb * 16 + j;
            /* Low 4 bits from ql (2 nibbles per byte) */
            int ql_byte = idx / 2;
            int q_lo;
            if (idx % 2 == 0)
                q_lo = ql[ql_byte] & 0x0F;
            else
                q_lo = ql[ql_byte] >> 4;

            /* High 2 bits from qh (4 values per byte) */
            int qh_byte = idx / 4;
            int qh_shift = (idx % 4) * 2;
            int q_hi = (qh[qh_byte] >> qh_shift) & 3;

            int q = q_lo | (q_hi << 4);
            dst[idx] = sc * ((float)q - 32.0f);
        }
    }
}

/* --- Dispatch --- */

int dequant(const void *src, float *dst, uint64_t n, uint32_t type) {
    if (type == GGML_TYPE_F32) {
        memcpy(dst, src, n * 4);
        return 0;
    }

    if (type == GGML_TYPE_F16) {
        const uint16_t *s = (const uint16_t *)src;
        for (uint64_t i = 0; i < n; i++)
            dst[i] = f16_to_f32(s[i]);
        return 0;
    }

    uint32_t block_size, block_bytes;
    if (dequant_block_info(type, &block_size, &block_bytes) != 0)
        return -1;

    if (n % block_size != 0)
        return -1;

    uint64_t n_blocks = n / block_size;
    const uint8_t *p = (const uint8_t *)src;

    for (uint64_t b = 0; b < n_blocks; b++) {
        float *out = dst + b * block_size;
        const void *blk = p + b * block_bytes;

        switch (type) {
        case GGML_TYPE_Q4_0: dequant_q4_0_block(blk, out); break;
        case GGML_TYPE_Q4_1: dequant_q4_1_block(blk, out); break;
        case GGML_TYPE_Q5_0: dequant_q5_0_block(blk, out); break;
        case GGML_TYPE_Q5_1: dequant_q5_1_block(blk, out); break;
        case GGML_TYPE_Q8_0: dequant_q8_0_block(blk, out); break;
        case GGML_TYPE_Q2_K: dequant_q2_k_block(blk, out); break;
        case GGML_TYPE_Q3_K: dequant_q3_k_block(blk, out); break;
        case GGML_TYPE_Q4_K: dequant_q4_k_block(blk, out); break;
        case GGML_TYPE_Q5_K: dequant_q5_k_block(blk, out); break;
        case GGML_TYPE_Q6_K: dequant_q6_k_block(blk, out); break;
        default: return -1;
        }
    }

    return 0;
}

int dequant_block_info(uint32_t type, uint32_t *block_size, uint32_t *block_bytes) {
    switch (type) {
    case GGML_TYPE_F32:  *block_size = 1;   *block_bytes = 4;   return 0;
    case GGML_TYPE_F16:  *block_size = 1;   *block_bytes = 2;   return 0;
    case GGML_TYPE_Q4_0: *block_size = 32;  *block_bytes = 18;  return 0;
    case GGML_TYPE_Q4_1: *block_size = 32;  *block_bytes = 20;  return 0;
    case GGML_TYPE_Q5_0: *block_size = 32;  *block_bytes = 22;  return 0;
    case GGML_TYPE_Q5_1: *block_size = 32;  *block_bytes = 24;  return 0;
    case GGML_TYPE_Q8_0: *block_size = 32;  *block_bytes = 34;  return 0;
    case GGML_TYPE_Q2_K: *block_size = 256; *block_bytes = 84;  return 0;
    case GGML_TYPE_Q3_K: *block_size = 256; *block_bytes = 110; return 0;
    case GGML_TYPE_Q4_K: *block_size = 256; *block_bytes = 144; return 0;
    case GGML_TYPE_Q5_K: *block_size = 256; *block_bytes = 176; return 0;
    case GGML_TYPE_Q6_K: *block_size = 256; *block_bytes = 210; return 0;
    default: return -1;
    }
}
