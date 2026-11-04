#ifndef XZ_MEL_H
#define XZ_MEL_H

#include <cstdint>

/*
 * 16 kHz / 40 维 mel 前端 —— 与训练侧 pymicro-features 逐位同源。
 *
 * 为什么是 vendor 的代码而不是自己写
 * ----------------------------------
 * microWakeWord 的特征前端是 TFLite Micro 的 audio microfrontend：
 *   - 30 ms 汉明窗 / 10 ms 步进 / 512 点定点 FFT / 40 mel（125~7500 Hz）
 *   - 噪声抑制（even/odd smoothing）
 *   - PCAN 自动增益（strength 0.95, offset 80, gain_bits 21）
 *   - 定点 log（scale_shift 6）
 * 这些定点细节（尤其是 PCAN 和 log 的量化）差一个 LSB，模型的输入分布就变了。
 * 所以直接把 pymicro-features 2.0.2 里 vendor 的 TFLM 源码编进来，
 * 用**完全相同的配置**，而不是重写一份"看起来一样"的实现。
 *
 * 源码位置：firmware/port/third_party/microfrontend（Apache-2.0，来自
 * pymicro-features 2.0.2 sdist，上游是 tensorflow/tflite-micro）。
 *
 * 输出：int8，量化参数 scale=0.10196079 / zp=-128，与模型输入张量一致。
 */
namespace xz_mel {

/* 初始化（内部会 malloc 滤波器组/FFT 表，约 20 KB）。返回 1 成功。 */
int Init();

/* 清空流式状态（换会话/重新开始检测时调用）。 */
void Reset();

/* 喂 160 个 16 kHz 样本（10 ms），产出一帧 40 维 int8 特征。
 * 返回 1 表示产出了特征，0 表示还没攒够。 */
int Process10ms(const int16_t* pcm160, int8_t out40[40]);

}  // namespace xz_mel

#endif  // XZ_MEL_H
