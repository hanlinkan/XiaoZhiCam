/*
 * kws_mww.h -- microWakeWord v2 定点推理引擎（C 版）
 *
 * 模型：26,049 参数，mixednet（first_conv 32 -> 4x[dw+pointwise], stride=3），
 *       输入 [1,3,40] int8 mel，输出 uint8 概率（0~255）。
 * 权重：mww_model.h（由 tflite 导出，含每通道 Q31 乘数 + 右移位数）。
 *
 * 数据流（与 tflite 逐算子对应）：
 *   mel[3x40] --concat(hist2)--> [5x40] --conv 32@5x40--> [32]
 *     --concat(hist4)--> [5x32] --dw 5--> [32] --conv 64@1x1--> [64]
 *     --concat(hist10)--> [11x64] --split--> 低32 取后7帧 --dw 7--> [32]
 *                                     \--> 高32 取后11帧 --dw 11-> [32]
 *     --concat--> [64] --conv 64@1x1--> [64]
 *     --concat(hist14)--> [15x64] --split--> 低32 取后9帧 --dw 9--> [32]
 *                                     \--> 高32 取后15帧 --dw 15-> [32]
 *     --concat--> [64] --conv 64@1x1--> [64]
 *     --concat(hist22)--> [23x64] --dw 23--> [64] --conv 64@1x1--> [64]
 *     --concat(hist16)--> [17x64] --flatten 1088--> FC --> [1] --logistic--> 概率
 *
 * 两个实现要点（TFLite 里踩过）：
 *   1. requantize 用 gemmlowp 的两步舍入：SaturatingRoundingDoublingHighMul
 *      后 RoundingDivideByPOT。少一步就差 1 LSB，经 sigmoid 放大成 12 个概率 LSB。
 *   2. 权重是 per-channel 量化，乘数必须逐输出通道取，不能取均值。
 *
 * 静态状态约 4.2 KB，热路径零分配、零浮点。
 */
#ifndef XZ_KWS_MWW_H
#define XZ_KWS_MWW_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 清零所有历史缓冲（初值 -128 = 量化域的"零激活"）。 */
void kws_mww_reset(void);

/* 喂一帧 40 维 int8 mel（10 ms）。每攒够 3 帧跑一次网络。
 * 返回 -1 表示本次还没出结果；否则返回 0~255 的概率。 */
int kws_mww_feed(const int8_t mel40[40]);

/* 最近一次概率（0~255），还没跑过时为 0。 */
int kws_mww_last_prob(void);

#ifdef __cplusplus
}
#endif

#endif /* XZ_KWS_MWW_H */
