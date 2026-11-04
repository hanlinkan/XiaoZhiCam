/*
 * p3_playback.c — 共存模式下验证 AO 播放（不杀 rmm）
 *
 * 背景
 *   逆向 rmm_fast3 得到确定结论：rmm 是看门狗的唯一合法持有者。
 *   实测对照组（kill rmm 后什么都不做）系统照样复位，
 *   说明 hidog 内核线程不会接管。因此**不能停 rmm**。
 *
 *   那就走共存：rmm 继续跑（喂狗、摄像头正常），我们与它共用音频设备。
 *   p2_capture 已验证 AI 采集可行（100/100 帧，RMS 33.4%）。
 *   本程序验证另一半：AO 播放。
 *
 * 做法
 *   1. 读 rmm 已生效的 AO 参数，原样沿用（改不了，也没必要改）
 *   2. AO_Enable + EnableChn
 *   3. 生成 1kHz 正弦波（8kHz/16bit/mono），分帧 SendFrame 播放 2 秒
 *   4. 统计 SendFrame 成功率与耗时
 *
 * 注意
 *   - 绝不调用 HI_MPI_VB_Exit/VB_Init（会毁掉 rmm 的视频缓冲池）
 *   - 不 Disable AO，避免干扰 rmm 的对讲功能
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/time.h>

#include "hi_type.h"
#include "hi_common.h"
#include "hi_comm_aio.h"
#include "mpi_sys.h"
#include "mpi_ai.h"
#include "mpi_ao.h"

#define AO_DEV        0
#define AO_CHN        0
#define SYS_ALIGN     64
#define TONE_HZ       1000
#define PLAY_SECONDS  2
#define FRAME_MS      40          /* 320 点 @8kHz = 40ms，与 rmm 的 pt 一致 */

static short g_buf[2048];

