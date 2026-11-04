/*
 * p2_capture.c — 共存模式下采集音频（不杀 rmm）
 *
 * 前几次失败的三个原因，这次全部修正：
 *   1. 没有 HI_MPI_SYS_SetConf(u32AlignWidth)：MPP 用默认对齐宽度分配 AI 缓冲，
 *      导致 "Pool 0 hasn't mapped!"。官方 sample 用 64。
 *   2. 直接 GetFrame 而不先等数据就绪：官方用 select() 等 AI fd 可读后再取帧。
 *   3. 参数照搬 16kHz/1ch，与 rmm 已生效的 8kHz/2ch 不一致。
 *
 * 本程序**不 kill rmm**，走共存路线：
 *   rmm 继续跑（喂看门狗、摄像头正常），我们只读取 AI 通道。
 *
 * 注意：绝不调用 HI_MPI_VB_Exit/VB_Init——那会销毁 rmm 的视频缓冲池。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/select.h>

#include "hi_type.h"
#include "hi_common.h"
#include "hi_comm_aio.h"
#include "mpi_sys.h"
#include "mpi_ai.h"
#include "mpi_ao.h"

#define AI_DEV        0
#define AI_CHN        0
#define SYS_ALIGN     64          /* SAMPLE_SYS_ALIGN_WIDTH */
#define CAP_FRAMES    100         /* 100 帧 × 40ms ≈ 4 秒 */
#define SELECT_TIMEOUT_S 2

