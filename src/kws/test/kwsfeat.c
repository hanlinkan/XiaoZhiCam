/*
 * kwsfeat.c -- "部署链路"特征提取器（训练侧用，不参与固件）
 *
 * 存在的唯一理由：让训练和部署走**同一条**信号链。
 * 顺序与固件完全一致：
 *
 *   AI 8k 原始帧 -> afe（去工频 + HPF200 + LPF3k）-> 取 AGC/噪声门之前的 tmp
 *                -> 电平归一化（kws_level）-> 8k->16k 半带 -> 40 维 mel/10 ms
 *
 * 背景：同一个「你好小智」，走训练链路（16k 原始 -> pymicro）模型给 255，
 * 走部署链路只给 105（等于静音）。模型不是不认识词，是不认识这条链。
 * 所以训练特征必须由这里产生。
 *
 * 编译成 .so，由 test/mww_deploy_features.py 用 ctypes 调用。
 */
#include <string.h>

#include "afe.h"
#include "kws_level.h"
#include "kws_mel.h"
#include "kws_resamp.h"

#define FEAT_DIM 40
#define BUF16_MAX 512

static afe_t s_afe;
static int16_t s_buf16[BUF16_MAX];
static int s_pend;

static int16_t sat16(int v)
{
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

void kwsfeat_reset(void)
{
    kws_mel_init();          /* 幂等：分配滤波器组/FFT 表 */
    afe_reset(&s_afe);
    kws_level_reset();
    kws_resamp_reset();
    kws_mel_reset();
    s_pend = 0;
}

/* 送 n 个 8k 样本，把产生的 mel 帧写进 out（每帧 40 字节），返回帧数。
 * max_frames 是 out 的容量（帧）。 */
int kwsfeat_process(const int16_t *pcm8k, int n, int8_t *out, int max_frames)
{
    int frames = 0;

    while (n > 0) {
        int16_t afe_out[AFE_MAX_FRAME];
        int16_t gained[320];
        int16_t up[640];
        int chunk = (n > 320) ? 320 : n;
        int i, m, gain;

        afe_process(&s_afe, pcm8k, afe_out, chunk);

        gain = kws_level_gain_q8(s_afe.tmp, chunk);
        for (i = 0; i < chunk; i++) {
            gained[i] = sat16(((int32_t)s_afe.tmp[i] * gain) >> 8);
        }

        m = kws_resamp_2x(gained, chunk, up);

        for (i = 0; i < m; ) {
            int room = BUF16_MAX - s_pend;
            int take = (m - i < room) ? (m - i) : room;
            memcpy(s_buf16 + s_pend, up + i, sizeof(int16_t) * (size_t)take);
            s_pend += take;
            i += take;

            while (s_pend >= 160) {
                int8_t frame[FEAT_DIM];
                /* 注意：mel 必须每 160 点喂一次（它内部有 30 ms 窗），
                 * 不能因为"输出缓冲满了"就跳过，否则前端状态会错位。 */
                if (kws_mel_process(s_buf16, frame)) {
                    if (frames >= max_frames) {
                        return -1;      /* 调用方给的空间不够，直接报错 */
                    }
                    memcpy(out + frames * FEAT_DIM, frame, FEAT_DIM);
                    frames++;
                }
                memmove(s_buf16, s_buf16 + 160,
                        sizeof(int16_t) * (size_t)(s_pend - 160));
                s_pend -= 160;
            }
        }

        pcm8k += chunk;
        n -= chunk;
    }
    return frames;
}
