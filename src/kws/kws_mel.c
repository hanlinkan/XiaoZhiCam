/*
 * kws_mel.c -- microWakeWord 特征前端（C 版）
 *
 * 移植自 ~/xzfw/port/src/xz_mel.cc，逻辑逐行保留，只做了三处改动：
 *   1. namespace -> 静态变量 + 前缀
 *   2. constexpr -> enum
 *   3. 错误路径补 NULL 检查（原版依赖调用方保证）
 *
 * 与训练侧的量化关系（不要改）：
 *   pymicro 返回 uint16 log-mel，Python 侧 data / 0.101960786 - 128
 *   后 astype(int8)（向零截断）。这里用同一公式，常数预先除好。
 */
#include "kws_mel.h"

#include <stddef.h>
#include <string.h>

#include "tensorflow/lite/experimental/microfrontend/lib/frontend.h"
#include "tensorflow/lite/experimental/microfrontend/lib/frontend_util.h"

enum { KWS_MEL_SAMPLE_RATE = 16000, KWS_MEL_FEATURE_SIZE = 40 };

static struct FrontendConfig s_cfg;
static struct FrontendState s_st;
static int s_ready = 0;

/* 0.0390625 / 0.101960786 ≈ 0.3831414，即 uint16 log-mel -> int8 特征。
 * 注意这是 float 常量：不做定点化，40 次乘加/帧在软浮点下也只有微秒级，
 * 而换定点就可能与训练侧差 1 个 LSB。 */
static const float kFeatureToInt8 = 0.0390625f / 0.101960786f;

static void mel_config(void)
{
    /* 先取上游默认值，再逐项覆盖 —— 与 pymicro_features 的做法一致。
     * 不要只写"看得见的那几个字段"：结构体里未显式赋值的项（比如
     * filterbank.output_scale_shift）留 0 会让前端输出整体错位。 */
    FrontendFillConfigWithDefaults(&s_cfg);

    s_cfg.window.size_ms = 30;
    s_cfg.window.step_size_ms = 10;

    s_cfg.filterbank.num_channels = KWS_MEL_FEATURE_SIZE;
    s_cfg.filterbank.lower_band_limit = 125.0f;
    s_cfg.filterbank.upper_band_limit = 7500.0f;

    s_cfg.noise_reduction.smoothing_bits = 10;
    s_cfg.noise_reduction.even_smoothing = 0.025f;
    s_cfg.noise_reduction.odd_smoothing = 0.06f;
    s_cfg.noise_reduction.min_signal_remaining = 0.05f;

    s_cfg.pcan_gain_control.enable_pcan = 1;
    s_cfg.pcan_gain_control.strength = 0.95f;
    s_cfg.pcan_gain_control.offset = 80.0f;
    s_cfg.pcan_gain_control.gain_bits = 21;

    s_cfg.log_scale.enable_log = 1;
    s_cfg.log_scale.scale_shift = 6;
}

int kws_mel_init(void)
{
    if (s_ready) {
        return 1;
    }
    mel_config();
    if (!FrontendPopulateState(&s_cfg, &s_st, KWS_MEL_SAMPLE_RATE)) {
        return 0;
    }
    s_ready = 1;
    return 1;
}

void kws_mel_reset(void)
{
    if (!s_ready) {
        return;
    }
    FrontendReset(&s_st);
    /* FrontendReset 不清 PCAN，见头文件里的说明。 */
    if (s_st.pcan_gain_control.noise_estimate != NULL) {
        memset(s_st.pcan_gain_control.noise_estimate, 0,
               sizeof(uint32_t) * (size_t)s_st.pcan_gain_control.num_channels);
    }
}

int kws_mel_process(const int16_t *pcm160, int8_t out40[40])
{
    size_t read = 0;
    struct FrontendOutput fo;
    int i;

    if (!s_ready || pcm160 == NULL || out40 == NULL) {
        return 0;
    }

    fo = FrontendProcessSamples(&s_st, pcm160, 160, &read);
    if (fo.size != (size_t)KWS_MEL_FEATURE_SIZE) {
        return 0;
    }

    for (i = 0; i < KWS_MEL_FEATURE_SIZE; i++) {
        const float v = (float)fo.values[i] * kFeatureToInt8;
        /* 先减 zp 再向零截断，与 Python 的 astype(int8) 完全同序。
         * 不能写成 (int)v - 128 再截断：log-mel 原始值能到 700 以上，
         * 那样会把 >255 的通道全部压成 127。
         * 例：value=665 -> v=254.7 -> q=126；先截断会得到 -1。 */
        int q = (int)(v - 128.0f);
        if (q > 127) q = 127;
        if (q < -128) q = -128;
        out40[i] = (int8_t)q;
    }
    return 1;
}
