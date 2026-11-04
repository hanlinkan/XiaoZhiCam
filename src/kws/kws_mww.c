/*
 * kws_mww.c -- microWakeWord v2 定点推理引擎（C 版）
 *
 * 移植自 ~/xzfw/port/src/xz_mww.cc，逐行保留算术顺序，只做：
 *   namespace -> static、constexpr -> enum、引用 -> 指针。
 *
 * ⚠️ 这个文件里的每一步都是"逐位对齐 TFLite"的一部分，改任何一处
 *    （比如把 MulQ31 换成 >>、把 int64 累加换成 int32、调整累加顺序）
 *    都会让概率偏几个 LSB，模型行为就变了。改动必须跑 test/ 的逐位比对。
 */
#include "kws_mww.h"

#include <stdint.h>
#include <string.h>
#include <limits.h>

#include "mww_model.h"

enum { kMel = 40, kC1 = 32, kC2 = 64 };

/* ---- 流式历史缓冲（初值 -128） ---- */
static int8_t s_mel[2 * 40];
static int8_t s1[4 * 32];
static int8_t s2l[10 * 32], s2h[10 * 32];
static int8_t s3l[14 * 32], s3h[14 * 32];
static int8_t s4[22 * 64];
static int8_t s5[16 * 64];

/* ---- 工作缓冲（每帧复用，不参与历史） ---- */
static int8_t b_mel[5 * 40];
static int8_t b1[5 * 32];
static int8_t b2l[11 * 32], b2h[11 * 32];
static int8_t b3l[15 * 32], b3h[15 * 32];
static int8_t b4[23 * 64];
static int8_t b5[17 * 64];
static int8_t t32[32];
static int8_t t64a[64], t64b[64], t64c[64];

/* ---- 三帧攒批 ---- */
static int8_t pend[3 * 40];
static int pend_n = 0;
static int last_prob = 0;

static int8_t clamp8(int32_t v)
{
    if (v < -128) return -128;
    if (v > 127) return 127;
    return (int8_t)v;
}

/* gemmlowp::SaturatingRoundingDoublingHighMul
 * round(a*b / 2^31)，溢出饱和到 INT32_MAX。 */
static int32_t sat_round_high_mul(int32_t a, int32_t b)
{
    const int overflow = (a == b && a == INT32_MIN);
    const int64_t ab = (int64_t)a * b;
    const int32_t nudge = ab >= 0 ? (1 << 30) : (1 - (1 << 30));
    const int32_t hi = (int32_t)((ab + nudge) / (1LL << 31));
    return overflow ? INT32_MAX : hi;
}

/* gemmlowp::RoundingDivideByPOT */
static int32_t round_div_pot(int32_t x, int exponent)
{
    int32_t mask, rem, thr;
    if (exponent <= 0) return x;
    mask = (int32_t)((1LL << exponent) - 1);
    rem = x & mask;
    thr = (mask >> 1) + (x < 0 ? 1 : 0);
    return (x >> exponent) + (rem > thr ? 1 : 0);
}

/* gemmlowp::MultiplyByQuantizedMultiplier */
static int32_t mul_q31(int32_t x, int32_t mult, int32_t shift)
{
    const int left = shift > 0 ? shift : 0;
    const int right = shift > 0 ? 0 : -shift;
    /* 用乘法代替左移：对负数左移是未定义行为（UBSan 实测 x=-79516 时
     * 触发），-O2 下会被优化出错误结果——v4 权重下直接段错误。
     * 1LL << left 本身安全（left ≤ 31），乘法是定义良好的。 */
    int64_t xl = (int64_t)x * ((int64_t)1 << left);
    if (xl > INT32_MAX) xl = INT32_MAX;
    if (xl < INT32_MIN) xl = INT32_MIN;
    return round_div_pot(sat_round_high_mul((int32_t)xl, mult), right);
}

/* 一层卷积：stride=1、无 padding，输入按 [时间][通道] 连续存放。
 * is_dw 时权重按 [时间][通道]（每通道独立），否则 [输出][时间][输入]。 */
static void apply_layer(const MwwLayer *L, const int8_t *in, int8_t *out)
{
    const int in_off = -L->in_zp;   /* TFLite 的 input_offset */
    int c, t, o;

    if (L->is_dw) {
        for (c = 0; c < L->out_ch; c++) {
            int32_t acc = L->b[c];
            for (t = 0; t < L->k; t++) {
                acc += (int32_t)(in[t * L->out_ch + c] + in_off) * L->w[t * L->out_ch + c];
            }
            out[c] = clamp8(mul_q31(acc, L->mult[c], L->shift[c]) + L->out_zp);
        }
    } else {
        for (o = 0; o < L->out_ch; o++) {
            int32_t acc = L->b[o];
            const int8_t *wo = L->w + (size_t)o * L->k * L->in_ch;
            for (t = 0; t < L->k; t++) {
                const int8_t *ip = in + (size_t)t * L->in_ch;
                const int8_t *wp = wo + (size_t)t * L->in_ch;
                for (c = 0; c < L->in_ch; c++) {
                    acc += (int32_t)(ip[c] + in_off) * wp[c];
                }
            }
            out[o] = clamp8(mul_q31(acc, L->mult[o], L->shift[o]) + L->out_zp);
        }
    }
}

/* 把一段 [n][64] 按前 32 / 后 32 拆开写 */
static void store_split(const int8_t *src, int n, int8_t *lo, int8_t *hi)
{
    int i, c;
    for (i = 0; i < n; i++) {
        for (c = 0; c < 32; c++) {
            lo[i * 32 + c] = src[i * 64 + c];
            hi[i * 32 + c] = src[i * 64 + 32 + c];
        }
    }
}

