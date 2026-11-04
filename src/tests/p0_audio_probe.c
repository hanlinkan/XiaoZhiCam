/*
 * p0_audio_probe.c — P0 可行性验证：Hi3518E 音频通道占用探测
 *
 * 目的
 *   1. 确认 MPP 库能在目标设备上正常初始化（ABI / 内核接口兼容）
 *   2. 确认 16 kHz 采样率是否被 AI/AO 接受
 *   3. 判定原厂 rmm 进程是否独占音频：本程序**故意不停 rmm**，
 *      直接在它运行时尝试 Init，用返回值判断能否共存。
 *
 * 判读
 *   SYS_Init 成功 + AI_Enable 失败  → 音频被 rmm 独占，必须先停 rmm
 *   SYS_Init 失败                    → MPP 库与内核不匹配，工具链或 SDK 版本有问题
 *   AI_Enable 成功                   → 可共存，方案可大幅简化
 *
 * 编译：见同目录 Makefile
 * 运行：./p0_audio_probe
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "hi_type.h"
#include "hi_common.h"
#include "hi_comm_aio.h"
#include "mpi_sys.h"
#include "mpi_ai.h"
#include "mpi_ao.h"

#define AI_DEV      0
#define AI_CHN      0
#define AO_DEV      0
#define AO_CHN      0

#define PROBE_RATE     AUDIO_SAMPLE_RATE_16000
#define PROBE_PT_FRM   320          /* 20 ms @ 16 kHz */

static int g_step = 0;

/* 统一的结果打印：步号、动作、返回码。返回码语义由海思定义：
 * 0 成功；非 0 失败（负值，具体见 hi_errno.h）。 */
static void report(const char *what, HI_S32 ret)
{
    g_step++;
    printf("[%02d] %-24s ret=%d %s\n",
           g_step, what, (int)ret, ret == 0 ? "OK" : "FAIL");
    fflush(stdout);
}

static void fill_attr(AIO_ATTR_S *a)
{
    memset(a, 0, sizeof(*a));
    a->enSamplerate   = PROBE_RATE;
    a->enBitwidth     = AUDIO_BIT_WIDTH_16;
    a->enWorkmode     = AIO_MODE_I2S_MASTER;
    a->enSoundmode    = AUDIO_SOUND_MODE_MONO;
    a->u32EXFlag      = 0;
    a->u32FrmNum      = 4;          /* 4 帧缓冲，够小，便于观察 */
    a->u32PtNumPerFrm = PROBE_PT_FRM;
    a->u32ChnCnt      = 1;
    a->u32ClkSel      = 0;
}

int main(void)
{
    HI_S32      ret;
    AIO_ATTR_S  attr;
    AIO_ATTR_S  got;
    AUDIO_FRAME_S frame;
    AEC_FRAME_S   aec;

    printf("=== p0_audio_probe begin ===\n");
    printf("pid=%d  target=%dHz mono 16bit  %dpt/frm(20ms)\n",
           (int)getpid(), (int)PROBE_RATE, PROBE_PT_FRM);
    fflush(stdout);

    /* ---- 1. MPP 全局初始化 ---- */
    ret = HI_MPI_SYS_Init();
    report("HI_MPI_SYS_Init", ret);
    if (ret != 0) {
        printf("!! MPP 无法初始化：库与内核不匹配，或依赖库缺失\n");
        return 1;
    }

    /* ---- 2. AI（麦克风）---- */
    fill_attr(&attr);

    ret = HI_MPI_AI_SetPubAttr(AI_DEV, &attr);
    report("AI_SetPubAttr", ret);

    ret = HI_MPI_AI_Enable(AI_DEV);
    report("AI_Enable", ret);

    ret = HI_MPI_AI_EnableChn(AI_DEV, AI_CHN);
    report("AI_EnableChn", ret);

    /* ---- 3. AO（喇叭）---- */
    ret = HI_MPI_AO_SetPubAttr(AO_DEV, &attr);
    report("AO_SetPubAttr", ret);

    ret = HI_MPI_AO_Enable(AO_DEV);
    report("AO_Enable", ret);

    ret = HI_MPI_AO_EnableChn(AO_DEV, AO_CHN);
    report("AO_EnableChn", ret);

    /* ---- 4. 读回实际生效参数（驱动可能改写采样率）---- */
    memset(&got, 0, sizeof(got));
    ret = HI_MPI_AI_GetPubAttr(AI_DEV, &got);
    if (ret == 0) {
        printf("     AI actual: rate=%d width=%d mode=%d sound=%d frm=%u pt=%u ch=%u\n",
               (int)got.enSamplerate, (int)got.enBitwidth, (int)got.enWorkmode,
               (int)got.enSoundmode,
               (unsigned)got.u32FrmNum, (unsigned)got.u32PtNumPerFrm,
               (unsigned)got.u32ChnCnt);
    } else {
        printf("     AI_GetPubAttr ret=%d\n", (int)ret);
    }

    /* ---- 5. 实取一帧，验证数据通路真的通 ----
     * 注意：V100 系列的 API 是 HI_BOOL bBlock，不是 V200 的超时毫秒。
     * 这里用非阻塞，避免 AI 无数据时把进程卡死。 */
    memset(&frame, 0, sizeof(frame));
    memset(&aec, 0, sizeof(aec));
    ret = HI_MPI_AI_GetFrame(AI_DEV, AI_CHN, &frame, &aec, HI_FALSE);
    report("AI_GetFrame(nowait)", ret);
    if (ret == 0) {
        printf("     frame: len=%u seq=%u ts=%llu virt=%p\n",
               (unsigned)frame.u32Len, (unsigned)frame.u32Seq,
               (unsigned long long)frame.u64TimeStamp, frame.pVirAddr[0]);
        HI_MPI_AI_ReleaseFrame(AI_DEV, AI_CHN, &frame, &aec);
        printf("     AI_ReleaseFrame done\n");
    }

    /* ---- 6. 清理 ---- */
    HI_MPI_AO_DisableChn(AO_DEV, AO_CHN);
    HI_MPI_AO_Disable(AO_DEV);
    HI_MPI_AI_DisableChn(AI_DEV, AI_CHN);
    HI_MPI_AI_Disable(AI_DEV);
    HI_MPI_SYS_Exit();

    printf("=== p0_audio_probe end (%d steps) ===\n", g_step);
    return 0;
}
