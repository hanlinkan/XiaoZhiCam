/*
 * p29_ao_send.c —— 验证能否在 rmm 运行时直接往 AO 通道送帧
 *
 * 依据（/proc/umap/ao 实测）
 *   rmm 已经把 AO 设备配好并 Enable：8k/16bit/2ch/mono/320点/30帧，
 *   AoChn0 State=enable，但 BufEmp 高达 15848 —— 它平时根本不送数据，
 *   只在播提示音那一下才喂几帧。
 *
 * 思路
 *   不 SetPubAttr、不 Enable、不 Disable（这三件事才是之前"没声音 /
 *   破坏 rmm 对讲"的根源），只对已经 enable 的通道直接 SendFrame。
 *   参数必须与 rmm 配好的一致：8k / 16bit / mono / 320 点（40 ms/帧）。
 *
 * 本程序送 1 kHz 正弦波约 1.2 秒，用来判断喇叭有没有声音。
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
#define PTS      320          /* 320 点 @8k = 40 ms */
#define TOTAL    30           /* 30 帧 = 1.2 秒 */

/* 1 kHz 正弦，8 个采样一个周期，幅度 8000（约 -12 dBFS） */
static short g_buf[PTS];

int main(void)
{
    HI_S32        ret;
    AUDIO_FRAME_S f;
    int i, k, ok = 0, fail = 0;

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== p29_ao_send begin ===\n");

    ret = HI_MPI_SYS_Init();
    printf("SYS_Init     =0x%08X\n", (unsigned)ret);
    if (ret != 0) { printf("ABORT\n"); return 1; }

    for (i = 0; i < TOTAL; i++) {
        for (k = 0; k < PTS; k++) {
            int ph = (i * PTS + k) % 8;
            /* 手写三角近似，避免引入 libm */
            int v = (ph < 4) ? (ph * 2000) : ((8 - ph) * 2000);
            g_buf[k] = (short)(v - 4000);
        }

        memset(&f, 0, sizeof(f));
        f.enBitwidth   = AUDIO_BIT_WIDTH_16;
        f.enSoundmode  = AUDIO_SOUND_MODE_MONO;
        f.pVirAddr[0]  = g_buf;
        f.u32Len       = PTS * 2;
        f.u64TimeStamp = (HI_U64)i * 40;
        f.u32Seq       = (HI_U32)i;

        /* V100 的第四个参数是 HI_BOOL bBlock（不是 V200 的超时毫秒）。
           传 1000 会直接返回 0xA0168003 ILLEGAL_PARAM "invalid param: bBlock"。
           这里用阻塞模式，缓冲满时自然会等一会儿。 */
        ret = HI_MPI_AO_SendFrame(AO_DEV, AO_CHN, &f, HI_TRUE);
        if (ret == 0) ok++; else fail++;
        if (i < 3 || ret != 0)
            printf("[%02d] SendFrame  =0x%08X\n", i, (unsigned)ret);

        usleep(35000);   /* 略快于 40ms，让驱动自己缓冲 */
    }

    printf("--- summary ok=%d fail=%d ---\n", ok, fail);
    printf("=== p29_ao_send end ===\n");
    return 0;
}
