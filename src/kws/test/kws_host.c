/*
 * kws_host.c -- 唤醒词链路主机端工具（纯 C，与固件同一份代码）
 *
 * 用途：把「8k/16k PCM -> mel -> 网络 -> 概率」跑在 PC 上，与 C++/TFLite
 *       参考逐位比对。设备端不需要这个文件。
 *
 * 用法：
 *   kws_host feat   <in.wav> <out.feat>      只跑 mel，落盘 T*40 个 int8
 *   kws_host prob   <in.feat> <out.prob>     只跑网络，落盘 T/3 个 uint8
 *   kws_host all    <in.wav> <prefix>        两步都跑 -> prefix.feat/.prob
 *   kws_host stream <in.wav> <out.prob> [N]  连续流，每 N 帧重置一次 PCAN（N=0 不重置）
 *   kws_host cmp    <a> <b> [limit]          逐字节比较（limit 默认 0）
 *
 * WAV 只接受 16 kHz / 16 bit / 单声道（参考数据就是这种）；不接受就报错，
 * 不做重采样 —— 避免"两边都错但错得一样"。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#include "afe.h"          /* 设备上的音频前端：去工频 + HPF/LPF（KWS 取 AGC 前抽头） */
#include "kws.h"
#include "kws_mel.h"
#include "kws_mww.h"
#include "kws_resamp.h"

#define FEAT_DIM 40

static int read_file(const char *path, unsigned char **out, size_t *out_n)
{
    FILE *f = fopen(path, "rb");
    long n;
    size_t got;

    if (f == NULL) return 0;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return 0; }

    *out = (unsigned char *)malloc((size_t)n ? (size_t)n : 1);
    if (*out == NULL) { fclose(f); return 0; }
    got = fread(*out, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) { free(*out); *out = NULL; return 0; }
    *out_n = got;
    return 1;
}

static int write_file(const char *path, const void *data, size_t n)
{
    FILE *f = fopen(path, "wb");
    size_t w;
    if (f == NULL) return 0;
    w = fwrite(data, 1, n, f);
    fclose(f);
    return w == n;
}

/* 极简 WAV 解析：只找 data 块，校验 16 bit / 单声道 / 16 kHz */
static int read_wav(const char *path, int16_t **pcm, size_t *samples, uint32_t want_rate)
{
    unsigned char *b = NULL;
    size_t n = 0, pos;
    int ok = 0;

    if (!read_file(path, &b, &n)) return 0;
    if (n < 44 || memcmp(b, "RIFF", 4) != 0 || memcmp(b + 8, "WAVE", 4) != 0) {
        free(b);
        return 0;
    }

    pos = 12;
    while (pos + 8 <= n) {
        const char *id = (const char *)b + pos;
        uint32_t sz = 0;
        memcpy(&sz, b + pos + 4, 4);

        if (memcmp(id, "fmt ", 4) == 0 && pos + 8 + 16 <= n) {
            uint16_t fmt, ch;
            uint32_t rate;
            uint16_t bits;
            memcpy(&fmt, b + pos + 8, 2);
            memcpy(&ch, b + pos + 10, 2);
            memcpy(&rate, b + pos + 12, 4);
            memcpy(&bits, b + pos + 22, 2);
            if (fmt != 1 || ch != 1 || rate != want_rate || bits != 16) {
                fprintf(stderr, "WAV 格式不符（需要 PCM/单声道/%uk/16bit）：fmt=%u ch=%u rate=%u bits=%u\n",
                        want_rate / 1000, fmt, ch, rate, bits);
                free(b);
                return 0;
            }
        }

        if (memcmp(id, "data", 4) == 0) {
            size_t bytes = sz;
            if (pos + 8 + bytes > n) bytes = n - (pos + 8);
            *samples = bytes / 2;
            *pcm = (int16_t *)malloc(*samples * sizeof(int16_t));
            if (*pcm == NULL) { free(b); return 0; }
            memcpy(*pcm, b + pos + 8, *samples * sizeof(int16_t));
            ok = 1;
            break;
        }
        pos += 8 + sz + (sz & 1);
    }

    free(b);
    return ok;
}

