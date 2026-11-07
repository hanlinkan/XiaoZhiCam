/*
 * ai16k_setup.c —— 抢在 rmm 死后把 AI 设成 16 kHz，然后退出
 *
 * 为什么要独立成小程序
 *   实测：rmm 被 SIGKILL 后，AI_SetPubAttr 只在很短的窗口内可用。
 *     p28b 在 kill 后 1 秒内调用 -> SetPubAttr=0，读回 16000
 *     p8s  因启动耗时 4~6 秒        -> SetPubAttr=0xA0158009 NOT_PERM
 *   所以由 solo16k.sh 在 kill 后立刻调用本程序，先把静态属性定下来；
 *   p8s 启动后只要 GetPubAttr 读到 16000 再 Enable 就行，不必再 Set。
 *
 * 边界
 *   不调用 Enable / EnableChn —— 避免把通道锁住，交给 p8s。
 *   不调用 SYS_Exit —— 保持 MPP 状态供 p8s 复用。
 *   不写 flash、不改任何持久配置。
 */

#include <stdio.h>
#include <string.h>

#include "hi_type.h"
#include "hi_common.h"
#include "hi_comm_aio.h"
#include "mpi_sys.h"
#include "mpi_ai.h"
#include "mpi_vb.h"

#define AI_DEV 0

int main(void)
{
    HI_S32     ret;
    AIO_ATTR_S attr;

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== ai16k_setup begin ===\n");

    ret = HI_MPI_SYS_Init();
    printf("SYS_Init    =0x%08X\n", (unsigned)ret);
    if (ret != 0) { printf("ABORT\n"); return 1; }

    ret = HI_MPI_VB_Init();
    printf("VB_Init     =0x%08X\n", (unsigned)ret);

    /* 实测唯一能过的组合：沿用 rmm 的 ch=2/pt=320/clk=1/frm=30，只改采样率 */
    memset(&attr, 0, sizeof(attr));
    attr.enSamplerate   = AUDIO_SAMPLE_RATE_16000;
    attr.enBitwidth     = AUDIO_BIT_WIDTH_16;
    attr.enWorkmode     = AIO_MODE_I2S_MASTER;
    attr.enSoundmode    = AUDIO_SOUND_MODE_MONO;
    attr.u32EXFlag      = 0;
    attr.u32FrmNum      = 30;
    attr.u32PtNumPerFrm = 320;
    attr.u32ChnCnt      = 2;
    attr.u32ClkSel      = 1;

    ret = HI_MPI_AI_SetPubAttr(AI_DEV, &attr);
    printf("SetPubAttr  =0x%08X\n", (unsigned)ret);

    memset(&attr, 0, sizeof(attr));
    if (HI_MPI_AI_GetPubAttr(AI_DEV, &attr) == 0)
        printf("readback    rate=%d ch=%u pt=%u frm=%u clk=%u\n",
               (int)attr.enSamplerate, (unsigned)attr.u32ChnCnt,
               (unsigned)attr.u32PtNumPerFrm, (unsigned)attr.u32FrmNum,
               (unsigned)attr.u32ClkSel);

    printf("=== ai16k_setup end ===\n");
    return 0;
}
