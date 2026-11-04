/*
 * kws.c -- 唤醒词判决层（8 kHz -> 16 kHz -> mel -> 网络 -> 命中）
 *
 * 移植自 ~/xzfw/port/src/xz_kws_engine.cc（C++），把里面积累的五版踩坑保留下来：
 *
 *   1. 门控必须用**峰值**，不能用帧 RMS。实测一个 20 ms 帧里语音 RMS 只有
 *      155（-46 dBFS），静音 16，但换算成 dB 只差 0.1 dB —— 峰均比 40 dB
 *      把 RMS 平均没了。改峰值后语音 -6 dBFS、静音 -50 dBFS，门控才有意义。
 *   2. 底噪估计要"快降、有条件慢升"：说话时几乎冻结，否则自己的声音会把
 *      底噪顶上去，rel 永远够不到 4 dB（实测卡在 2.6~3.9）。
 *   3. 全零帧不计分：AI 断流时会整帧输出 0，定点 mel+PCAN 把它归一化成
 *      某个固定模式，网络在它上面给 240~254 分（分布外输入）。
 *   4. PCAN 每 1.5 s 重置一次（只在安静时）：训练片段都是 1.5 s 从头开始，
 *      设备连续跑十几秒后 PCAN 会把底噪归一化成"像语音"，输出一路涨到 255。
 *   5. 每次重新武装给 guard：冷启动有一次 255 的瞬态（录音确认当时是完全
 *      平稳的 -37 dBFS 底噪）。
 */
#include "kws.h"

#include <math.h>
#include <string.h>

#include "kws_mel.h"
#include "kws_mww.h"
#include "kws_level.h"
#include "kws_resamp.h"

/* ---------- 常量 ---------- */

#define KWS_FRAME16      160        /* 16 kHz / 10 ms */
#define KWS_BUF16        512        /* 16k 待处理缓冲（>= 一帧 + 一次转换余量） */
#define KWS_SETTLE_FRMS  20         /* 重置后 200 ms 不计分 */
#define KWS_QUIET_RUN    3          /* 连续 3 帧安静才算真安静（120 ms） */
#define KWS_PCAN_PERIOD  150        /* 1.5 s = 训练片段长度 */
#define KWS_QUIET_REL10  30         /* rel < 3.0 dB 视为安静 */
/* 电气/拼接瞬态：实测抽头里偶尔出现"2 个样本内 ±50 跳到 ±20000 并削波"的尖峰
 * （帧被抢导致的拼接缝、或 codec 电气毛刺），安静段的正常峰值只有 ±5000。
 * 这种尖峰过后电平归一化的增益会从 0 dB 缓慢爬回 +36 dB，**爬升的斜坡上
 * 模型会误报**：真机 178 s 里 3 次尖峰 → 3 次误唤醒，全部在尖峰后 1.8~2.0 s。
 * 处理：判定为瞬态就丢掉整个前端状态并静默 2.5 s（等增益重新收敛）。 */
#define KWS_TRANSIENT_ABS    12000
#define KWS_TRANSIENT_GUARD  250    /* 250 帧 = 2.5 s */
/* 门控 hangover：模型输出依赖过去 ~480 ms（16 帧 64 通道的历史），
 * 所以它"认定是词"的那一帧，声学峰值往往已经过去，逐帧门控会把它挡掉。
 * 实测：real07 片段 hot=8、gated=0，同一时刻 rel 从 5.9 dB 掉到门限以下。
 * 释放时间取 500 ms，与模型上下文长度同量级。 */
#define KWS_GATE_HOLD    50         /* 500 ms = 50 帧 */
#define KWS_HIT_COOLDOWN 1500       /* ms，一次发声只算一次 */
#define KWS_CONFIRM_WIN  8          /* 连续 ≥阈值 的决策窗口数（10ms/窗 = 80ms 持续）
                                     * 才确认命中：真词持续 300-800ms，开口瞬态
                                     * 只有 1-5 窗。官方 README 两阶段检测的第二阶段。 */
