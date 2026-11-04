/*
 * kws_resamp.h -- 8 kHz -> 16 kHz 半带插值（定点，Q13）
 *
 * 为什么不用线性插值：训练侧是 scipy.signal.resample_poly(x, 2, 1)，
 * 线性插值的高频镜像会直接落进 mel 前端，模型看到的谱和训练时不一样。
 * 这里用同一组滤波器系数（firwin(41, 0.5, kaiser(5)) ×2），只是定点化。
 *
 * 代价：每个输出样本 20 次乘加（偶数相只剩中心抽头，等于 1 次乘），
 *       8k->16k 每秒 16000 输出 ≈ 160k 乘加/秒，对 240 MHz 是 1% 量级。
 */
#ifndef XZ_KWS_RESAMP_H
#define XZ_KWS_RESAMP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 清空历史（换会话 / 重新武装时调用）。 */
void kws_resamp_reset(void);

/* 送 n 个 8k 样本，写 2n 个 16k 样本到 out（容量至少 2n）。返回写出数量。 */
int kws_resamp_2x(const int16_t *in, int n, int16_t *out);

#ifdef __cplusplus
}
#endif

#endif /* XZ_KWS_RESAMP_H */
