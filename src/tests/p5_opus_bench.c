/*
 * p5_opus_bench.c — Opus 定点编解码性能基准（目标设备实测）
 *
 * 这是决定音频方案生死的最后一项：
 *   如果 8kHz 单声道 Opus 的实时因子 RTF 接近或超过 1，
 *   说明这颗 240MHz 无 FPU 的核扛不住，必须改用硬件 G.711/G.726。
 *
 * 测法
 *   用 1kHz 方波生成 8kHz/16bit/mono PCM，跑 N 帧编码 + N 帧解码，
 *   用 gettimeofday 计时，算出：
 *     RTF  = 处理总耗时 / 音频总时长   （<1 才算实时可行）
 *     CPU% = RTF × 100                 （单核占用）
 *   分别在 complexity 0/1/2 下测，找出可用的档位。
 *
 * 说明
 *   - 只用 libopus 定点库（已确认软浮点调用为 0）
 *   - 帧长 20ms（160 样本）；协议侧 60ms 可用多帧打包
 *   - 结果落盘 SD 卡，便于设备复位后仍可查
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>

#include <opus.h>

#define RATE        8000
#define CHANNELS    1
#define FRAME_MS    20
#define FRAME_SAMP  (RATE / 1000 * FRAME_MS)     /* 160 */
#define N_FRAMES    1500                          /* 30 秒音频 */
#define MAX_PKT     512

static short  g_pcm[FRAME_SAMP];
static short  g_out[FRAME_SAMP];
static unsigned char g_pkt[MAX_PKT];

static double now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

static void gen_tone(void)
{
    int i;
    for (i = 0; i < FRAME_SAMP; i++) {
        static int phase = 0;
        int half = RATE / 2000;              /* 1kHz 半周期样本数 */
        g_pcm[i] = ((phase / half) & 1) ? 6000 : -6000;
        phase = (phase + 1) % RATE;
    }
}

int main(void)
{
    FILE *log = fopen("/tmp/hd1/opus_bench.log", "w");
    int   complexity_list[] = { 0, 1, 2, 5 };
    int   ci;

    if (log == NULL) log = stdout;
    setvbuf(log, NULL, _IOLBF, 0);

    fprintf(log, "=== p5_opus_bench ===\n");
    fprintf(log, "libopus %s, rate=%d ch=%d frame=%dms(%d samp), frames=%d (%.0fs audio)\n",
            opus_get_version_string(), RATE, CHANNELS, FRAME_MS, FRAME_SAMP,
            N_FRAMES, (double)N_FRAMES * FRAME_MS / 1000.0);
    fflush(log);

    for (ci = 0; ci < (int)(sizeof(complexity_list)/sizeof(int)); ci++) {
        int          complexity = complexity_list[ci];
        int          err = 0;
        OpusEncoder *enc;
        OpusDecoder *dec;
        int          i;
        double       t0, t1, enc_ms = 0, dec_ms = 0;
        long         total_bytes = 0;
        int          enc_ok = 0, dec_ok = 0;

        enc = opus_encoder_create(RATE, CHANNELS, OPUS_APPLICATION_VOIP, &err);
        if (err != OPUS_OK || enc == NULL) {
            fprintf(log, "[cx=%d] encoder_create 失败: %s\n", complexity, opus_strerror(err));
            continue;
        }
        dec = opus_decoder_create(RATE, CHANNELS, &err);
        if (err != OPUS_OK || dec == NULL) {
            fprintf(log, "[cx=%d] decoder_create 失败: %s\n", complexity, opus_strerror(err));
            opus_encoder_destroy(enc);
            continue;
        }

        opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(complexity));
        opus_encoder_ctl(enc, OPUS_SET_BITRATE(16000));   /* 16kbps，语音够用 */
        opus_encoder_ctl(enc, OPUS_SET_VBR(1));

        /* ---- 编码 ---- */
        gen_tone();
        t0 = now_ms();
        for (i = 0; i < N_FRAMES; i++) {
            int n = opus_encode(enc, g_pcm, FRAME_SAMP, g_pkt, MAX_PKT);
            if (n > 0) { enc_ok++; total_bytes += n; }
        }
        t1 = now_ms();
        enc_ms = t1 - t0;

        /* ---- 解码 ---- */
        t0 = now_ms();
        for (i = 0; i < N_FRAMES; i++) {
            int n = opus_decode(dec, g_pkt, (opus_int32)(total_bytes / (enc_ok ? enc_ok : 1)),
                                g_out, FRAME_SAMP, 0);
            if (n > 0) dec_ok++;
        }
        t1 = now_ms();
        dec_ms = t1 - t0;

        {
            double audio_ms = (double)N_FRAMES * FRAME_MS;
            double enc_rtf  = enc_ms / audio_ms;
            double dec_rtf  = dec_ms / audio_ms;
            double avg_pkt  = enc_ok ? (double)total_bytes / enc_ok : 0;

            fprintf(log, "\n[cx=%d] ---- 结果 ----\n", complexity);
            fprintf(log, "  编码成功 %d/%d, 平均包长 %.1f B (%.1f kbps)\n",
                    enc_ok, N_FRAMES, avg_pkt, avg_pkt * 8.0 / (FRAME_MS / 1000.0) / 1000.0);
            fprintf(log, "  解码成功 %d/%d\n", dec_ok, N_FRAMES);
            fprintf(log, "  编码耗时 %.0f ms  -> RTF %.3f, CPU %5.1f%%\n",
                    enc_ms, enc_rtf, enc_rtf * 100.0);
            fprintf(log, "  解码耗时 %.0f ms  -> RTF %.3f, CPU %5.1f%%\n",
                    dec_ms, dec_rtf, dec_rtf * 100.0);
            fprintf(log, "  编+解合计 CPU %5.1f%%\n", (enc_rtf + dec_rtf) * 100.0);
            fflush(log);
        }

        opus_encoder_destroy(enc);
        opus_decoder_destroy(dec);
    }

    fprintf(log, "\n=== 判读 ===\n");
    fprintf(log, "RTF < 0.35  → 宽裕，可叠加唤醒词与网络开销\n");
    fprintf(log, "RTF 0.35~0.6 → 可用，但需严格控制其他线程开销\n");
    fprintf(log, "RTF > 0.6    → 风险高，考虑改用 G.711/G.726 硬件编解码\n");
    fflush(log);

    if (log != stdout) fclose(log);
    return 0;
}
