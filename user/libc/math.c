#include "libc.h"

/* All math functions are pure C, compiled with -msse2 for float support */

float fabsf(float x) {
    /* Clear sign bit */
    union { float f; uint32_t u; } u;
    u.f = x;
    u.u &= 0x7FFFFFFF;
    return u.f;
}

float floorf(float x) {
    int i = (int)x;
    if (x < 0.0f && x != (float)i)
        i--;
    return (float)i;
}

float ceilf(float x) {
    int i = (int)x;
    if (x > 0.0f && x != (float)i)
        i++;
    return (float)i;
}

float fmaxf(float a, float b) {
    return a > b ? a : b;
}

float fminf(float a, float b) {
    return a < b ? a : b;
}

/* sqrtf via Newton-Raphson iteration */
float sqrtf(float x) {
    if (x < 0.0f) return -1.0f; /* NaN-like behavior */
    if (x == 0.0f) return 0.0f;

    /* Initial estimate using bit manipulation */
    union { float f; uint32_t u; } u;
    u.f = x;
    u.u = (u.u >> 1) + 0x1FC00000; /* rough approximation */

    float guess = u.f;

    /* 4 Newton-Raphson iterations: guess = (guess + x/guess) / 2 */
    guess = 0.5f * (guess + x / guess);
    guess = 0.5f * (guess + x / guess);
    guess = 0.5f * (guess + x / guess);
    guess = 0.5f * (guess + x / guess);

    return guess;
}

/*
 * expf via range reduction + polynomial approximation
 *
 * exp(x) = 2^k * exp(r) where x = k*ln2 + r, |r| <= ln2/2
 * exp(r) ≈ 1 + r + r²/2 + r³/6 + r⁴/24 + r⁵/120
 */
float expf(float x) {
    /* Clamp to prevent overflow/underflow */
    if (x > 88.0f) return 3.4028235e+38f; /* ~FLT_MAX */
    if (x < -88.0f) return 0.0f;

    /* Range reduction: x = k * ln(2) + r */
    const float ln2 = 0.6931471805599453f;
    const float inv_ln2 = 1.4426950408889634f;

    float k_real = x * inv_ln2;
    int k = (int)k_real;
    if (k_real < 0.0f && (float)k != k_real)
        k--;

    float r = x - (float)k * ln2;

    /* Polynomial approximation for exp(r), |r| <= ln2/2 */
    float r2 = r * r;
    float r3 = r2 * r;
    float r4 = r3 * r;
    float r5 = r4 * r;

    float exp_r = 1.0f + r + r2 * 0.5f + r3 * (1.0f / 6.0f)
                  + r4 * (1.0f / 24.0f) + r5 * (1.0f / 120.0f);

    /* Multiply by 2^k using bit manipulation */
    union { float f; uint32_t u; } scale;
    scale.u = (uint32_t)((k + 127) << 23);

    return exp_r * scale.f;
}

/*
 * logf via exponent extraction + polynomial approximation
 *
 * log(x) = k*ln(2) + log(m) where x = m * 2^k, 1 <= m < 2
 * log(m) approximated by polynomial around m=1
 */
float logf(float x) {
    if (x <= 0.0f) return -3.4028235e+38f; /* -inf-like */

    /* Extract exponent and mantissa */
    union { float f; uint32_t u; } u;
    u.f = x;

    int exp_bits = (int)((u.u >> 23) & 0xFF) - 127;
    /* Normalize mantissa to [1.0, 2.0) */
    u.u = (u.u & 0x007FFFFF) | 0x3F800000;
    float m = u.f;

    /* t = m - 1, use polynomial log(1+t) ≈ t - t²/2 + t³/3 - t⁴/4 + t⁵/5 */
    float t = m - 1.0f;
    float t2 = t * t;
    float t3 = t2 * t;
    float t4 = t3 * t;
    float t5 = t4 * t;

    float log_m = t - t2 * 0.5f + t3 * (1.0f / 3.0f)
                  - t4 * 0.25f + t5 * 0.2f;

    const float ln2 = 0.6931471805599453f;
    return (float)exp_bits * ln2 + log_m;
}

/*
 * tanhf via expf:
 *   tanh(x) = (e^2x - 1) / (e^2x + 1)
 * For large |x|, clamp to ±1.
 */
float tanhf(float x) {
    if (x > 10.0f) return 1.0f;
    if (x < -10.0f) return -1.0f;

    float e2x = expf(2.0f * x);
    return (e2x - 1.0f) / (e2x + 1.0f);
}

/*
 * sinf via range reduction + Taylor series
 *
 * Reduce x to [-pi, pi], then to [-pi/2, pi/2].
 * Taylor: x - x^3/6 + x^5/120 - x^7/5040 + x^9/362880
 */
float sinf(float x) {
    const float PI = 3.14159265358979323846f;
    const float TWO_PI = 6.28318530717958647692f;
    const float HALF_PI = 1.57079632679489661923f;

    /* Range reduce to [-pi, pi] */
    x = x - floorf(x / TWO_PI) * TWO_PI;
    if (x > PI)
        x -= TWO_PI;
    if (x < -PI)
        x += TWO_PI;

    /* Reduce to [-pi/2, pi/2] using sin(pi - x) = sin(x) */
    if (x > HALF_PI)
        x = PI - x;
    else if (x < -HALF_PI)
        x = -PI - x;

    /* Taylor series: x - x^3/6 + x^5/120 - x^7/5040 + x^9/362880 */
    float x2 = x * x;
    float x3 = x2 * x;
    float x5 = x3 * x2;
    float x7 = x5 * x2;
    float x9 = x7 * x2;

    return x - x3 * (1.0f / 6.0f) + x5 * (1.0f / 120.0f)
           - x7 * (1.0f / 5040.0f) + x9 * (1.0f / 362880.0f);
}

float cosf(float x) {
    const float HALF_PI = 1.57079632679489661923f;
    return sinf(x + HALF_PI);
}

float sigmoidf(float x) {
    return 1.0f / (1.0f + expf(-x));
}
