#include "libc.h"

/*
 * GGML dequantization — convert quantized blocks to F32.
 *
 * Supports: F32, F16, Q4_0, Q4_1, Q5_0, Q5_1, Q8_0,
 *           Q2_K, Q3_K, Q4_K, Q5_K, Q6_K
 *
 * All code is portable C — no arch-specific intrinsics.
 * Inner loops are structured for compiler auto-vectorization
 * (SSE2 on x86_64, NEON on ARM64).
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
            union { uint32_t u; float f; } r;
            r.u = sign;
            return r.f;
        }
        while (!(mant & 0x400)) {
            mant <<= 1;
            exp--;
        }
        exp++;
        mant &= 0x3FF;
        exp += 112;
    } else if (exp == 31) {
        exp = 255;
    } else {
        exp += 112;
    }

    uint32_t bits = sign | (exp << 23) | (mant << 13);
    union { uint32_t u; float f; } r;
    r.u = bits;
    return r.f;
}

/* Read little-endian uint16 (safe for unaligned access) */
static inline uint16_t read_u16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

/* --- Q8_0: 34 bytes → 32 floats --- */

static void dequant_q8_0_block(const uint8_t *p, float *dst) {
    float scale = f16_to_f32(read_u16(p));
    const int8_t *quants = (const int8_t *)(p + 2);

    /* Process 4 elements per iteration for auto-vectorization */
    for (int i = 0; i < 32; i += 4) {
        dst[i]     = (float)quants[i]     * scale;
        dst[i + 1] = (float)quants[i + 1] * scale;
        dst[i + 2] = (float)quants[i + 2] * scale;
        dst[i + 3] = (float)quants[i + 3] * scale;
    }
}

/* --- Q4_0: 18 bytes → 32 floats --- */

static void dequant_q4_0_block(const uint8_t *p, float *dst) {
    float scale = f16_to_f32(read_u16(p));
    const uint8_t *quants = p + 2;

    /* Process 4 bytes (8 floats) per iteration for auto-vectorization */
    for (int i = 0; i < 16; i += 4) {
        dst[i * 2]     = (float)((int)(quants[i]     & 0x0F) - 8) * scale;
        dst[i * 2 + 1] = (float)((int)(quants[i]     >> 4)   - 8) * scale;
        dst[i * 2 + 2] = (float)((int)(quants[i + 1] & 0x0F) - 8) * scale;
        dst[i * 2 + 3] = (float)((int)(quants[i + 1] >> 4)   - 8) * scale;
        dst[i * 2 + 4] = (float)((int)(quants[i + 2] & 0x0F) - 8) * scale;
        dst[i * 2 + 5] = (float)((int)(quants[i + 2] >> 4)   - 8) * scale;
        dst[i * 2 + 6] = (float)((int)(quants[i + 3] & 0x0F) - 8) * scale;
        dst[i * 2 + 7] = (float)((int)(quants[i + 3] >> 4)   - 8) * scale;
    }
}

/* --- Q4_1: 20 bytes → 32 floats --- */

static void dequant_q4_1_block(const uint8_t *p, float *dst) {
    float scale = f16_to_f32(read_u16(p));
    float min_val = f16_to_f32(read_u16(p + 2));
    const uint8_t *quants = p + 4;

    for (int i = 0; i < 16; i += 4) {
        dst[i * 2]     = (float)(quants[i]     & 0x0F) * scale + min_val;
        dst[i * 2 + 1] = (float)(quants[i]     >> 4)   * scale + min_val;
        dst[i * 2 + 2] = (float)(quants[i + 1] & 0x0F) * scale + min_val;
        dst[i * 2 + 3] = (float)(quants[i + 1] >> 4)   * scale + min_val;
        dst[i * 2 + 4] = (float)(quants[i + 2] & 0x0F) * scale + min_val;
        dst[i * 2 + 5] = (float)(quants[i + 2] >> 4)   * scale + min_val;
        dst[i * 2 + 6] = (float)(quants[i + 3] & 0x0F) * scale + min_val;
        dst[i * 2 + 7] = (float)(quants[i + 3] >> 4)   * scale + min_val;
    }
}

/* --- Q5_0: 22 bytes → 32 floats --- */