static int cmd_feat(const char *wav, const char *out)
{
    int16_t *pcm = NULL;
    size_t ns = 0, i;
    int8_t f40[FEAT_DIM];
    unsigned char *feats = NULL;
    size_t nf = 0, cap = 0;
    int rc = 0;

    if (!read_wav(wav, &pcm, &ns, 16000)) return 2;
    if (!kws_mel_init()) { fprintf(stderr, "mel 初始化失败\n"); free(pcm); return 3; }

    cap = (ns / 160 + 1) * FEAT_DIM;
    feats = (unsigned char *)malloc(cap);
    if (feats == NULL) { free(pcm); return 4; }

    for (i = 0; i + 160 <= ns; i += 160) {
        if (kws_mel_process(&pcm[i], f40)) {
            memcpy(feats + nf, f40, FEAT_DIM);
            nf += FEAT_DIM;
        }
    }

    if (!write_file(out, feats, nf)) rc = 4;
    else printf("feat: %zu 帧 -> %s\n", nf / FEAT_DIM, out);

    free(feats);
    free(pcm);
    return rc;
}

static int cmd_prob(const char *feat, const char *out)
{
    unsigned char *b = NULL;
    size_t n = 0, i;
    unsigned char *probs = NULL;
    size_t np = 0;
    int rc = 0;

    if (!read_file(feat, &b, &n)) { fprintf(stderr, "读特征失败: %s\n", feat); return 2; }
    if (n % FEAT_DIM != 0) { fprintf(stderr, "特征长度不是 40 的整数倍: %zu\n", n); free(b); return 3; }

    kws_mww_reset();
    probs = (unsigned char *)malloc(n / FEAT_DIM + 1);
    if (probs == NULL) { free(b); return 4; }

    for (i = 0; i < n / FEAT_DIM; i++) {
        int p = kws_mww_feed((const int8_t *)b + i * FEAT_DIM);
        if (p >= 0) probs[np++] = (unsigned char)p;
    }

    if (!write_file(out, probs, np)) rc = 4;
    else printf("prob: %zu 个 -> %s（特征 %zu 帧）\n", np, out, n / FEAT_DIM);

    free(probs);
    free(b);
    return rc;
}

/* 连续流：每 N 个 10ms 帧重置一次 mel（含 PCAN），网络历史保留。
 * gain_db 只是诊断用：固件里这一级在判决层（kws 引擎），放在这里是为了
 * 在 PC 上就能复现"加增益后能不能唤醒"，不用反复上机。 */
static int cmd_stream(const char *wav, const char *out, int reset_frames, int gain_db)
{
    int16_t *pcm = NULL;
    size_t ns = 0, i;
    int8_t f40[FEAT_DIM];
    unsigned char *probs = NULL;
    size_t np = 0;
    int frame = 0, rc = 0;
    int gain_q8 = 256;
    int16_t *gained = NULL;

    if (!read_wav(wav, &pcm, &ns, 16000)) return 2;
    if (!kws_mel_init()) { fprintf(stderr, "mel 初始化失败\n"); free(pcm); return 3; }
    kws_mww_reset();

    if (gain_db != 0) {
        /* Q8 定点增益 + 饱和，避免在主机和 ARM 上得到不同的舍入 */
        double g = pow(10.0, gain_db / 20.0) * 256.0;
        gain_q8 = (int)(g + 0.5);
        if (gain_q8 < 1) gain_q8 = 1;
        gained = (int16_t *)malloc(ns * sizeof(int16_t));
        if (gained == NULL) { free(pcm); return 4; }
        for (i = 0; i < ns; i++) {
            int32_t v = ((int32_t)pcm[i] * gain_q8) >> 8;
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            gained[i] = (int16_t)v;
        }
        printf("stream: 前置增益 %+d dB (Q8=%d)\n", gain_db, gain_q8);
    }

    probs = (unsigned char *)malloc(ns / 160 + 1);
    if (probs == NULL) { free(pcm); free(gained); return 4; }

    for (i = 0; i + 160 <= ns; i += 160, frame++) {
        const int16_t *src = gained ? &gained[i] : &pcm[i];
        int p;
        if (reset_frames > 0 && frame > 0 && (frame % reset_frames) == 0) {
            kws_mel_reset();
        }
        if (!kws_mel_process(src, f40)) continue;
        p = kws_mww_feed(f40);
        if (p >= 0) probs[np++] = (unsigned char)p;
    }

    if (!write_file(out, probs, np)) rc = 4;
    else printf("stream: %zu 帧概率 -> %s（输入 %d 帧）\n", np, out, frame);

    free(probs);
    free(pcm);
    free(gained);
    return rc;
}

