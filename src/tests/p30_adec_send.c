/*
 * p30_adec_send.c —— 验证能否绕开 rmm，直接通过 ADEC 通路播放
 *
 * 背景
 *   p29 已证明 HI_MPI_AO_SendFrame 可以成功入队（30 帧全 OK、DMA 在跑），
 *   但喇叭没有声音。而 rmm 的播放路径是 G.726 -> ADEC 解码 -> AO，
 *   /proc/umap/adec 显示 Chn0 = g726、SendCnt=0（rmm 平时不送）。
 *   怀疑 AO 的 DMA 只从 ADEC 的内部通路取数据，SendFrame 的队列被旁路。
 *
 * 本程序
 *   把原厂提示音 /home/forbidden.g726.orig（9280 B，G.726 16kbps，约 4.6 秒）
 *   按包送进 ADEC Chn0，看喇叭能否出声。
 *   有声音 = ADEC 通路可用 = p8s 可以完全绕开 rmm 自己播。
 *
 * 边界：不 SetPubAttr、不 Enable、不 Disable，只 SendStream。
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "hi_type.h"
#include "hi_common.h"
#include "hi_comm_aio.h"
#include "hi_comm_adec.h"
#include "mpi_sys.h"
#include "mpi_adec.h"
#include "mpi_ao.h"

#define ADEC_CHN   0
#define PKT_SIZE   14           /* CbPackLen-0x10；rmm 也是小包 */
#define PCM_PATH   "/home/forbidden.g726.orig"

int main(void)
{
    HI_S32         ret;
    AUDIO_STREAM_S st;
    ADEC_CHN_ATTR_S aattr;
    ADEC_ATTR_G726_S g726attr;
    unsigned char  buf[PKT_SIZE];
    int fd, n, pkt = 0, ok = 0, fail = 0;

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== p30_adec_send begin ===\n");

    ret = HI_MPI_SYS_Init();
    printf("SYS_Init  =0x%08X\n", (unsigned)ret);
    if (ret != 0) { printf("ABORT\n"); return 1; }

    /* 必须先建通道，否则 SendStream 返回 0xA0188005 EN_ERR_UNEXIST。
       pValue 不能为 NULL —— AdecCheckAttr 会报 0xA0188006 NULL_PTR，
       它要指向 ADEC_ATTR_G726_S 指明码率。rmm 用的是 G726-16（2 bit）。 */
    g726attr.enG726bps = G726_32K;   /* 原厂 forbidden.g726 是 4bit 包，320B = 80ms */

    memset(&aattr, 0, sizeof(aattr));
    aattr.enType     = PT_G726;
    aattr.u32BufSize = 30;     /* 合法上限内取最大；CbPackLen 随之为 30 左右 */
    aattr.enMode     = ADEC_MODE_PACK;
    aattr.pValue     = &g726attr;
    ret = HI_MPI_ADEC_CreateChn(ADEC_CHN, &aattr);
    printf("CreateChn =0x%08X\n", (unsigned)ret);

    fd = open(PCM_PATH, O_RDONLY);
    printf("open %s = %d\n", PCM_PATH, fd);
    if (fd < 0) return 1;

    while ((n = (int)read(fd, buf, PKT_SIZE)) > 0) {
        memset(&st, 0, sizeof(st));
        st.pStream      = buf;        /* 虚拟地址 */
        st.u32PhyAddr   = 0;          /* 用户态缓冲无物理地址 */
        st.u32Len       = (HI_U32)n;
        st.u64TimeStamp = (HI_U64)pkt * 160;
        st.u32Seq       = (HI_U32)pkt;

        ret = HI_MPI_ADEC_SendStream(ADEC_CHN, &st, HI_TRUE);
        if (ret == 0) ok++; else fail++;
        if (pkt < 3 || ret != 0)
            printf("[%02d] SendStream len=%d ret=0x%08X\n", pkt, n, (unsigned)ret);

        /* 这个通道不会自动送 AO —— 必须自己把解码后的 PCM 取出来转发，
           否则缓冲很快满，SendStream 返回 0xA0188040。 */
        {
            AUDIO_FRAME_INFO_S fi;
            memset(&fi, 0, sizeof(fi));
            if (HI_MPI_ADEC_GetFrame(ADEC_CHN, &fi, HI_FALSE) == 0 && fi.pstFrame) {
                HI_MPI_AO_SendFrame(0, 0, fi.pstFrame, HI_FALSE);
                HI_MPI_ADEC_ReleaseFrame(ADEC_CHN, &fi);
            }
        }

        usleep(7000);     /* 14 B @ 2000 B/s = 7 ms */
        pkt++;
        if (pkt > 700) break;
    }
    close(fd);

    printf("--- summary pkt=%d ok=%d fail=%d ---\n", pkt, ok, fail);
    printf("=== p30_adec_send end ===\n");
    return 0;
}