static void dequant_q5_0_block(const uint8_t *p, float *dst) {
    float scale = f16_to_f32(read_u16(p));
    const uint8_t *qh = p + 2;
    const uint8_t *qs = p + 6;

    uint32_t hmask;
    memcpy(&hmask, qh, 4);

    for (int i = 0; i < 16; i++) {
        int lo = (qs[i] & 0x0F) | (((hmask >> (i * 2))     & 1) << 4);
        int hi = (qs[i] >> 4)   | (((hmask >> (i * 2 + 1)) & 1) << 4);
        dst[i * 2]     = (float)(lo - 16) * scale;
        dst[i * 2 + 1] = (float)(hi - 16) * scale;
    }
}

/* --- Q5_1: 24 bytes → 32 floats --- */

static void dequant_q5_1_block(const uint8_t *p, float *dst) {
    float scale = f16_to_f32(read_u16(p));
    float min_val = f16_to_f32(read_u16(p + 2));
    const uint8_t *qh = p + 4;
    const uint8_t *qs = p + 8;

    uint32_t hmask;
    memcpy(&hmask, qh, 4);

    for (int i = 0; i < 16; i++) {
        int lo = (qs[i] & 0x0F) | (((hmask >> (i * 2))     & 1) << 4);
        int hi = (qs[i] >> 4)   | (((hmask >> (i * 2 + 1)) & 1) << 4);
        dst[i * 2]     = (float)lo * scale + min_val;
        dst[i * 2 + 1] = (float)hi * scale + min_val;
    }
}

/* --- Q2_K: 84 bytes → 256 floats --- */

static void dequant_q2_k_block(const uint8_t *p, float *dst) {
    const uint8_t *scales = p;
    const uint8_t *quants = p + 16;
    float d = f16_to_f32(read_u16(p + 80));
    float dmin = f16_to_f32(read_u16(p + 82));

    for (int sb = 0; sb < 16; sb++) {
        float sc = d * (float)(scales[sb] & 0x0F);
        float mn = dmin * (float)(scales[sb] >> 4);
        const uint8_t *qb = quants + sb * 4;
        for (int j = 0; j < 4; j++) {
            uint8_t q = qb[j];
            dst[sb * 16 + j * 4]     = sc * (float)((q >> 0) & 3) - mn;
            dst[sb * 16 + j * 4 + 1] = sc * (float)((q >> 2) & 3) - mn;
            dst[sb * 16 + j * 4 + 2] = sc * (float)((q >> 4) & 3) - mn;
            dst[sb * 16 + j * 4 + 3] = sc * (float)((q >> 6) & 3) - mn;
        }
    }
}

/* --- Q3_K: 110 bytes → 256 floats --- */

static void dequant_q3_k_block(const uint8_t *p, float *dst) {
    const uint8_t *hmasks = p;
    const uint8_t *quants = p + 32;
    const uint8_t *sc_raw = p + 96;
    float d = f16_to_f32(read_u16(p + 108));

    /* Decode 16 6-bit scales from 12 bytes */
    int32_t sc32[16];
    for (int i = 0; i < 4; i++) {
        sc32[2*i]   = sc_raw[i] & 0x0F;
        sc32[2*i+1] = sc_raw[i] >> 4;
    }
    for (int i = 0; i < 4; i++) {
        sc32[8+2*i]   = sc_raw[4+i] & 0x0F;
        sc32[8+2*i+1] = sc_raw[4+i] >> 4;
    }
    for (int i = 0; i < 4; i++) {
        sc32[4*i]   |= ((sc_raw[8+i] >> 0) & 3) << 4;
        sc32[4*i+1] |= ((sc_raw[8+i] >> 2) & 3) << 4;
        sc32[4*i+2] |= ((sc_raw[8+i] >> 4) & 3) << 4;
        sc32[4*i+3] |= ((sc_raw[8+i] >> 6) & 3) << 4;
    }

    for (int sb = 0; sb < 16; sb++) {
        float sc = d * (float)(sc32[sb] - 32);
        for (int j = 0; j < 16; j++) {
            int idx = sb * 16 + j;
            int q_lo = (quants[idx / 4] >> ((idx % 4) * 2)) & 3;
            int hbit = (hmasks[idx / 8] >> (idx % 8)) & 1;
            int q = q_lo | (hbit << 2);
            dst[idx] = sc * ((float)q - 4.0f);
        }
    }
}