#define KWS_GUARD_MS     500        /* 每次武装的防抖 */
#define KWS_GUARD_BOOT   6000       /* 上电后第一次武装的防抖 */
/* 电平归一化：模型对绝对电平敏感（PCAN 的动态范围有限）。
 * 实测同一段录音：滤波后原始电平（-52 dBFS RMS）模型只给 105（等于静音），
 * +30 dB（-23 dBFS RMS）才给 220 并命中。所以必须把语音 RMS 拉到训练域附近。
 * 用 RMS 而不是峰值：训练样本是峰值归一化到 -20 dBFS 的，fir 滤波后
 * 峰均比很大，按峰值归一化反而拉不上去。 */


/* ---------- 状态 ---------- */

static kws_cfg_t s_cfg;
static int s_inited;

/* 16k 待处理 */
static int16_t s_buf16[KWS_BUF16];
static int s_pend;

/* 底噪 / 门控 */
static float s_floor_db;
static int s_floor_ready;
static int s_floor_frames;
static int s_high_run;
static int s_rel_db10;
static int s_quiet_run;
static int s_gate_hold;

/* 计时（10 ms 为单位，从 mel 帧推进，不依赖系统时钟） */
static uint32_t s_t_ms;
static int s_since_reset;      /* 距离上次 PCAN 重置的帧数 */
static int s_settle;
static int s_guard;

/* 命中 */
static uint32_t s_last_hit_ms;
static int s_hits;

/* 诊断 */
static int s_prob;
static int s_prob_peak;
static int s_hot, s_hot_gated, s_zero_hot;
static int s_mel_frames;
static int s_transients;
static int s_confirm;      /* 连续 ≥阈值 的决策窗口计数 */
static int s_hot_total, s_gated_total, s_zero_total;
static int s_resets;

/* ---------- 小工具 ---------- */

static int clamp16(int v)
{
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return v;
}

/* 峰值 -> dBFS。软浮点在 10 ms 节奏下可忽略（约 30 个周期/次）。 */
static float peak_db(int peak)
{
    if (peak <= 0) return -90.0f;
    return 20.0f * log10f((float)peak / 32768.0f + 1e-9f);
}

/* ---------- 底噪 / 门控 ---------- */

/* 返回 1 表示本帧通过门控；同时把是否全零写进 *nonzero。
 *
 * 底噪跟踪（上一版第五版，四版都错过）：
 *   · step < 0（比底噪低）：每帧最多降 0.5 dB，跟得动环境；
 *   · step >= 1 dB（很可能在说话）：每帧最多升 0.002 dB（≈50 s 才 1 dB），
 *     基本冻结，保证一句话里底噪不会被自己顶上去；
 *   · 0 < step < 1 dB（接近底噪）：每帧最多升 0.02 dB，用来跟上房间变化；
 *   · 如果连续 300 帧（3 s）都"明显高于底噪"，那是底噪估错了，
 *     允许 0.1 dB/帧 快速恢复。 */
static int gate_frame(const int16_t *p, int *nonzero)
{
    int i, peak = 0, any = 0;
    float db, step, rel;

    for (i = 0; i < KWS_FRAME16; i++) {
        int v = p[i];
        int a;
        if (v != 0) any = 1;
        a = v < 0 ? -v : v;
        if (a > peak) peak = a;
    }
    *nonzero = any;

    db = peak_db(peak);

    if (!s_floor_ready) {
        if (s_floor_frames == 0) {
            s_floor_db = db;
            s_floor_frames = 1;
            return 1;
        }
        if (db < s_floor_db) s_floor_db = db;
        if (++s_floor_frames < 30) return 1;   /* 前 300 ms：建立期，放行 */
        s_floor_ready = 1;
        return 1;
    }

    step = db - s_floor_db;
    if (step < 0.0f) {
        s_floor_db += (step < -0.5f) ? -0.5f : step;
        s_high_run = 0;
    } else if (step > 0.0f) {
        if (step >= 1.0f) {
            if (++s_high_run > 300) {
                s_floor_db += (step > 0.1f) ? 0.1f : step;
            } else {
                s_floor_db += (step > 0.002f) ? 0.002f : step;
            }
        } else {
            s_high_run = 0;
            s_floor_db += (step > 0.02f) ? 0.02f : step;
        }
    }

    rel = db - s_floor_db;
    s_rel_db10 = (int)(rel * 10.0f);

    if (s_cfg.gate_db10 == 0) return 1;   /* 0 = 关闭门控（调试用） */

    if (s_rel_db10 >= s_cfg.gate_db10) {
        s_gate_hold = KWS_GATE_HOLD;      /* 电平够 → 打开并续期 */
        return 1;
    }
    if (s_gate_hold > 0) {                /* 余量期内保持打开 */
        s_gate_hold--;
        return 1;
    }
    return 0;
}

