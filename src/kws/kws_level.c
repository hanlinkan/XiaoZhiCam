/* kws_level.c -- 电平归一化（见头文件里的设计说明） */
#include "kws_level.h"

#include <stddef.h>

static int s_lvl_q8 = KWS_LEVEL_INIT_Q8;

/* int32 整数开方（逐位法，无除法、无 64 位乘）。 */
static int isqrt_u32(unsigned int x)
{
    unsigned int r = 0, b = 1u << 30;
    while (b > x) b >>= 2;
    while (b) {
        if (x >= r + b) { x -= r + b; r = (r >> 1) + b; }
        else            { r >>= 1; }
        b >>= 2;
    }
    return (int)r;
}

void kws_level_reset(void)
{
    s_lvl_q8 = KWS_LEVEL_INIT_Q8;
}

/* 诊断用：当前电平估计（Q8）与上一段的输入 RMS */
static int s_dbg_in_rms;

int kws_level_in_rms(void) { return s_dbg_in_rms; }

int kws_level_gain_q8(const int16_t *pcm8k, int n)
{
    unsigned int sq = 0;
    int i, rms, gain;

    if (pcm8k == NULL || n <= 0) {
        return KWS_LEVEL_GAIN_MIN;
    }

    for (i = 0; i < n; i++) {
        int32_t t = pcm8k[i] >> 2;
        sq += (unsigned int)(t * t);
    }
    rms = isqrt_u32(sq / (unsigned int)n) << 2;
    s_dbg_in_rms = rms;

    if (rms > 0) {
        int d = rms - s_lvl_q8;
        s_lvl_q8 += (d > 0) ? (d >> 3) : (d >> 5);
        if (s_lvl_q8 < 8) s_lvl_q8 = 8;
        if (s_lvl_q8 > 16384) s_lvl_q8 = 16384;
    }

    gain = (s_lvl_q8 > 0) ? ((KWS_LEVEL_TARGET_Q8 * 256) / s_lvl_q8) : KWS_LEVEL_GAIN_MAX;
    if (gain < KWS_LEVEL_GAIN_MIN) gain = KWS_LEVEL_GAIN_MIN;
    if (gain > KWS_LEVEL_GAIN_MAX) gain = KWS_LEVEL_GAIN_MAX;
    return gain;
}