/* 取 [frames][32] 里最后 k 帧的起点 */
static const int8_t *last_frames(const int8_t *buf, int frames, int k)
{
    return buf + (frames - k) * 32;
}

static int run_net(void)
{
    const MwwLayer *l_conv1 = &kMwwLayers[0];
    const MwwLayer *l_dw1 = &kMwwLayers[1];
    const MwwLayer *l_conv2 = &kMwwLayers[2];
    const MwwLayer *l_dw2 = &kMwwLayers[3];
    const MwwLayer *l_dw3 = &kMwwLayers[4];
    const MwwLayer *l_conv3 = &kMwwLayers[5];
    const MwwLayer *l_dw4 = &kMwwLayers[6];
    const MwwLayer *l_dw5 = &kMwwLayers[7];
    const MwwLayer *l_conv4 = &kMwwLayers[8];
    const MwwLayer *l_dw6 = &kMwwLayers[9];
    const MwwLayer *l_conv5 = &kMwwLayers[10];
    const MwwLayer *l_fc = &kMwwLayers[11];
    int lut_idx;

    /* 1) mel 历史 + 新的 3 帧 */
    memcpy(b_mel, s_mel, sizeof(s_mel));
    memcpy(b_mel + 2 * kMel, pend, sizeof(pend));
    memcpy(s_mel, b_mel + 3 * kMel, sizeof(s_mel));   /* 最近 2 帧 */

    apply_layer(l_conv1, b_mel, t32);                 /* [5x40] -> 32 */

    /* 2) block0：dw5 + pointwise */
    memcpy(b1, s1, sizeof(s1));
    memcpy(b1 + 4 * kC1, t32, kC1);
    memcpy(s1, b1 + 1 * kC1, sizeof(s1));             /* 最近 4 帧 */
    apply_layer(l_dw1, b1, t32);                      /* zp -128 -> -26 */
    apply_layer(l_conv2, t32, t64a);                  /* -> 64, zp -128 */

    /* 3) block1：concat(10) -> split -> dw7 / dw11 -> concat -> 1x1 */
    memcpy(b2l, s2l, sizeof(s2l));
    memcpy(b2h, s2h, sizeof(s2h));
    store_split(t64a, 1, b2l + 10 * 32, b2h + 10 * 32);
    apply_layer(l_dw2, last_frames(b2l, 11, 7), t64a); /* 低 32 通道，后 7 帧 */
    apply_layer(l_dw3, b2h, t64a + 32);                /* 高 32 通道，11 帧 */
    memcpy(s2l, b2l + 1 * 32, sizeof(s2l));            /* 最近 10 帧 */
    memcpy(s2h, b2h + 1 * 32, sizeof(s2h));
    apply_layer(l_conv3, t64a, t64b);

    /* 4) block2：concat(14) -> split -> dw9 / dw15 -> concat -> 1x1 */
    memcpy(b3l, s3l, sizeof(s3l));
    memcpy(b3h, s3h, sizeof(s3h));
    store_split(t64b, 1, b3l + 14 * 32, b3h + 14 * 32);
    apply_layer(l_dw4, last_frames(b3l, 15, 9), t64b);
    apply_layer(l_dw5, b3h, t64b + 32);
    memcpy(s3l, b3l + 1 * 32, sizeof(s3l));
    memcpy(s3h, b3h + 1 * 32, sizeof(s3h));
    apply_layer(l_conv4, t64b, t64c);

    /* 5) block3：concat(22) -> dw23 -> 1x1 */
    memcpy(b4, s4, sizeof(s4));
    memcpy(b4 + 22 * kC2, t64c, kC2);
    memcpy(s4, b4 + 1 * kC2, sizeof(s4));             /* 最近 22 帧 */
    apply_layer(l_dw6, b4, t64a);
    apply_layer(l_conv5, t64a, t64b);

    /* 6) 尾部 concat(16) -> flatten -> FC -> logistic */
    memcpy(b5, s5, sizeof(s5));
    memcpy(b5 + 16 * kC2, t64b, kC2);
    memcpy(s5, b5 + 1 * kC2, sizeof(s5));             /* 最近 16 帧 */
    apply_layer(l_fc, b5, t32);

    lut_idx = (int)t32[0] + 128;
    last_prob = (int)mww_logistic[lut_idx] + MWW_LOGISTIC_OUT_ADD;
    if (last_prob < 0) last_prob = 0;
    if (last_prob > 255) last_prob = 255;
    return last_prob;
}

void kws_mww_reset(void)
{
    memset(s_mel, -128, sizeof(s_mel));
    memset(s1, -128, sizeof(s1));
    memset(s2l, -128, sizeof(s2l));
    memset(s2h, -128, sizeof(s2h));
    memset(s3l, -128, sizeof(s3l));
    memset(s3h, -128, sizeof(s3h));
    memset(s4, -128, sizeof(s4));
    memset(s5, -128, sizeof(s5));
    pend_n = 0;
    last_prob = 0;
}

int kws_mww_feed(const int8_t mel40[40])
{
    if (mel40 == NULL) return -1;
    memcpy(pend + pend_n * kMel, mel40, kMel);
    if (++pend_n < 3) return -1;
    pend_n = 0;
    return run_net();
}

int kws_mww_last_prob(void)
{
    return last_prob;
}