/* ---------- 主处理 ---------- */

/* 处理 16k 缓冲里的整帧。返回 1 表示命中。 */
static int process_frames(void)
{
    int pos = 0, hit = 0;
    int8_t mel[40];

    while (s_pend - pos >= KWS_FRAME16) {
        const int16_t *p = s_buf16 + pos;
        int nonzero = 1;
        int open;

        if (++s_since_reset > 100000) s_since_reset = 100000;
        s_t_ms += 10;

        open = gate_frame(p, &nonzero);

        /* 安静时每 1.5 s 重置一次 PCAN（把前端拉回训练域） */
        if (s_rel_db10 < KWS_QUIET_REL10) {
            if (++s_quiet_run >= KWS_QUIET_RUN && s_since_reset >= KWS_PCAN_PERIOD) {
                kws_mel_reset();
                s_since_reset = 0;
                s_settle = KWS_SETTLE_FRMS;
                s_resets++;
            }
        } else {
            s_quiet_run = 0;
            s_since_reset = 0;      /* 有人在说话：别在词中间重置 */
        }

        if (s_guard > 0) s_guard--;
        if (s_settle > 0) s_settle--;

        /* 零帧也要送 mel（10 ms STFT 需要连续），只是分数不计入判定 */
        if (kws_mel_process(p, mel)) {
            int prob;
            s_mel_frames++;
            prob = kws_mww_feed(mel);
            if (prob >= 0) {
                s_prob = prob;
                if (prob > s_prob_peak) s_prob_peak = prob;

                if (prob >= s_cfg.threshold) {
                    s_confirm++;
                    if (!nonzero) {
                        s_zero_hot++;
                        s_zero_total++;
                    } else {
                        s_hot++;
                        s_hot_total++;
                        if (open) {
                            s_hot_gated++;
                            s_gated_total++;
                        }
                    }
                } else {
                    s_confirm = 0;
                }

                if (nonzero && open && prob >= s_cfg.threshold &&
                    s_confirm >= KWS_CONFIRM_WIN &&
                    s_guard == 0 && s_settle == 0 &&
                    (s_last_hit_ms == 0 || s_t_ms - s_last_hit_ms > KWS_HIT_COOLDOWN)) {
                    s_last_hit_ms = s_t_ms;
                    s_hits++;
                    s_confirm = 0;
                    hit = 1;
                }
            }
        }

        pos += KWS_FRAME16;
    }

    if (pos > 0) {
        int left = s_pend - pos;
        if (left > 0) {
            memmove(s_buf16, s_buf16 + pos, sizeof(int16_t) * (size_t)left);
        }
        s_pend = left;
    }
    return hit;
}

/* ---------- 对外接口 ---------- */

int kws_init(const kws_cfg_t *cfg)
{
    if (cfg != NULL) {
        s_cfg = *cfg;
    } else {
        s_cfg.threshold = KWS_DEF_THRESHOLD;
        s_cfg.gain_db = KWS_DEF_GAIN_DB;
        s_cfg.gate_db10 = KWS_DEF_GATE_DB10;
    }
    if (!kws_mel_init()) {
        return 0;
    }
    kws_reset();
    s_inited = 1;
    return 1;
}

void kws_set_params(int threshold, int gain_db, int gate_db10)
{
    s_cfg.threshold = threshold;
    s_cfg.gain_db = gain_db;
    s_cfg.gate_db10 = gate_db10;
}

/* 只重置判决状态，保留音频前端（去工频 / 电平均衡 / mel / 网络历史）。
 *
 * 为什么必须区分：前端有自适应状态，冷启动后要约 1.5 s 才收敛，收敛前
 * 的特征分布和稳态完全不同。实测 v9 模型在每个冷启动处都会报一次高分
 * （19 个独立底噪文件各命中 1 次；同一批文件前置 2 s 底噪后全部 0 命中）。
 * 所以"重新武装"绝不能碰前端，否则每说完一句话就要误唤醒一次。 */
