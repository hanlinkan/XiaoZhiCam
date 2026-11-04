/*
 * microWakeWord 定点推理引擎。
 *
 * 数据流（与 tflite 逐算子对应，张量编号见 out/kws/inspect_model.py 的输出）：
 *
 *   mel[3x40] --concat(hist2)--> [5x40] --conv 32@5x40--> [32]
 *     --concat(hist4)--> [5x32] --dw 5--> [32] --conv 64@1x1--> [64]
 *     --concat(hist10)--> [11x64] --split--> 低32 取后7 帧 --dw 7--> [32]
 *                                     \--> 高32 取后11帧 --dw 11-> [32]
 *     --concat(通道)--> [64] --conv 64@1x1--> [64]
 *     --concat(hist14)--> [15x64] --split--> 低32 取后9 帧 --dw 9--> [32]
 *                                     \--> 高32 取后15帧 --dw 15-> [32]
 *     --concat--> [64] --conv 64@1x1--> [64]
 *     --concat(hist22)--> [23x64] --dw 23--> [64] --conv 64@1x1--> [64]
 *     --concat(hist16)--> [17x64] --flatten 1088--> FC --> [1] --logistic--> 概率
 *
 * 两个实现要点（都在 TFLite 里踩过）：
 *   1. requantize 用 gemmlowp 的两步舍入：SaturatingRoundingDoublingHighMul 后
 *      RoundingDivideByPOT。少一步就差 1 LSB，经 sigmoid 会放大成 12 个概率 LSB。
 *   2. 权重是 per-channel 量化，乘数必须逐输出通道取，不能取一个均值。
 */
#include "xz_mww.h"

#include <cstring>

#include "mww_model.h"

