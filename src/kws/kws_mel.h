/*
 * kws_mel.h -- microWakeWord 特征前端（16 kHz / 40 维 / 10 ms），全定点
 *
 * 来源：~/xzfw/port/src/xz_mel.cc（C++）的 C 移植，逻辑逐行保留。
 * 底层是 TFLM microfrontend（mf/ 目录，Apache-2.0），配置与训练侧
 * pymicro-features 2.0.2 逐项对齐：
 *   window 30 ms / step 10 ms / 40 channels / 125~7500 Hz
 *   noise_reduction: bits 10, even 0.025, odd 0.06, min_signal_remaining 0.05
 *   pcan: enable, strength 0.95, offset 80, gain_bits 21
 *   log_scale: enable, scale_shift 6
 *
 * 为什么必须和训练侧逐位同源：mel 差一个 LSB，模型输入分布就变，
 * 概率会整段漂移。所以不要"重写一个看起来一样的"，要用同一份实现。
 *
 * 内存：Init 时 malloc 约 20 KB（滤波器组 / FFT 表 / FFT 缓冲），
 *       之后热路径零分配。
 */
#ifndef XZ_KWS_MEL_H
#define XZ_KWS_MEL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化（一次性）。返回 1 成功，0 失败（内存不足）。 */
int kws_mel_init(void);

/* 清空流式状态。换会话 / 重新武装检测时调用。
 *
 * ⚠️ 不只清 window/fft/filterbank：PCAN 的 noise_estimate 会随输入缓慢
 * 收敛，连续跑十几秒后底噪会被归一化成"像语音"的电平，模型输出一路涨到
 * 255（上一轮实测）。训练/验收时每段音频都是全新前端，所以这里必须手动
 * 把 PCAN 也清掉，让部署态与训练域一致。 */
void kws_mel_reset(void);

/* 喂 160 个 16 kHz 样本（10 ms），产出一帧 40 维 int8 特征。
 * 返回 1 表示产出了特征，0 表示还没攒够（或未初始化）。 */
int kws_mel_process(const int16_t *pcm160, int8_t out40[40]);

#ifdef __cplusplus
}
#endif

#endif /* XZ_KWS_MEL_H */