/* --- K-quant scale decode (shared by Q4_K and Q5_K) ---
 * Decodes 8 scale/min pairs from 12 bytes of packed data. */

static void decode_k_scales(const uint8_t *sc_raw, uint8_t *sc, uint8_t *mn) {
    sc[0] = sc_raw[0] & 0x3F;
    mn[0] = sc_raw[0] >> 6 | ((sc_raw[1] & 0x0F) << 2);
    sc[1] = (sc_raw[1] >> 4) | ((sc_raw[2] & 0x03) << 4);
    mn[1] = (sc_raw[2] >> 2);
    sc[2] = sc_raw[3] & 0x3F;
    mn[2] = sc_raw[3] >> 6 | ((sc_raw[4] & 0x0F) << 2);
    sc[3] = (sc_raw[4] >> 4) | ((sc_raw[5] & 0x03) << 4);
    mn[3] = (sc_raw[5] >> 2);
    sc[4] = sc_raw[6] & 0x3F;
    mn[4] = sc_raw[6] >> 6 | ((sc_raw[7] & 0x0F) << 2);
    sc[5] = (sc_raw[7] >> 4) | ((sc_raw[8] & 0x03) << 4);
    mn[5] = (sc_raw[8] >> 2);
    sc[6] = sc_raw[9] & 0x3F;
    mn[6] = sc_raw[9] >> 6 | ((sc_raw[10] & 0x0F) << 2);
    sc[7] = (sc_raw[10] >> 4) | ((sc_raw[11] & 0x03) << 4);
    mn[7] = (sc_raw[11] >> 2);
}

/* --- Q4_K: 144 bytes → 256 floats --- */

static void dequant_q4_k_block(const uint8_t *p, float *dst) {
    float d = f16_to_f32(read_u16(p));
    float dmin = f16_to_f32(read_u16(p + 2));
    uint8_t sc[8], mn[8];
    decode_k_scales(p + 4, sc, mn);
    const uint8_t *quants = p + 16;

    for (int sb = 0; sb < 8; sb++) {
        float scale = d * (float)sc[sb];
        float min_val = dmin * (float)mn[sb];
        const uint8_t *qb = quants + sb * 16;
        /* Process 4 bytes (8 floats) per iteration */
        for (int j = 0; j < 16; j += 4) {
            dst[sb * 32 + j * 2]     = scale * (float)(qb[j]     & 0x0F) - min_val;
            dst[sb * 32 + j * 2 + 1] = scale * (float)(qb[j]     >> 4)   - min_val;
            dst[sb * 32 + j * 2 + 2] = scale * (float)(qb[j + 1] & 0x0F) - min_val;
            dst[sb * 32 + j * 2 + 3] = scale * (float)(qb[j + 1] >> 4)   - min_val;
            dst[sb * 32 + j * 2 + 4] = scale * (float)(qb[j + 2] & 0x0F) - min_val;
            dst[sb * 32 + j * 2 + 5] = scale * (float)(qb[j + 2] >> 4)   - min_val;
            dst[sb * 32 + j * 2 + 6] = scale * (float)(qb[j + 3] & 0x0F) - min_val;
            dst[sb * 32 + j * 2 + 7] = scale * (float)(qb[j + 3] >> 4)   - min_val;
        }
    }
}

/* --- Q5_K: 176 bytes → 256 floats --- */

static void dequant_q5_k_block(const uint8_t *p, float *dst) {
    float d = f16_to_f32(read_u16(p));
    float dmin = f16_to_f32(read_u16(p + 2));
    uint8_t sc[8], mn[8];
    decode_k_scales(p + 4, sc, mn);
    const uint8_t *qh = p + 16;
    const uint8_t *ql = p + 48;

    for (int sb = 0; sb < 8; sb++) {
        float scale = d * (float)sc[sb];
        float min_val = dmin * (float)mn[sb];
        const uint8_t *qb = ql + sb * 16;
        const uint8_t *hb = qh + sb * 4;
        for (int j = 0; j < 16; j++) {
            int lo = (qb[j] & 0x0F) | (((hb[j / 4] >> ((j % 4) * 2))     & 1) << 4);
            int hi = (qb[j] >> 4)   | (((hb[j / 4] >> ((j % 4) * 2 + 1)) & 1) << 4);
            dst[sb * 32 + j * 2]     = scale * (float)lo - min_val;
            dst[sb * 32 + j * 2 + 1] = scale * (float)hi - min_val;
        }
    }
}

