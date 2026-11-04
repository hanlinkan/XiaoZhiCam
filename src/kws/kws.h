/*
 * kws.h -- 唤醒词判决层（8 kHz PCM 进，命中事件出）
 *
 * 分工：
 *   kws_mel.c  16 kHz -> 40 维 mel（与训练逐位同源）
 *   kws_mww.c  40 维 mel -> 概率
 *   kws.c      **本文件**：8k->16k 重采样、固定增益、静音门控、PCAN 周期、
 *              guard/settle/cooldown、阈值判决。也就是"什么时候算喊到了"。
 *
 * 这一层的每个常数都来自上一版的上机实测（见 docs/04-wakeword-plan.md §5.5）：
 *   - 阈值 190/255：噪声簇最高 151，真词最低 220
 *   - 增益 +28 dB：真实语音从 105 升到 172~253
 *   - 门控用**峰值**相对底噪 4 dB：20 ms 帧上语音与静音的 RMS 只差 0.1 dB
 *   - 全零帧不计分：模型在数字零帧上给 240~254 分（OOD）
 *   - PCAN 每 1.5 s 重置：不重置时 3999 帧里 3916 帧饱和
 *
 * 全部静态分配；热路径每 10 ms 一次，无锁、无 malloc。
 */
#ifndef XZ_KWS_H
#define XZ_KWS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 默认参数（可用 kws_init 覆盖） */
/* 阈值 240（2026-10-31 真机验证后上调）：
 *   离线 48 条真人唤词：210 时 45/48，240 时 44/48（只掉 1 条）
 *   旧负样本 321 s：210 时 2 次误触，240 时 0 次
 *   留出的 240 s 真机底噪：两者都 0（峰值 182，余量 58 分）
 *   真机实测唤词（143 s 录音）：210 与 240 都命中 2 次，一个没少
 * 曲线在 240~250 之间陡降（250 掉到 41/48），240 是拐点。 */
#define KWS_DEF_THRESHOLD   240   /* 0~255 */
/* 手动增益默认 0：电平已由 kws_level 自动归一化到 -23 dBFS，
   这里再叠 28 dB 会把正常说话推到削顶（v9 训练时用的就是 0 dB）。 */
#define KWS_DEF_GAIN_DB     0
#define KWS_DEF_GATE_DB10   40    /* 4.0 dB，单位 0.1 dB */

typedef struct {
    int threshold;    /* 模型分门限 0~255 */
    int gain_db;      /* 送 mel 之前的固定增益 */
    int gate_db10;    /* 峰值相对底噪的门限，0.1 dB 为单位；0 = 关闭门控 */
} kws_cfg_t;

typedef struct {
    int prob;         /* 最近一次网络输出 */
    int prob_peak;    /* 自上次 kws_diag 以来的峰值（读后清零） */
    int hot;          /* 达门限的帧数（读后清零） */
    int hot_gated;    /* 其中通过门控的帧数（读后清零） */
    int zero_hot;     /* 全零帧达门限的帧数（读后清零） */
    int resets;       /* PCAN 重置累计次数 */
    int rel_db10;     /* 最近一帧的 峰值-底噪，0.1 dB */
    int floor_db10;   /* 当前底噪估计，0.1 dB */
    int hits;         /* 命中累计 */
    int hot_total;    /* 达门限的帧数（累计） */
    int gated_total;  /* 其中通过门控的帧数（累计） */
    int zero_total;   /* 全零帧达门限的帧数（累计） */
    int in_rms;       /* 最近一段输入的 RMS（加增益之前） */
    int mel_frames;   /* 自上次 diag 以来成功产出的 mel 帧数（读后清零） */
    int transients;   /* 自上次 diag 以来被判定为电气瞬态的次数（读后清零） */
} kws_diag_t;

/* 初始化（一次性）：mel 表 + 网络状态。返回 1 成功。cfg 可为 NULL 用默认值。 */
int kws_init(const kws_cfg_t *cfg);

/* 改判决参数（运行时调参用，不影响已建立的底噪）。 */
void kws_set_params(int threshold, int gain_db, int gate_db10);

/* 彻底复位：前端（含 PCAN）+ 网络历史 + 底噪估计 + 计时。 */
void kws_reset(void);

/* 重新武装：复位 + 给一段 guard（两次唤醒之间防抖）。
 * cold_start=1 时给 6 s（实测上电冷启动有一次 255 的瞬态），
 * 之后每次重新武装给 500 ms。 */
void kws_arm(int cold_start);

/* 暂停检测（进入录音/播放时调用），只清统计，不重置前端。 */
void kws_disarm(void);

/* 喂 8 kHz 单声道 PCM（任意长度，设备上是 320 点 / 40 ms）。
 * 返回 1 表示本次命中唤醒词（一次发声只报一次）。 */
int kws_feed8k(const int16_t *pcm8k, int n);

/* 诊断快照（读后把本秒计数清零）。 */
void kws_diag(kws_diag_t *out);

#ifdef __cplusplus
}
#endif

#endif /* XZ_KWS_H */
