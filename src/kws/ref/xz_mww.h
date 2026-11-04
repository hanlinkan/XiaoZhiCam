#ifndef XZ_MWW_H
#define XZ_MWW_H

#include <cstdint>

/*
 * microWakeWord 相机端定点推理引擎（2026-10-21）。
 *
 * 模型：wakeword_v2，26,049 参数，mixednet（first_conv 32 -> 4x[dw+pointwise]，
 * stride=3），输入 [1,3,40] int8 mel，输出 uint8 概率（0~255）。
 *
 * 为什么手写而不是移植 TFLite Micro：见 out/kws/mww-引擎设计.md
 *   —— 6 种算子、结构固定，手写只要 ~4 KB 状态 + 26 KB 权重，
 *      而且能把 CONCAT/SLICE 直接做成 memcpy。
 *
 * 权重与量化参数由 out/kws/export_engine.py 生成到 mww_model.h：
 *   每层给 int8 权重、int32 bias、每输出通道的 Q31 乘数 + 右移位数。
 *   运行时只做 int32 乘加 + gemmlowp 的两步舍入，没有任何浮点。
 *
 * 流式状态：6 个历史缓冲（mel 2 帧、conv 4 帧、block1 10 帧、block2 14 帧、
 * block3 22 帧、尾部 16 帧），全部静态分配，初值 -128（量化域的"零激活"）。
 * 缓冲是纯移位寄存器，喂满各自长度后与训练时的初值无关 —— 前 0.22 秒的
 * 输出有瞬态，对"要说满 0.5 秒的唤醒词"没有影响。
 */
namespace xz_mww {

/* 清零所有历史缓冲。 */
void Reset();

/* 喂一帧 40 维 int8 mel（10 ms）。
 * 每攒够 3 帧跑一次网络。
 * 返回 -1 表示本次还没出结果；否则返回 0~255 的概率字节。 */
int Feed(const int8_t* mel40);

/* 最近一次概率（0~255），还没跑过时为 0。 */
int LastProbability();

}  // namespace xz_mww

#endif  // XZ_MWW_H
