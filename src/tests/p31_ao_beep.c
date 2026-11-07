/*
 * p31_ao_beep.c —— 直接向 AO 通道送帧，播放"哔哔"节奏音
 *
 * 依据（hi3518_ao.ko 反编译 + /proc/umap/ao 实测）
 *   AOPutFrame 的准入条件均已满足：
 *     frame.enBitwidth == dev.enBitwidth        (16bit)
 *     frame.u32Len     == dev.u32Len            (320点 mono = 640B)
 *     frame.u32Len     <  0x2001
 *     AoChn state      == enable
 *   数据由 __copy_from_user(dma_buf, frame.pVirAddr[0], u32Len) 搬入环形缓冲，
 *   不要求 pVirAddr/PhyAddr 必须来自 VB 池。
 *
 *   所以本程序不 SetPubAttr、不 Enable、不 Disable，只 SendFrame ——
 *   和 rmm 播提示音走的是同一条路。
 *
 * 音源：0.3s 响 + 0.2s 静，重复 8 次（共 4s），1kHz，幅度 12000（约 -8.7dBFS）。
 * 这样即便环境嘈杂也能分辨，且不会和正常语音混淆。
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
#define PTS      320            /* 320 点 @8k = 40ms/帧 */
#define FRAMES   100            /* 100 帧 = 4 秒 */

static short g_buf[PTS];

/* 0: 响  1: 静   —— 300ms 响 / 200ms 静 */
static int pattern(int frame_idx)
{
    int ms = frame_idx * 40;        /* 该帧起始时刻 */
    return ((ms % 500) < 300) ? 0 : 1;
}

int main(void)
{
    HI_S32         ret;
    AUDIO_FRAME_S  f;
    int i, k, ok = 0, fail = 0;

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== p31_ao_beep begin ===\n");

    ret = HI_MPI_SYS_Init();
    printf("SYS_Init  =0x%08X\n", (unsigned)ret);
    if (ret != 0) { printf("ABORT\n"); return 1; }

    for (i = 0; i < FRAMES; i++) {
        int silent = pattern(i);
        for (k = 0; k < PTS; k++) {
            if (silent) {
                g_buf[k] = 0;
            } else {
                /* 1kHz：8 个采样一个周期，三角波近似
                   相位从帧内绝对位置推出，避免帧边界出现台阶 */
                int ph = ((i * PTS) + k) % 8;
                int v  = (ph < 4) ? (ph * 4000) : ((8 - ph) * 4000);
                g_buf[k] = (short)(v - 8000);   /* -8000..+8000 */
            }
        }

        memset(&f, 0, sizeof(f));
        f.enBitwidth   = AUDIO_BIT_WIDTH_16;
        f.enSoundmode  = AUDIO_SOUND_MODE_MONO;
        f.pVirAddr[0]  = g_buf;
        f.u32Len       = PTS * 2;                /* 640 字节 */
        f.u64TimeStamp = (HI_U64)i * 40;
        f.u32Seq       = (HI_U32)i;

        ret = HI_MPI_AO_SendFrame(AO_DEV, AO_CHN, &f, HI_TRUE);
        if (ret == 0) ok++; else fail++;
        if (i < 2 || ret != 0)
            printf("[%02d] SendFrame=0x%08X\n", i, (unsigned)ret);

        usleep(40000);              /* 严格按 40ms 节拍喂，和 DMA 同速 */
    }

    printf("--- summary ok=%d fail=%d (expect ok=100) ---\n", ok, fail);
    printf("=== p31_ao_beep end ===\n");
    return 0;
}