/* 8k -> 16k：只跑重采样，落盘 s16le，用来和 scipy 的 float 版本比 SNR */
static int cmd_resamp(const char *wav, const char *out)
{
    int16_t *pcm = NULL;
    size_t ns = 0, i;
    unsigned char *out16 = NULL;
    size_t n16 = 0;

    if (!read_wav(wav, &pcm, &ns, 8000)) return 2;

    out16 = (unsigned char *)malloc(ns * 4 + 4);
    if (out16 == NULL) { free(pcm); return 4; }

    kws_resamp_reset();
    for (i = 0; i + 320 <= ns; i += 320) {
        int16_t buf[640];
        int m = kws_resamp_2x(&pcm[i], 320, buf);
        memcpy(out16 + n16, buf, (size_t)m * sizeof(int16_t));
        n16 += (size_t)m * sizeof(int16_t);
    }

    if (!write_file(out, out16, n16)) {
        free(out16); free(pcm);
        return 4;
    }
    printf("resamp: %zu 样本 -> %s（%zu 字节，%.2f s @16k）\n",
           n16 / 2, out, n16, (double)(n16 / 2) / 16000.0);
    free(out16);
    free(pcm);
    return 0;
}

/* 完整判决层：8k WAV -> 引擎 -> 命中次数。参数可覆盖，用于离线扫参。 */
static afe_t g_afe_kws;
static int16_t g_afe_out[AFE_MAX_FRAME];

static int cmd_kws(const char *wav, int thr, int gain, int gate10, int verbose, int use_afe,
                   int no_guard, int rearm_s)
{
    int16_t *pcm = NULL;
    size_t ns = 0, i;
    int hits = 0, peak = 0;
    kws_cfg_t cfg;
    kws_diag_t d;

    if (!read_wav(wav, &pcm, &ns, 8000)) return 2;

    cfg.threshold = thr;
    cfg.gain_db = gain;
    cfg.gate_db10 = gate10;
    if (!kws_init(&cfg)) { fprintf(stderr, "kws 初始化失败\n"); free(pcm); return 3; }

    if (no_guard) kws_reset();   /* 评估短片段时不要 guard，否则词头被吃掉 */
    else kws_arm(0);             /* 主机测试不走 6 s 冷启动防抖 */

    if (use_afe) {
        /* 设备上的顺序：AI 采集 -> afe(去工频/滤波) -> 取 AGC 前的 tmp -> KWS。
         * 这一段必须模拟，否则 50 Hz 干扰的峰值会盖住语音，门控根本没法定。 */
        afe_reset(&g_afe_kws);
        if (getenv("KWS_NO_DEHUM")) g_afe_kws.dehum_enable = 0;
    }

    for (i = 0; i + 320 <= ns; i += 320) {
        const int16_t *in = &pcm[i];
        if (use_afe) {
            afe_process(&g_afe_kws, &pcm[i], g_afe_out, 320);
            in = g_afe_kws.tmp;      /* AGC / 噪声门之前 */
        }
        /* 模拟设备上的"重新武装"：验证前端状态是否被正确保留 */
        if (rearm_s > 0 && i >= 320 && (i / 8000) != ((i - 320) / 8000) &&
            (i / 8000) % rearm_s == 0) {
            kws_arm(0);
            if (verbose) printf("  [rearm @ %ds]\n", (int)(i / 8000));
        }
        if (kws_feed8k(in, 320)) {
            int t = (int)(i / 8000);
            hits++;
            {
                kws_diag(&d);
                printf("  hit #%d @ %d.%03ds  prob=%d rel=%.1fdB floor=%.1fdB\n",
                       hits, t, (int)((i % 8000) * 1000 / 8000), d.prob,
                       d.rel_db10 / 10.0, d.floor_db10 / 10.0);
            }
        }
        kws_diag(&d);
        if (verbose >= 2) printf("%d\n", d.prob);
        if (d.prob_peak > peak) peak = d.prob_peak;
        /* 每秒一行：看底噪有没有被语音顶上去（这是"喊不醒"最常见的根因） */
        if (verbose && i >= 320 && (i / 8000) != ((i - 320) / 8000)) {
            printf("  t=%3ds prob_peak=%3d rel=%5.1f floor=%6.1f hot=%d gate=%d\n",
                   (int)(i / 8000), d.prob_peak, d.rel_db10 / 10.0, d.floor_db10 / 10.0,
                   d.hot, d.gated_total ? 1 : 0);
        }
    }

    kws_diag(&d);
    printf("kws: %s  时长=%.1fs  命中=%d  峰值=%d  hot=%d 过门控=%d 零帧=%d 重置=%d\n",
           wav, (double)ns / 8000.0, hits, peak, d.hot_total, d.gated_total,
           d.zero_total, d.resets);

    free(pcm);
    return 0;
}