int main(void)
{
    HI_S32        ret;
    AIO_ATTR_S    attr;
    AIO_ATTR_S    got;
    AUDIO_FRAME_S frame;
    AEC_FRAME_S   ref;
    MPP_SYS_CONF_S sysconf;
    int           ai_fd;
    int           i, ok = 0, timeout_cnt = 0, fail_cnt = 0;
    long          peak = 0, total = 0, clipped = 0;
    double        sum_sq = 0;

    printf("=== p2_capture begin (共存模式，不杀 rmm) ===\n");
    fflush(stdout);

    /* 1. 设置系统对齐宽度——必须在 SYS_Init 之前 */
    memset(&sysconf, 0, sizeof(sysconf));
    sysconf.u32AlignWidth = SYS_ALIGN;
    ret = HI_MPI_SYS_SetConf(&sysconf);
    printf("[%d] SYS_SetConf(align=%d)   ret=%d\n", 1, SYS_ALIGN, (int)ret);

    ret = HI_MPI_SYS_Init();
    printf("[%d] SYS_Init                ret=%d\n", 2, (int)ret);
    if (ret != 0) {
        printf("!! SYS_Init 失败，无法继续\n");
        return 1;
    }

    /* 2. 读 rmm 当前生效的参数，按它的值来（共存必须一致） */
    memset(&got, 0, sizeof(got));
    if (HI_MPI_AI_GetPubAttr(AI_DEV, &got) == 0) {
        printf("     rmm 当前: rate=%d width=%d mode=%d sound=%d ch=%u pt=%u frm=%u clksel=%u exflag=%u\n",
               (int)got.enSamplerate, (int)got.enBitwidth, (int)got.enWorkmode,
               (int)got.enSoundmode, (unsigned)got.u32ChnCnt,
               (unsigned)got.u32PtNumPerFrm, (unsigned)got.u32FrmNum,
               (unsigned)got.u32ClkSel, (unsigned)got.u32EXFlag);
    }

    /* 3. 用 rmm 的参数原样尝试（预期 SetPubAttr 仍失败，因为已被锁定） */
    memcpy(&attr, &got, sizeof(attr));
    if (attr.enSamplerate == 0) {          /* 读不到就给官方默认 */
        memset(&attr, 0, sizeof(attr));
        attr.enSamplerate   = AUDIO_SAMPLE_RATE_8000;
        attr.enBitwidth     = AUDIO_BIT_WIDTH_16;
        attr.enWorkmode     = AIO_MODE_I2S_MASTER;
        attr.enSoundmode    = AUDIO_SOUND_MODE_MONO;
        attr.u32EXFlag      = 1;
        attr.u32FrmNum      = 30;
        attr.u32PtNumPerFrm = 320;
        attr.u32ChnCnt      = 2;
        attr.u32ClkSel      = 1;
    }
    ret = HI_MPI_AI_SetPubAttr(AI_DEV, &attr);
    printf("[%d] AI_SetPubAttr           ret=%d (失败属正常，参数已被 rmm 锁定)\n", 3, (int)ret);

    ret = HI_MPI_AI_Enable(AI_DEV);
    printf("[%d] AI_Enable               ret=%d\n", 4, (int)ret);

    ret = HI_MPI_AI_EnableChn(AI_DEV, AI_CHN);
    printf("[%d] AI_EnableChn            ret=%d\n", 5, (int)ret);

    /* 4. 取 AI 的 fd 用于 select */
    ai_fd = HI_MPI_AI_GetFd(AI_DEV, AI_CHN);
    printf("[%d] AI_GetFd                fd=%d\n", 6, ai_fd);
    if (ai_fd < 0) {
        printf("!! 拿不到 AI fd，中止\n");
        return 2;
    }

    /* 5. select 等数据就绪再取帧 */
    printf("开始采集 %d 帧...\n", CAP_FRAMES);
    fflush(stdout);

    for (i = 0; i < CAP_FRAMES; i++) {
        fd_set rfds;
        struct timeval tv;

        FD_ZERO(&rfds);
        FD_SET(ai_fd, &rfds);
        tv.tv_sec  = SELECT_TIMEOUT_S;
        tv.tv_usec = 0;

        ret = select(ai_fd + 1, &rfds, NULL, NULL, &tv);
        if (ret == 0) {
            timeout_cnt++;
            if (timeout_cnt <= 3)
                printf("     [%d] select 超时（AI 无数据）\n", i);
            continue;
        }
        if (ret < 0) {
            printf("     [%d] select 出错 errno=%d\n", i, errno);
            break;
        }

        memset(&frame, 0, sizeof(frame));
        memset(&ref, 0, sizeof(ref));
        ret = HI_MPI_AI_GetFrame(AI_DEV, AI_CHN, &frame, &ref, HI_FALSE);
        if (ret != 0) {
            fail_cnt++;
            if (fail_cnt <= 3)
                printf("     [%d] GetFrame ret=%d\n", i, (int)ret);
            continue;
        }

        ok++;
        if (frame.pVirAddr[0] && frame.u32Len > 0) {
            short *pcm = (short *)frame.pVirAddr[0];
            int    cnt = (int)(frame.u32Len / sizeof(short));
            int    k;
            for (k = 0; k < cnt; k++) {
                long v = pcm[k];
                long a = v < 0 ? -v : v;
                if (a > peak) peak = a;
                if (a >= 32767) clipped++;
                sum_sq += (double)(v * v);
            }
            total += cnt;
        }
        HI_MPI_AI_ReleaseFrame(AI_DEV, AI_CHN, &frame, &ref);
    }

    printf("\n===== 采集结果 =====\n");
    printf("成功取帧 : %d / %d\n", ok, CAP_FRAMES);
    printf("select超时: %d\n", timeout_cnt);
    printf("取帧失败 : %d\n", fail_cnt);
    printf("样本总数 : %ld\n", total);
    if (total > 0) {
        double ms = sum_sq / (double)total;
        double y = ms > 0 ? ms / 2.0 : 0;
        int it;
        for (it = 0; it < 25; it++) y = (y + ms / y) / 2.0;
        printf("RMS 电平 : %.0f / 32767 (%.1f%%)\n", y, y * 100.0 / 32767.0);
        printf("峰值     : %ld (%.1f%%)\n", peak, (double)peak * 100.0 / 32767.0);
        printf("削波样本 : %ld\n", clipped);
    }

    printf("\n===== 结论 =====\n");
    if (ok > 0 && total > 0)
        printf("共存采集打通：不杀 rmm 也能拿到音频数据。\n");
    else if (timeout_cnt > 0)
        printf("select 全部超时：AI 通道已打开但没有数据流（可能 rmm 未启用采集）\n");
    else
        printf("取帧失败，需要进一步排查。\n");
    fflush(stdout);

    /* 不 Disable：避免影响 rmm 的音频配置 */
    return 0;
}
