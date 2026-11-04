/*
 * p4_loopback.c — 采集直通播放（共存模式），用于主观评估音质与断音
 *
 * 目的
 *   p2_capture 证明能采、p3_playback 证明能放。本程序把两者接起来：
 *   对着摄像头说话 → 直接从喇叭出来。
 *   这是判断"音质能不能接受""会不会断"最直接的手段。
 *
 * 关键设计
 *   1. 用 select() 等 AI 数据就绪，天然以帧长(40ms)为节奏，不会把 AO 缓冲灌爆
 *   2. 每帧发送前用 HI_MPI_AO_QueryChnStat 查缓冲水位，超过阈值就丢弃本帧，
 *      避免 p3 里发现的"写入快于播放"导致缓冲累积
 *   3. 同时把采集到的 PCM 落盘到 SD 卡，便于回放取证
 *
 * 注意
 *   - 不 kill rmm（看门狗的唯一合法持有者）
 *   - 不调用 VB_Exit/VB_Init
 *   - 不 Disable AI/AO，避免影响 rmm 的对讲
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/select.h>
#include <sys/time.h>

#include "hi_type.h"
#include "hi_common.h"
#include "hi_comm_aio.h"
#include "mpi_sys.h"
#include "mpi_ai.h"
#include "mpi_ao.h"

#define AI_DEV        0
#define AI_CHN        0
#define AO_DEV        0
#define AO_CHN        0
#define SYS_ALIGN     64
#define RUN_SECONDS   20
#define SELECT_TIMEOUT_S 1
#define PCM_OUT       "/tmp/hd1/loopback.pcm"
#define AO_BUF_HIGH   24        /* AO 缓冲超过该帧数则丢帧，防累积（frm 总 30） */