/* 诊断：把 afe 的 AGC 前抽头落盘，用来确认 KWS 实际吃到的是什么 */
static int cmd_afedump(const char *wav, const char *out, int post_agc)
{
    int16_t *pcm = NULL;
    size_t ns = 0, i;
    FILE *f;

    if (!read_wav(wav, &pcm, &ns, 8000)) return 2;
    f = fopen(out, "wb");
    if (f == NULL) { free(pcm); return 4; }

    afe_reset(&g_afe_kws);
    for (i = 0; i + 320 <= ns; i += 320) {
        afe_process(&g_afe_kws, &pcm[i], g_afe_out, 320);
        fwrite(post_agc ? g_afe_out : g_afe_kws.tmp, sizeof(int16_t), 320, f);
    }
    fclose(f);
    printf("afedump: %zu 样本 -> %s\n", ns, out);
    free(pcm);
    return 0;
}

static int cmd_cmp(const char *a, const char *b, int limit)
{
    unsigned char *x = NULL, *y = NULL;
    size_t nx = 0, ny = 0, i;
    int maxd = 0, at = -1, ndiff = 0;

    if (!read_file(a, &x, &nx) || !read_file(b, &y, &ny)) {
        fprintf(stderr, "读文件失败\n");
        free(x); free(y);
        return 2;
    }
    if (nx != ny) {
        printf("长度不同: %zu vs %zu\n", nx, ny);
        free(x); free(y);
        return 3;
    }
    for (i = 0; i < nx; i++) {
        int d = (int)x[i] - (int)y[i];
        if (d < 0) d = -d;
        if (d > 0) ndiff++;
        if (d > maxd) { maxd = d; at = (int)i; }
    }
    printf("比较 %zu 项: 不同 %d 项, 最大偏差 %d (第 %d 项), 判据 <= %d -> %s\n",
           nx, ndiff, maxd, at, limit, maxd <= limit ? "PASS" : "FAIL");
    free(x);
    free(y);
    return maxd <= limit ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "用法: %s feat|prob|all|stream|resamp|kws|cmp ...\n", argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "feat") == 0 && argc >= 4) return cmd_feat(argv[2], argv[3]);
    if (strcmp(argv[1], "prob") == 0 && argc >= 4) return cmd_prob(argv[2], argv[3]);
    if (strcmp(argv[1], "stream") == 0 && argc >= 4) {
        return cmd_stream(argv[2], argv[3], argc >= 5 ? atoi(argv[4]) : 0,
                          argc >= 6 ? atoi(argv[5]) : 0);
    }
    if (strcmp(argv[1], "resamp") == 0 && argc >= 4) return cmd_resamp(argv[2], argv[3]);
    if (strcmp(argv[1], "afedump") == 0 && argc >= 4)
        return cmd_afedump(argv[2], argv[3], argc >= 5 ? atoi(argv[4]) : 0);
    if (strcmp(argv[1], "kws") == 0 && argc >= 3) {
        return cmd_kws(argv[2],
                       argc >= 4 ? atoi(argv[3]) : KWS_DEF_THRESHOLD,
                       argc >= 5 ? atoi(argv[4]) : KWS_DEF_GAIN_DB,
                       argc >= 6 ? atoi(argv[5]) : KWS_DEF_GATE_DB10,
                       argc >= 7 ? atoi(argv[6]) : 0,
                       argc >= 8 ? atoi(argv[7]) : 0,
                       argc >= 9 ? atoi(argv[8]) : 0,
                       argc >= 10 ? atoi(argv[9]) : 0);
    }
    if (strcmp(argv[1], "cmp") == 0 && argc >= 4) {
        return cmd_cmp(argv[2], argv[3], argc >= 5 ? atoi(argv[4]) : 0);
    }
    if (strcmp(argv[1], "all") == 0 && argc >= 4) {
        char f[512], p[512];
        int rc;
        snprintf(f, sizeof(f), "%s.feat", argv[3]);
        snprintf(p, sizeof(p), "%s.prob", argv[3]);
        rc = cmd_feat(argv[2], f);
        if (rc != 0) return rc;
        return cmd_prob(f, p);
    }

    fprintf(stderr, "参数不对\n");
    return 1;
}