namespace {

constexpr int kMel = 40;
constexpr int kC1 = 32;
constexpr int kC2 = 64;

/* 历史缓冲（帧数 x 通道） */
int8_t s_mel[2 * kMel];
int8_t s1[4 * kC1];
int8_t s2l[10 * kC1], s2h[10 * kC1];
int8_t s3l[14 * kC1], s3h[14 * kC1];
int8_t s4[22 * kC2];
int8_t s5[16 * kC2];

/* 工作缓冲 */
int8_t b_mel[5 * kMel];
int8_t b1[5 * kC1];
int8_t b2l[11 * kC1], b2h[11 * kC1];
int8_t b3l[15 * kC1], b3h[15 * kC1];
int8_t b4[23 * kC2];
int8_t b5[17 * kC2];
int8_t t32[kC1];
int8_t t64a[kC2], t64b[kC2], t64c[kC2];
int8_t pend[3 * kMel];
int pend_n = 0;
int last_prob = 0;

inline int8_t Clamp8(int32_t v) {
    if (v < -128) return -128;
    if (v > 127) return 127;
    return (int8_t)v;
}

/* gemmlowp::SaturatingRoundingDoublingHighMul */
inline int32_t SatRoundHighMul(int32_t a, int32_t b) {
    const bool overflow = (a == b && a == INT32_MIN);
    const int64_t ab = (int64_t)a * b;
    const int32_t nudge = ab >= 0 ? (1 << 30) : (1 - (1 << 30));
    const int32_t hi = (int32_t)((ab + nudge) / (1ll << 31));
    return overflow ? INT32_MAX : hi;
}

/* gemmlowp::RoundingDivideByPOT */
inline int32_t RoundDivPOT(int32_t x, int exponent) {
    if (exponent <= 0) return x;
    const int32_t mask = (int32_t)((1ll << exponent) - 1);
    const int32_t rem = x & mask;
    const int32_t thr = (mask >> 1) + (x < 0 ? 1 : 0);
    return (x >> exponent) + (rem > thr ? 1 : 0);
}

/* gemmlowp::MultiplyByQuantizedMultiplier */
inline int32_t MulQ31(int32_t x, int32_t mult, int32_t shift) {
    const int left = shift > 0 ? shift : 0;
    const int right = shift > 0 ? 0 : -shift;
    int64_t xl = (int64_t)x << left;
    if (xl > INT32_MAX) xl = INT32_MAX;
    if (xl < INT32_MIN) xl = INT32_MIN;
    return RoundDivPOT(SatRoundHighMul((int32_t)xl, mult), right);
}

/* 一层卷积：stride=1、无 padding，输入按 [时间][通道] 连续存放。
 * is_dw 时权重按 [时间][通道]（每通道独立），否则 [输出][时间][输入]。 */
void ApplyLayer(const MwwLayer& L, const int8_t* in, int8_t* out) {
    const int in_off = -L.in_zp;   /* TFLite 的 input_offset */
    if (L.is_dw) {
        for (int c = 0; c < L.out_ch; c++) {
            int32_t acc = L.b[c];
            for (int t = 0; t < L.k; t++) {
                acc += (int32_t)(in[t * L.out_ch + c] + in_off) * L.w[t * L.out_ch + c];
            }
            out[c] = Clamp8(MulQ31(acc, L.mult[c], L.shift[c]) + L.out_zp);
        }
    } else {
        for (int o = 0; o < L.out_ch; o++) {
            int32_t acc = L.b[o];
            const int8_t* wo = L.w + (size_t)o * L.k * L.in_ch;
            for (int t = 0; t < L.k; t++) {
                const int8_t* ip = in + (size_t)t * L.in_ch;
                const int8_t* wp = wo + (size_t)t * L.in_ch;
                for (int c = 0; c < L.in_ch; c++) {
                    acc += (int32_t)(ip[c] + in_off) * wp[c];
                }
            }
            out[o] = Clamp8(MulQ31(acc, L.mult[o], L.shift[o]) + L.out_zp);
        }
    }
}

/* 把一段 [n][ch] 的 64 通道数据按前 32/后 32 拆开写 */
inline void StoreSplit(const int8_t* src, int n, int8_t* lo, int8_t* hi) {
    for (int i = 0; i < n; i++) {
        for (int c = 0; c < 32; c++) {
            lo[i * 32 + c] = src[i * 64 + c];
            hi[i * 32 + c] = src[i * 64 + 32 + c];
        }
    }
}

/* 取 [frames][32] 里最后 k 帧的起点 */
inline const int8_t* LastFrames(const int8_t* buf, int frames, int k) {
    return buf + (frames - k) * 32;
}

int Run() {
    const MwwLayer& l_conv1 = kMwwLayers[0];
    const MwwLayer& l_dw1 = kMwwLayers[1];
    const MwwLayer& l_conv2 = kMwwLayers[2];
    const MwwLayer& l_dw2 = kMwwLayers[3];
    const MwwLayer& l_dw3 = kMwwLayers[4];
    const MwwLayer& l_conv3 = kMwwLayers[5];
    const MwwLayer& l_dw4 = kMwwLayers[6];
    const MwwLayer& l_dw5 = kMwwLayers[7];
    const MwwLayer& l_conv4 = kMwwLayers[8];
    const MwwLayer& l_dw6 = kMwwLayers[9];
    const MwwLayer& l_conv5 = kMwwLayers[10];
    const MwwLayer& l_fc = kMwwLayers[11];

    /* 1) mel 历史 + 新的 3 帧 */
    memcpy(b_mel, s_mel, sizeof(s_mel));
    memcpy(b_mel + 2 * kMel, pend, sizeof(pend));
    memcpy(s_mel, b_mel + 3 * kMel, sizeof(s_mel));   /* 最近 2 帧 */

    ApplyLayer(l_conv1, b_mel, t32);                  /* [5x40] -> 32 */

    /* 2) block0：dw5 + pointwise */
    memcpy(b1, s1, sizeof(s1));
    memcpy(b1 + 4 * kC1, t32, kC1);
    memcpy(s1, b1 + 1 * kC1, sizeof(s1));             /* 最近 4 帧 */
    ApplyLayer(l_dw1, b1, t32);                       /* zp -128 -> -26 */
    ApplyLayer(l_conv2, t32, t64a);                   /* -> 64, zp -128 */

    /* 3) block1：concat(10) -> split -> dw7 / dw11 -> concat -> 1x1 */
    memcpy(b2l, s2l, sizeof(s2l));
    memcpy(b2h, s2h, sizeof(s2h));
    StoreSplit(t64a, 1, b2l + 10 * 32, b2h + 10 * 32);
    ApplyLayer(l_dw2, LastFrames(b2l, 11, 7), t64a);  /* 低 32 通道，后 7 帧 */
    ApplyLayer(l_dw3, b2h, t64a + 32);                /* 高 32 通道，11 帧 */
    memcpy(s2l, b2l + 1 * 32, sizeof(s2l));           /* 最近 10 帧 */
    memcpy(s2h, b2h + 1 * 32, sizeof(s2h));
    ApplyLayer(l_conv3, t64a, t64b);

    /* 4) block2：concat(14) -> split -> dw9 / dw15 -> concat -> 1x1 */
    memcpy(b3l, s3l, sizeof(s3l));
    memcpy(b3h, s3h, sizeof(s3h));
    StoreSplit(t64b, 1, b3l + 14 * 32, b3h + 14 * 32);
    ApplyLayer(l_dw4, LastFrames(b3l, 15, 9), t64b);
    ApplyLayer(l_dw5, b3h, t64b + 32);
    memcpy(s3l, b3l + 1 * 32, sizeof(s3l));
    memcpy(s3h, b3h + 1 * 32, sizeof(s3h));
    ApplyLayer(l_conv4, t64b, t64c);

    /* 5) block3：concat(22) -> dw23 -> 1x1 */
    memcpy(b4, s4, sizeof(s4));
    memcpy(b4 + 22 * kC2, t64c, kC2);
    memcpy(s4, b4 + 1 * kC2, sizeof(s4));             /* 最近 22 帧 */
    ApplyLayer(l_dw6, b4, t64a);
    ApplyLayer(l_conv5, t64a, t64b);

    /* 6) 尾部 concat(16) -> flatten -> FC -> logistic */
    memcpy(b5, s5, sizeof(s5));
    memcpy(b5 + 16 * kC2, t64b, kC2);
    memcpy(s5, b5 + 1 * kC2, sizeof(s5));             /* 最近 16 帧 */
    ApplyLayer(l_fc, b5, t32);

    const int lut_idx = (int)t32[0] + 128;
    last_prob = (int)mww_logistic[lut_idx] + MWW_LOGISTIC_OUT_ADD;
    if (last_prob < 0) last_prob = 0;
    if (last_prob > 255) last_prob = 255;
    return last_prob;
}

}  // namespace

namespace xz_mww {

void Reset() {
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

int Feed(const int8_t* mel40) {
    if (mel40 == nullptr) return -1;
    memcpy(pend + pend_n * kMel, mel40, kMel);
    if (++pend_n < 3) return -1;
    pend_n = 0;
    return Run();
}

int LastProbability() { return last_prob; }

}  // namespace xz_mww
