/*
 * p35_adec_rmm.c —— 完全照 rmm 的做法驱动 ADEC 播放
 *
 * 依据（逆向记录 + hi3518_adec.ko / audio_voice_adp.o 反编译）
 *   rmm_init_ao  : AO_SetPubAttr{8k/16bit/i2s_master/mono/ex=1/frm=30/pt=320/ch=2/clk=1}
 *                  -> AO_Enable -> AO_EnableChn(0,0) -> SYS_Bind(ADEC0 -> AO0)
 *   rmm_init_adec: ADEC_CreateChn(0, {PT_G726, pValue={4}}) -> Bind
 *   send_ao_beep : 每包 80 字节 G.726，套 4 字节头 {0,1,40,0}，u32Len=84，
 *                  ADEC_SendStream(0, &st, 1)，usleep(23000)
 *
 *   头部的关键在 byte[2] = 40：DecodeG726Frm 里
 *       bVar2 = *(byte*)(in + 2);  total = (bVar2 + 2) * 2;
 *   所以 40 -> 84 字节消费量，正好是 4 字节头 + 80 字节载荷。
 *   之前我们送裸 G.726，byte[2] 是码流数据，算出的长度乱掉 -> 0xA0188040。
 *
 * 本程序同时把之前 p22 解掉的 ADEC->AO 绑定重新建起来（rmm 的 TTS 依赖它）。
 *
 * 用法: p35 [g726文件] [循环次数]
 *   默认 /home/forbidden.g726.orig，播 1 遍
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include "hi_type.h"
#include "hi_common.h"
#include "hi_comm_aio.h"
#include "hi_comm_adec.h"
#include "mpi_sys.h"
#include "mpi_adec.h"

#define ADEC_CHN   0
#define PKT_DATA   80          /* 每包 G.726 载荷字节数 */
#define PKT_TOTAL  (PKT_DATA + 4)
#define HALF_LEN   40          /* byte[2] = 载荷的一半 */

int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : "/home/forbidden.g726.orig";
    int loops = (argc > 2) ? atoi(argv[2]) : 1;
    HI_S32 ret;
    ADEC_CHN_ATTR_S attr;
    ADEC_ATTR_G726_S g726;
    MPP_CHN_S src, dst;
    AUDIO_STREAM_S st;
    unsigned char pkt[PKT_TOTAL];
    unsigned char data[PKT_DATA];
    int fd, n, pktno, ok = 0, fail = 0, first_err = 0, L;

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== p35_adec_rmm begin (%s x%d) ===\n", path, loops);

    ret = HI_MPI_SYS_Init();
    printf("SYS_Init        =0x%08X\n", (unsigned)ret);

    /* 恢复 rmm 依赖的绑定（p22 曾 UnBind 过） */
    memset(&src, 0, sizeof(src));
    memset(&dst, 0, sizeof(dst));
    src.enModId = HI_ID_ADEC; src.s32DevId = 0; src.s32ChnId = 0;
    dst.enModId = HI_ID_AO;   dst.s32DevId = 0; dst.s32ChnId = 0;
    ret = HI_MPI_SYS_Bind(&src, &dst);
    printf("Bind(ADEC->AO)  =0x%08X\n", (unsigned)ret);

    /* 通道可能已被 rmm 建好；已存在（0xA0188004）就复用 */
    /* u32BufSize 有合法上限（2048 被拒）。rmm 用的是 20，这里按 20/30/100/... 依次试探，
       直到成功或返回 EXIST（已被 rmm 建好，可直接复用）。 */
    {
        static const HI_U32 sizes[] = { 20, 30, 40, 60, 100, 200, 300, 500, 1024 };
        int i;
        ret = 0xFFFFFFFFu;
        for (i = 0; i < (int)(sizeof(sizes) / sizeof(sizes[0])); i++) {
            memset(&g726, 0, sizeof(g726));
            g726.enG726bps = (G726_BPS_E)4;    /* 4bit = 32kbps，同 rmm 的 pValue={4} */
            memset(&attr, 0, sizeof(attr));
            attr.enType     = PT_G726;
            attr.u32BufSize = sizes[i];
            attr.enMode     = ADEC_MODE_PACK;
            attr.pValue     = &g726;
            ret = HI_MPI_ADEC_CreateChn(ADEC_CHN, &attr);
            printf("CreateChn(buf=%u) =0x%08X\n", (unsigned)sizes[i], (unsigned)ret);
            if (ret == 0 || ret == 0xA0188004u) break;
        }
        if (ret == 0xA0188004u) printf("  (already exist, reuse)\n");
        if (ret != 0 && ret != 0xA0188004u) printf("  !! no acceptable u32BufSize\n");
    }

    fd = open(path, O_RDONLY);
    if (fd < 0) { printf("open %s failed errno=%d\n", path, errno); return 1; }
    printf("open ok\n");

    for (L = 0; L < loops; L++) {
        lseek(fd, 0, SEEK_SET);
        pktno = 0;
        while ((n = read(fd, data, PKT_DATA)) > 0) {
            if (n < PKT_DATA) break;            /* 尾部残包丢弃 */

            pkt[0] = 0; pkt[1] = 1; pkt[2] = HALF_LEN; pkt[3] = 0;
            memcpy(pkt + 4, data, PKT_DATA);

            memset(&st, 0, sizeof(st));
            st.pStream      = pkt;
            st.u32PhyAddr   = 0;
            st.u32Len       = PKT_TOTAL;
            st.u64TimeStamp = (HI_U64)pktno * 160;
            st.u32Seq       = (HI_U32)pktno;

            ret = HI_MPI_ADEC_SendStream(ADEC_CHN, &st, HI_TRUE);
            if (ret == 0) {
                ok++;
            } else {
                fail++;
                if (!first_err) {
                    first_err = 1;
                    printf("[ERR] pkt %d ret=0x%08X\n", pktno, (unsigned)ret);
                }
            }
            pktno++;
            usleep(23000);                      /* rmm 就是这个节奏：约 20ms 一包 */
        }
    }

    printf("--- summary ok=%d fail=%d ---\n", ok, fail);
    printf("=== p35_adec_rmm end ===\n");
    return 0;
}