int main(void)
{
    HI_S32        ret;
    AIO_ATTR_S    attr;
    AIO_ATTR_S    got;
    AUDIO_FRAME_S frame;
    MPP_SYS_CONF_S sysconf;
    int           rate, pt_per_frm, frm_num, chn_cnt;
    int           frames, i, ok = 0, fail = 0;
    int           phase = 0;
    struct timeval t0, t1;
    long          elapsed_ms;

    printf("=== p3_playback begin (共存模式，不杀 rmm) ===\n");
    fflush(stdout);

    memset(&sysconf, 0, sizeof(sysconf));
    sysconf.u32AlignWidth = SYS_ALIGN;
    ret = HI_MPI_SYS_SetConf(&sysconf);
    printf("[1] SYS_SetConf(align=%d)  ret=%d\n", SYS_ALIGN, (int)ret);

    ret = HI_MPI_SYS_Init();
    printf("[2] SYS_Init               ret=%d\n", (int)ret);
    if (ret != 0) {
        printf("!! SYS_Init 失败\n");
        return 1;
    }

    /* 读 rmm 当前 AO 参数 */
    memset(&got, 0, sizeof(got));
    if (HI_MPI_AO_GetPubAttr(AO_DEV, &got) == 0) {
        printf("    rmm AO: rate=%d width=%d sound=%d ch=%u pt=%u frm=%u\n",
               (int)got.enSamplerate, (int)got.enBitwidth, (int)got.enSoundmode,
               (unsigned)got.u32ChnCnt, (unsigned)got.u32PtNumPerFrm,
               (unsigned)got.u32FrmNum);
        rate       = (int)got.enSamplerate ? (int)got.enSamplerate : 8000;
        pt_per_frm = (int)got.u32PtNumPerFrm ? (int)got.u32PtNumPerFrm : 320;
        frm_num    = (int)got.u32FrmNum ? (int)got.u32FrmNum : 30;
        chn_cnt    = (int)got.u32ChnCnt ? (int)got.u32ChnCnt : 2;
    } else {
        printf("    AO_GetPubAttr 失败，用官方默认值\n");
        rate = 8000; pt_per_frm = 320; frm_num = 30; chn_cnt = 2;
        memset(&got, 0, sizeof(got));
        got.enSamplerate   = AUDIO_SAMPLE_RATE_8000;
        got.enBitwidth     = AUDIO_BIT_WIDTH_16;
        got.enWorkmode     = AIO_MODE_I2S_MASTER;
        got.enSoundmode    = AUDIO_SOUND_MODE_MONO;
        got.u32EXFlag      = 1;
        got.u32FrmNum      = 30;
        got.u32PtNumPerFrm = 320;
        got.u32ChnCnt      = 2;
        got.u32ClkSel      = 1;
    }

    /* 尝试设置（预期失败，已被锁定） */
    memcpy(&attr, &got, sizeof(attr));
    ret = HI_MPI_AO_SetPubAttr(AO_DEV, &attr);
    printf("[3] AO_SetPubAttr          ret=%d (失败属正常)\n", (int)ret);

    ret = HI_MPI_AO_Enable(AO_DEV);
    printf("[4] AO_Enable              ret=%d\n", (int)ret);

    ret = HI_MPI_AO_EnableChn(AO_DEV, AO_CHN);
    printf("[5] AO_EnableChn           ret=%d\n", (int)ret);

    if (ret != 0) {
        printf("!! AO 通道打不开，播放无法验证\n");
        return 2;
    }

    /* 生成 1kHz 正弦波（整数查表，避免浮点） */
    frames = PLAY_SECONDS * 1000 / FRAME_MS;
    printf("播放 %d 帧 (%d 秒, %dHz 正弦)...\n", frames, PLAY_SECONDS, TONE_HZ);
    fflush(stdout);

    gettimeofday(&t0, NULL);

    for (i = 0; i < frames; i++) {
        int n = pt_per_frm;
        int k;
        if (n > (int)(sizeof(g_buf)/sizeof(g_buf[0]))) n = sizeof(g_buf)/sizeof(g_buf[0]);

        for (k = 0; k < n; k++) {
            /* 简单方波近似，幅度约 1/4 满量程，够验证通路且不刺耳 */
            int half = rate / (2 * TONE_HZ);
            g_buf[k] = ((phase / (half ? half : 1)) & 1) ? 6000 : -6000;
            phase++;
            if (phase >= rate) phase = 0;
        }

        memset(&frame, 0, sizeof(frame));
        frame.enBitwidth  = AUDIO_BIT_WIDTH_16;
        frame.enSoundmode = AUDIO_SOUND_MODE_MONO;
        frame.pVirAddr[0] = g_buf;
        frame.u32PhyAddr[0] = 0;
        frame.u32Len      = (HI_U32)(n * sizeof(short));
        frame.u32Seq      = (HI_U32)i;
        frame.u64TimeStamp = 0;

        ret = HI_MPI_AO_SendFrame(AO_DEV, AO_CHN, &frame, HI_TRUE);
        if (ret == 0) {
            ok++;
        } else {
            fail++;
            if (fail <= 3)
                printf("     [%d] SendFrame ret=%d\n", i, (int)ret);
        }
    }

    gettimeofday(&t1, NULL);
    elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_usec - t0.tv_usec) / 1000;

    printf("\n===== 播放结果 =====\n");
    printf("SendFrame 成功 : %d / %d\n", ok, frames);
    printf("SendFrame 失败 : %d\n", fail);
    printf("总耗时         : %ld ms（理论 %d ms）\n", elapsed_ms, PLAY_SECONDS * 1000);
    if (ok > 0 && elapsed_ms > 0)
        printf("平均每帧耗时   : %.1f ms（帧长 %d ms）\n",
               (double)elapsed_ms / (double)ok, FRAME_MS);

    printf("\n===== 结论 =====\n");
    if (ok == frames)
        printf("AO 播放通路打通：共存模式下可以完整推流播放。\n");
    else if (ok > 0)
        printf("AO 部分成功，存在丢帧，需要收紧发送节奏。\n");
    else
        printf("AO 播放失败，需要排查。\n");
    fflush(stdout);

    return 0;
}
