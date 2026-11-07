/*
 * p32_ao_state.c —— 用 AOQueryChnState 直接看环形队列占用
 *
 * 目的：判明下面两种情况到底是哪一种
 *   A) 我们送的帧根本没进环形缓冲  -> 送完后 busy 仍为 0
 *   B) 帧进了缓冲但 DMA 不消费      -> 送完后 busy = 20 且不下降
 *
 * 背景（hi3518_ao.ko 反编译）：
 *   AOPutFrame 把数据拷到 ctx + 0x10 + write*0x18 指向的 VA，
 *   AODmaCallback 用 ctx + 0x04 + read*0x18 的 phys 做 DMA。
 *   两者按槽位对应，理论上应当是同一块缓冲。
 *   但实测 /proc/umap/ao 的 BufEmp（欠载计数）在我们送帧期间仍持续上涨，
 *   说明 DMA 每次回调都认为队列为空 —— 与静态分析矛盾，故实测之。
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "hi_type.h"
#include "hi_common.h"
#include "hi_comm_aio.h"
#include "mpi_sys.h"
#include "mpi_ao.h"

#define AO_DEV   0
#define AO_CHN   0
#define PTS      320
#define NFRAME   20

static short g_buf[PTS];

static void show(const char *tag)
{
    AO_CHN_STATE_S st;
    HI_S32 ret;

    memset(&st, 0, sizeof(st));
    ret = HI_MPI_AO_QueryChnStat(AO_DEV, AO_CHN, &st);
    printf("%-10s QueryChnStat=0x%08X  total=%u free=%u busy=%u\n",
           tag, (unsigned)ret,
           (unsigned)st.u32ChnTotalNum,
           (unsigned)st.u32ChnFreeNum,
           (unsigned)st.u32ChnBusyNum);
}

int main(void)
{
    HI_S32        ret;
    AUDIO_FRAME_S f;
    int i, k, ok = 0, fail = 0;

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== p32_ao_state begin ===\n");

    ret = HI_MPI_SYS_Init();
    printf("SYS_Init  =0x%08X\n", (unsigned)ret);
    if (ret != 0) return 1;

    show("before");

    for (i = 0; i < NFRAME; i++) {
        for (k = 0; k < PTS; k++) {
            int ph = ((i * PTS) + k) % 8;
            int v  = (ph < 4) ? (ph * 4000) : ((8 - ph) * 4000);
            g_buf[k] = (short)(v - 8000);
        }
        memset(&f, 0, sizeof(f));
        f.enBitwidth   = AUDIO_BIT_WIDTH_16;
        f.enSoundmode  = AUDIO_SOUND_MODE_MONO;
        f.pVirAddr[0]  = g_buf;
        f.u32Len       = PTS * 2;
        f.u64TimeStamp = (HI_U64)i * 40;
        f.u32Seq       = (HI_U32)i;
        ret = HI_MPI_AO_SendFrame(AO_DEV, AO_CHN, &f, HI_FALSE);
        if (ret == 0) ok++; else fail++;
    }
    printf("send done ok=%d fail=%d\n", ok, fail);

    show("after");
    sleep(1);
    show("after1s");
    sleep(2);
    show("after3s");

    printf("=== p32_ao_state end ===\n");
    return 0;
}