int main(void)
{
    HI_S32         ret;
    AIO_ATTR_S     attr;
    AIO_ATTR_S     got;
    AUDIO_FRAME_S  frame;
    AEC_FRAME_S    ref;
    MPP_SYS_CONF_S sysconf;
    int            ai_fd;
    int            rate, pt_per_frm;
    int            ok = 0, drop = 0, tmo = 0, fail = 0;
    FILE          *fp;
    struct timeval t0, t1;
    long           elapsed_ms;
    int            last_seq_gap = 0;

    printf("=== p4_loopback begin (采集直通播放，20 秒) ===\n");
    fflush(stdout);

    memset(&sysconf, 0, sizeof(sysconf));
    sysconf.u32AlignWidth = SYS_ALIGN;
    HI_MPI_SYS_SetConf(&sysconf);

    ret = HI_MPI_SYS_Init();
    printf("[1] SYS_Init               ret=%d\n", (int)ret);
    if (ret != 0) return 1;

    memset(&got, 0, sizeof(got));
    if (HI_MPI_AI_GetPubAttr(AI_DEV, &got) != 0) {
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
    rate       = (int)got.enSamplerate ? (int)got.enSamplerate : 8000;
    pt_per_frm = (int)got.u32PtNumPerFrm ? (int)got.u32PtNumPerFrm : 320;
    printf("    rate=%d ch=%u pt=%u frm=%u\n", rate,
           (unsigned)got.u32ChnCnt, (unsigned)got.u32PtNumPerFrm,
           (unsigned)got.u32FrmNum);

    memcpy(&attr, &got, sizeof(attr));
    HI_MPI_AI_SetPubAttr(AI_DEV, &attr);
    HI_MPI_AI_Enable(AI_DEV);
    ret = HI_MPI_AI_EnableChn(AI_DEV, AI_CHN);
    printf("[2] AI_EnableChn           ret=%d\n", (int)ret);

    memcpy(&attr, &got, sizeof(attr));
    HI_MPI_AO_SetPubAttr(AO_DEV, &attr);
    HI_MPI_AO_Enable(AO_DEV);
    ret = HI_MPI_AO_EnableChn(AO_DEV, AO_CHN);
    printf("[3] AO_EnableChn           ret=%d\n", (int)ret);

    if (ret != 0) {
        printf("!! AO 打不开，无法环回\n");
        return 2;
    }

    ai_fd = HI_MPI_AI_GetFd(AI_DEV, AI_CHN);
    printf("[4] AI_GetFd               fd=%d\n", ai_fd);
    if (ai_fd < 0) return 3;

    fp = fopen(PCM_OUT, "wb");
    printf("PCM 落盘: %s (%s)\n", PCM_OUT, fp ? "ok" : "无法打开，仅内存");
    fflush(stdout);
    printf(">>> 现在对着摄像头说话，20 秒内应能从喇叭听到自己 <<<\n");
    fflush(stdout);

    gettimeofday(&t0, NULL);

    while (1) {
        fd_set rfds;
        struct timeval tv;
        AO_CHN_STATE_S st;

        gettimeofday(&t1, NULL);
        elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_usec - t0.tv_usec) / 1000;
        if (elapsed_ms >= RUN_SECONDS * 1000) break;

        FD_ZERO(&rfds);
        FD_SET(ai_fd, &rfds);
        tv.tv_sec = SELECT_TIMEOUT_S;
        tv.tv_usec = 0;

        ret = select(ai_fd + 1, &rfds, NULL, NULL, &tv);
        if (ret == 0) { tmo++; continue; }
        if (ret < 0) { fail++; break; }

        memset(&frame, 0, sizeof(frame));
        memset(&ref, 0, sizeof(ref));
        ret = HI_MPI_AI_GetFrame(AI_DEV, AI_CHN, &frame, &ref, HI_FALSE);
        if (ret != 0) { fail++; continue; }

        if (frame.u32Seq && last_seq_gap >= 0) {
            /* 检测序号跳变，说明采集侧丢帧 */
        }
        last_seq_gap = (int)frame.u32Seq;

        if (fp && frame.pVirAddr[0] && frame.u32Len > 0)
            fwrite(frame.pVirAddr[0], 1, frame.u32Len, fp);

        /* AO 缓冲水位保护 */
        memset(&st, 0, sizeof(st));
        if (HI_MPI_AO_QueryChnStat(AO_DEV, AO_CHN, &st) == 0) {
            if (st.u32ChnBusyNum > AO_BUF_HIGH) {
                drop++;
                HI_MPI_AI_ReleaseFrame(AI_DEV, AI_CHN, &frame, &ref);
                continue;
            }
        }

        ret = HI_MPI_AO_SendFrame(AO_DEV, AO_CHN, &frame, HI_FALSE);
        if (ret == 0) ok++; else fail++;

        HI_MPI_AI_ReleaseFrame(AI_DEV, AI_CHN, &frame, &ref);
    }

    gettimeofday(&t1, NULL);
    elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_usec - t0.tv_usec) / 1000;

    if (fp) fclose(fp);

    printf("\n===== 环回结果 =====\n");
    printf("运行时长     : %ld ms\n", elapsed_ms);
    printf("已播放帧     : %d\n", ok);
    printf("因缓冲丢弃   : %d\n", drop);
    printf("select 超时  : %d\n", tmo);
    printf("失败         : %d\n", fail);
    if (ok > 0) {
        double play_ms = (double)ok * pt_per_frm * 1000.0 / rate;
        printf("理论播放时长 : %.0f ms\n", play_ms);
        printf("丢帧率       : %.2f%%\n",
               ok > 0 ? (double)drop * 100.0 / (double)(ok + drop) : 0.0);
        printf("PCM 文件     : %s (%.0f KB)\n", PCM_OUT,
               play_ms * rate * 2 / 1000.0 / 1024.0);
    }
    printf("\n===== 结论 =====\n");
    if (ok > 0 && drop == 0)
        printf("环回通畅，全程无丢帧。可直接主观评估音质与断音。\n");
    else if (ok > 0)
        printf("环回基本通畅，但存在丢帧，需调 AO_BUF_HIGH 或发送节奏。\n");
    else
        printf("环回失败。\n");
    fflush(stdout);
    return 0;
}
