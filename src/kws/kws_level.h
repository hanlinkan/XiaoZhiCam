/*
 * kws_level.h -- 唤醒词支路的电平归一化（8 kHz，Q8 增益）
 *
 * 为什么需要：microWakeWord 对**绝对电平**很敏感。实测同一段录音，
 * 固定增益 0/+18/+24/+30 dB 时模型峰值分别是 105/136/207/220，只有
 * 超过某个量才认得出——105 就是它把整段当静音。设备端电平随房间、
 * PGA、麦克风个体差异变化，所以固定增益是运气，归一化才是工程解。
 *
 * 目标：把语音 RMS 拉到 -23 dBFS（训练样本的域），增益钳在 0~+36 dB。
 * 用 RMS 而不是峰值：训练样本是峰值归一化的，滤波后峰均比很大，
 * 按峰值归一化反而拉不上去（实测低 15 dB）。
 *
 * 时间常数：上升 >>3（约 320 ms）、下降 >>5（约 1.3 s）。
 * 说大声音时不要立刻把增益压下去，否则词的包络被压扁。
 *
 * 单独成一个模块，是为了让"训练侧特征提取"和"固件判决层"用**同一份**
 * 电平逻辑——否则训练/部署又会分家。
 */
#ifndef XZ_KWS_LEVEL_H
#define XZ_KWS_LEVEL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KWS_LEVEL_TARGET_Q8   2320    /* -23 dBFS，Q8（32768 × 10^(-23/20)） */
#define KWS_LEVEL_INIT_Q8     104     /* -50 dBFS，偏保守的初值 */
#define KWS_LEVEL_GAIN_MIN    256     /* 0 dB */
#define KWS_LEVEL_GAIN_MAX    16384   /* +36 dB */

void kws_level_reset(void);

/* 送一段 8 kHz PCM（设备上是 320 点 / 40 ms），返回本段使用的增益（Q8）。
 * 内部会先更新电平估计再算增益。 */
int kws_level_gain_q8(const int16_t *pcm8k, int n);

/* 诊断：最近一段输入的 RMS（8 kHz 原始域，未加增益） */
int kws_level_in_rms(void);

#ifdef __cplusplus
}
#endif

#endif /* XZ_KWS_LEVEL_H */
