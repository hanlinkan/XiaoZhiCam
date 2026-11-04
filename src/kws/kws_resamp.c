/*
 * kws_resamp.c -- 8 kHz -> 16 kHz 半带插值（定点，Q13）
 *
 * 系数：scipy.signal.firwin(41, 0.5, window=('kaiser', 5.0)) × 2
 *       —— 与 scipy.signal.resample_poly(x, 2, 1) 内部用的完全同一组，
 *       保证与训练侧重采样同域。×2 是插值增益（每个相位的直流增益为 1）。
 *
 * 半带特性：41 抽头里偶数抽头除中心外全为 0，所以
 *   偶数相 y[2n]   = h[20] * x[n-10]
 *   奇数相 y[2n+1] = Σ_{j=0..19} h[2j+1] * x[n-j]
 * 每个 8k 样本只花 20 次乘加。
 *
 * 定点：系数 Q13，累加 int32。最坏情况 |acc| = 32767 × Σ|h| ≈ 6.0e8 < 2^31，
 * 不会溢出（Σ|h_odd| = 18360/8192 ≈ 2.24）。
 */
#include "kws_resamp.h"

#include <stddef.h>

/* 奇数相 20 个抽头，Q13，已含 2x 增益 */
static const int16_t k_rs_odd[20] = {
    -17, 41, -80, 140, -229, 361, -563, 906, -1654, 5189,
    5189, -1654, 906, -563, 361, -229, 140, -80, 41, -17
};

#define RS_CENTER  8196         /* h[20] × 2 × 8192（= 1.0005 × 8192） */
#define RS_SHIFT   13
#define RS_ROUND   (1 << (RS_SHIFT - 1))
#define RS_MASK    31u          /* 32 槽环形缓冲，够存 20 个历史 */

static int16_t s_buf[32];
static uint32_t s_pos;

static int16_t sat16(int v)
{
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

void kws_resamp_reset(void)
{
    int i;
    for (i = 0; i < 32; i++) s_buf[i] = 0;
    s_pos = 0;
}

int kws_resamp_2x(const int16_t *in, int n, int16_t *out)
{
    int i, j, m = 0;

    if (in == NULL || out == NULL || n <= 0) {
        return 0;
    }

    for (i = 0; i < n; i++) {
        int32_t acc = 0;

        s_buf[s_pos & RS_MASK] = in[i];

        for (j = 0; j < 20; j++) {
            acc += (int32_t)k_rs_odd[j] * s_buf[(s_pos - (uint32_t)j) & RS_MASK];
        }

        out[m++] = sat16((((int32_t)s_buf[(s_pos - 10u) & RS_MASK] * RS_CENTER) + RS_ROUND) >> RS_SHIFT);
        out[m++] = sat16((acc + RS_ROUND) >> RS_SHIFT);

        s_pos++;
    }
    return m;
}