static void reset_decision(void)
{
    s_floor_db = 0.0f;
    s_floor_ready = 0;
    s_floor_frames = 0;
    s_high_run = 0;
    s_rel_db10 = 0;
    s_quiet_run = 0;
    s_gate_hold = 0;

    s_t_ms = 0;
    s_since_reset = 0;
    s_settle = 0;
    s_guard = 0;

    s_last_hit_ms = 0;
    s_prob = 0;
    s_prob_peak = 0;
    s_hot = s_hot_gated = s_zero_hot = 0;
    s_hot_total = s_gated_total = s_zero_total = 0;
    s_resets = 0;
}

/* 完整复位（含音频前端）。只在初始化和离线评估时用。 */
void kws_reset(void)
{
    s_confirm = 0;
    kws_level_reset();
    kws_resamp_reset();
    s_pend = 0;
    if (s_inited) {
        kws_mel_reset();
        kws_mww_reset();
    }
    reset_decision();
}

void kws_arm(int cold_start)
{
    reset_decision();          /* 不动前端：见 reset_decision 的说明 */
    s_guard = (cold_start ? KWS_GUARD_BOOT : KWS_GUARD_MS) / 10;
}

void kws_disarm(void)
{
    s_prob_peak = 0;
    s_hot = s_hot_gated = s_zero_hot = 0;
}

int kws_feed8k(const int16_t *pcm8k, int n)
{
    int hit = 0;
    int gain = 256;
    int i;

    if (!s_inited || pcm8k == NULL || n <= 0) {
        return 0;
    }

    /* 先查瞬态：有就直接重置前端并静默一段时间（见 KWS_TRANSIENT_ABS 的说明） */
    for (i = 0; i < n; i++) {
        int v = pcm8k[i];
        if (v > KWS_TRANSIENT_ABS || v < -KWS_TRANSIENT_ABS) {
            kws_reset();                 /* 前端 + 网络历史全丢，别让脏状态残留 */
            s_guard = KWS_TRANSIENT_GUARD;
            s_transients++;
            return 0;
        }
    }

    while (n > 0) {
        int16_t tmp8[320];          /* 40 ms @8k，设备的采集帧长 */
        int16_t tmp16[2 * 320];
        int chunk = (n > 320) ? 320 : n;
        int i, m;

        /* 电平归一化：每段（40 ms）更新一次估计，再叠加手动增益（调试用） */
        gain = kws_level_gain_q8(pcm8k, chunk);
        if (s_cfg.gain_db != 0) {
            int g = (int)(powf(10.0f, (float)s_cfg.gain_db / 20.0f) * 256.0f + 0.5f);
            gain = (gain * g) >> 8;
            if (gain > KWS_LEVEL_GAIN_MAX) gain = KWS_LEVEL_GAIN_MAX;
        }

        for (i = 0; i < chunk; i++) {
            tmp8[i] = (int16_t)clamp16(((int32_t)pcm8k[i] * gain) >> 8);
        }

        m = kws_resamp_2x(tmp8, chunk, tmp16);

        for (i = 0; i < m; ) {
            int room = KWS_BUF16 - s_pend;
            int take = (m - i < room) ? (m - i) : room;
            memcpy(s_buf16 + s_pend, tmp16 + i, sizeof(int16_t) * (size_t)take);
            s_pend += take;
            i += take;
            if (process_frames()) {
                hit = 1;            /* 一次发声只报一次，剩余样本继续喂 */
            }
        }

        pcm8k += chunk;
        n -= chunk;
    }
    return hit;
}

void kws_diag(kws_diag_t *out)
{
    if (out == NULL) return;
    out->prob = s_prob;
    out->prob_peak = s_prob_peak;
    out->hot = s_hot;
    out->hot_gated = s_hot_gated;
    out->zero_hot = s_zero_hot;
    out->resets = s_resets;
    out->rel_db10 = s_rel_db10;
    out->floor_db10 = (int)(s_floor_db * 10.0f);
    out->hits = s_hits;
    out->hot_total = s_hot_total;
    out->gated_total = s_gated_total;
    out->zero_total = s_zero_total;
    out->in_rms = kws_level_in_rms();
    out->mel_frames = s_mel_frames;
    s_mel_frames = 0;
    out->transients = s_transients;
    s_transients = 0;

    s_prob_peak = 0;
    s_hot = s_hot_gated = s_zero_hot = 0;
}