/* --- Q6_K: 210 bytes → 256 floats --- */

static void dequant_q6_k_block(const uint8_t *p, float *dst) {
    const uint8_t *ql = p;
    const uint8_t *qh = p + 128;
    const int8_t  *scales = (const int8_t *)(p + 192);
    float d = f16_to_f32(read_u16(p + 208));

    for (int sb = 0; sb < 16; sb++) {
        float sc = d * (float)scales[sb];
        for (int j = 0; j < 16; j++) {
            int idx = sb * 16 + j;
            int q_lo = (idx & 1) ? (ql[idx / 2] >> 4) : (ql[idx / 2] & 0x0F);
            int q_hi = (qh[idx / 4] >> ((idx % 4) * 2)) & 3;
            int q = q_lo | (q_hi << 4);
            dst[idx] = sc * ((float)q - 32.0f);
        }
    }
}

/* --- Dispatch ---
 * Switch-outside-loop: type check once, then tight inner loop per type.
 * Eliminates per-block branch prediction overhead for large tensors. */

int dequant(const void *src, float *dst, uint64_t n, uint32_t type) {
    if (type == GGML_TYPE_F32) {
        memcpy(dst, src, n * 4);
        return 0;
    }

    if (type == GGML_TYPE_F16) {
        const uint16_t *s = (const uint16_t *)src;
        /* Process 4 elements per iteration for auto-vectorization */
        uint64_t i = 0;
        for (; i + 3 < n; i += 4) {
            dst[i]     = f16_to_f32(s[i]);
            dst[i + 1] = f16_to_f32(s[i + 1]);
            dst[i + 2] = f16_to_f32(s[i + 2]);
            dst[i + 3] = f16_to_f32(s[i + 3]);
        }
        for (; i < n; i++)
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

    /* Switch-outside-loop: one branch, then tight dequant loop */
    switch (type) {
    case GGML_TYPE_Q4_0:
        for (uint64_t b = 0; b < n_blocks; b++)
            dequant_q4_0_block(p + b * block_bytes, dst + b * block_size);
        break;
    case GGML_TYPE_Q4_1:
        for (uint64_t b = 0; b < n_blocks; b++)
            dequant_q4_1_block(p + b * block_bytes, dst + b * block_size);
        break;
    case GGML_TYPE_Q5_0:
        for (uint64_t b = 0; b < n_blocks; b++)
            dequant_q5_0_block(p + b * block_bytes, dst + b * block_size);
        break;
    case GGML_TYPE_Q5_1:
        for (uint64_t b = 0; b < n_blocks; b++)
            dequant_q5_1_block(p + b * block_bytes, dst + b * block_size);
        break;
    case GGML_TYPE_Q8_0:
        for (uint64_t b = 0; b < n_blocks; b++)
            dequant_q8_0_block(p + b * block_bytes, dst + b * block_size);
        break;
    case GGML_TYPE_Q2_K:
        for (uint64_t b = 0; b < n_blocks; b++)
            dequant_q2_k_block(p + b * block_bytes, dst + b * block_size);
        break;
    case GGML_TYPE_Q3_K:
        for (uint64_t b = 0; b < n_blocks; b++)
            dequant_q3_k_block(p + b * block_bytes, dst + b * block_size);
        break;
    case GGML_TYPE_Q4_K:
        for (uint64_t b = 0; b < n_blocks; b++)
            dequant_q4_k_block(p + b * block_bytes, dst + b * block_size);
        break;
    case GGML_TYPE_Q5_K:
        for (uint64_t b = 0; b < n_blocks; b++)
            dequant_q5_k_block(p + b * block_bytes, dst + b * block_size);
        break;
    case GGML_TYPE_Q6_K:
        for (uint64_t b = 0; b < n_blocks; b++)
            dequant_q6_k_block(p + b * block_bytes, dst + b * block_size);
        break;
    default:
        return -1;
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
