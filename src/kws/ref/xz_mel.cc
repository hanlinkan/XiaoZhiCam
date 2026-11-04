/*
 * mel 前端封装：把 vendor 的 TFLM microfrontend 按训练时的配置初始化，
 * 并把它的 uint16 log-mel 输出量化成模型要的 int8。
 *
 * 配置与 pymicro-features 2.0.2 的 src/micro_features.cpp 逐项对齐：
 *   window 30 ms / step 10 ms / 40 channels / 125~7500 Hz
 *   noise_reduction: bits 10, even 0.025, odd 0.06, min_signal_remaining 0.05
 *   pcan: enable, strength 0.95, offset 80, gain_bits 21
 *   log_scale: enable, scale_shift 6
 *
 * 量化：pymicro 返回 value * 0.0390625（float32），Python 侧再做
 * data / 0.101960786 + (-128) 后 astype(int8)（向零截断）。
 * 这里用同一个公式，只是常数预先除好，避免每帧做一次除法。
 */
#include "xz_mel.h"

#include <cstring>

extern "C" {
#include "tensorflow/lite/experimental/microfrontend/lib/frontend.h"
#include "tensorflow/lite/experimental/microfrontend/lib/frontend_util.h"
}

namespace {

constexpr int kSampleRate = 16000;
constexpr int kFeatureSize = 40;

FrontendConfig g_cfg;
FrontendState g_st;
bool g_ready = false;

/* 0.0390625 / 0.101960786 ≈ 0.3831414，即 uint16 log-mel -> int8 特征 */
constexpr float kFeatureToInt8 = 0.0390625f / 0.101960786f;

void Config() {
    /* 先取上游默认值，再逐项覆盖 —— 与 pymicro_features 的做法一致。
     * 不要只写"看得见的那几个字段"：结构体里未显式赋值的项（比如 filterbank
     * 的 output_scale_shift）留 0 会让前端输出整体错位。 */
    FrontendFillConfigWithDefaults(&g_cfg);

    g_cfg.window.size_ms = 30;
    g_cfg.window.step_size_ms = 10;

    g_cfg.filterbank.num_channels = kFeatureSize;
    g_cfg.filterbank.lower_band_limit = 125.0f;
    g_cfg.filterbank.upper_band_limit = 7500.0f;

    g_cfg.noise_reduction.smoothing_bits = 10;
    g_cfg.noise_reduction.even_smoothing = 0.025f;
    g_cfg.noise_reduction.odd_smoothing = 0.06f;
    g_cfg.noise_reduction.min_signal_remaining = 0.05f;

    g_cfg.pcan_gain_control.enable_pcan = 1;
    g_cfg.pcan_gain_control.strength = 0.95f;
    g_cfg.pcan_gain_control.offset = 80.0f;
    g_cfg.pcan_gain_control.gain_bits = 21;

    g_cfg.log_scale.enable_log = 1;
    g_cfg.log_scale.scale_shift = 6;
}

}  // namespace

namespace xz_mel {

int Init() {
    if (g_ready) {
        return 1;
    }
    Config();
    if (!FrontendPopulateState(&g_cfg, &g_st, kSampleRate)) {
        return 0;
    }
    g_ready = true;
    return 1;
}

void Reset() {
    if (!g_ready) {
        return;
    }
    FrontendReset(&g_st);
    /* ⚠️ FrontendReset 只清 window/fft/filterbank/降噪，**不清 PCAN**。
     * PCAN 的 noise_estimate 会随输入缓慢收敛：连续跑十几秒后，底噪被它
     * 归一化成"像语音"的电平，模型的输出会一路涨到 255（实测）。
     * 训练/验收时每段音频都是全新前端，PCAN 从 0 开始 —— 这里手动清掉，
     * 让部署时的状态和训练域一致。 */
    if (g_st.pcan_gain_control.noise_estimate != nullptr) {
        memset(g_st.pcan_gain_control.noise_estimate, 0,
               sizeof(uint32_t) * (size_t)g_st.pcan_gain_control.num_channels);
    }
}

int Process10ms(const int16_t* pcm160, int8_t out40[40]) {
    if (!g_ready || pcm160 == nullptr || out40 == nullptr) {
        return 0;
    }
    size_t read = 0;
    const FrontendOutput fo = FrontendProcessSamples(&g_st, pcm160, 160, &read);
    if (fo.size != (size_t)kFeatureSize) {
        return 0;
    }
    for (int i = 0; i < kFeatureSize; i++) {
        const float v = (float)fo.values[i] * kFeatureToInt8;
        /* 先减 zp 再向零截断，与 Python 的 astype(int8) 完全同序。
         * 注意不能写成 (int)v - 128 再截断：log-mel 原始值能到 700 以上，
         * 那样会把 >255 的通道全部压成 127。
         * 例：value=665 -> v=254.7 -> q=126；先截断则会得到 -1。 */
        int q = (int)(v - 128.0f);
        if (q > 127) q = 127;
        if (q < -128) q = -128;
        out40[i] = (int8_t)q;
    }
    return 1;
}

}  // namespace xz_mel
