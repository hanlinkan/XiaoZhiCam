/*
 * p23_adec.c -- 按原厂路径播放：PCM -> AENC(G.726) -> ADEC_SendStream -> ADEC -> AO
 *
 * 逆向依据（REA 反编译 rmm_fast3 <send_ao_beep>）：
 *     FILE *s = fopen(path,"rb");
 *     while (fread(buf,1,0x50,s) == 0x50) {          // 每包 80 字节 G.726
 *         AUDIO_STREAM_S st;
 *         st.pStream  = <4字节头 {0,1,40,0} + 80字节数据>;
 *         st.u32Len   = 0x54;                        // 84
 *         HI_MPI_ADEC_SendStream(0, &st, 1);
 *         usleep(23000);
 *     }
 * 以及 <rmm_init_ao>：
 *     AO_DisableChn/Disable -> SetPubAttr(8000/16bit/i2s_master/mono/ex=1/frm=30/pt=320/ch=2/clk=1)
 *     -> Enable -> EnableChn -> SYS_Bind(src={ADEC,0,0}, dst={AO,0,0})
 *
 * 也就是说 AO 的 8kHz 数据源是硬件 ADEC，用户态 SendFrame 无效。
 * 本程序把 1kHz 正弦 PCM 编码成 G.726-32K，再按上面的包格式送 ADEC。
 *
 * 用法: p23_adec [秒数]   默认 6 秒
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>

#include "hi_type.h"
#include "hi_common.h"
#include "hi_comm_aio.h"
#include "hi_comm_aenc.h"
#include "hi_comm_adec.h"
#include "mpi_sys.h"
#include "mpi_ai.h"
#include "mpi_aenc.h"
#include "mpi_adec.h"

#define AI_DEV   0
#define AI_CHN   0
#define AO_DEV   0
#define AO_CHN   0
#define ADEC_CHN 0
#define FRAME    160          /* 20ms @ 8kHz */

static const short sine8[8] = { 0, 8485, 12000, 8485, 0, -8485, -12000, -8485 };

static long long now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

int main(int argc, char **argv)
{
    MPP_SYS_CONF_S sc;
    AIO_ATTR_S attr;
    AENC_CHN_ATTR_S aattr;
    AENC_ATTR_G726_S g726;
    AUDIO_FRAME_S f, of;
    AEC_FRAME_S ref;
    AUDIO_STREAM_S ast;
    MPP_CHN_S src, dst;
    HI_S32 ret;
    int aifd, chn, phase = 0, i;
    int seconds = (argc > 1) ? atoi(argv[1]) : 6;
    long long t0, next, last_print = 0;
    unsigned long long sq = 0;
    long nwin = 0;
    int pk = 0, nframes = 0, nbytes = 0, efail = 0;

    printf("=== p23 ADEC path probe (AENC G.726 -> ADEC -> AO) ===\n");
    memset(&sc, 0, sizeof(sc));
    sc.u32AlignWidth = 64;
    HI_MPI_SYS_SetConf(&sc);
    ret = HI_MPI_SYS_Init();
    printf("SYS_Init          ret=0x%08X\n", (unsigned)ret);

    /* 照 rmm_init_adec 的顺序：UnBind -> DestroyChn -> CreateChn -> 再 Bind */
    memset(&src, 0, sizeof(src));
    memset(&dst, 0, sizeof(dst));
    src.enModId = HI_ID_ADEC; src.s32DevId = 0; src.s32ChnId = 0;
    dst.enModId = HI_ID_AO;   dst.s32DevId = 0; dst.s32ChnId = 0;
    ret = HI_MPI_SYS_UnBind(&src, &dst);
    printf("UnBind(ADEC->AO)  ret=0x%08X\n", (unsigned)ret);
    ret = HI_MPI_ADEC_DestroyChn(ADEC_CHN);
    printf("ADEC_DestroyChn   ret=0x%08X\n", (unsigned)ret);

    {
        ADEC_CHN_ATTR_S dattr;
        ADEC_ATTR_G726_S dg;
        memset(&dattr, 0, sizeof(dattr));
        memset(&dg, 0, sizeof(dg));
        dg.enG726bps = G726_32K;
        dattr.enType     = PT_G726;         /* 21 */
        dattr.u32BufSize = 0x14;            /* 20 */
        dattr.enMode     = 0;
        dattr.pValue     = &dg;
        ret = HI_MPI_ADEC_CreateChn(ADEC_CHN, &dattr);
        printf("ADEC_CreateChn(0) ret=0x%08X %s\n", (unsigned)ret,
               ret == 0 ? "OK" : "FAIL");
    }
    ret = HI_MPI_SYS_Bind(&src, &dst);
    printf("Bind(ADEC->AO)    ret=0x%08X\n", (unsigned)ret);

    /* AI：只 Enable，不动 acodec */
    memset(&attr, 0, sizeof(attr));
    HI_MPI_AI_GetPubAttr(AI_DEV, &attr);
    HI_MPI_AI_Enable(AI_DEV);
    HI_MPI_AI_EnableChn(AI_DEV, AI_CHN);
    aifd = HI_MPI_AI_GetFd(AI_DEV, AI_CHN);
    printf("ai_fd=%d\n", aifd);

    /* AENC: G.726 32kbps */
    memset(&aattr, 0, sizeof(aattr));
    memset(&g726, 0, sizeof(g726));
    g726.enG726bps = G726_32K;
    aattr.enType   = PT_G726;
    aattr.u32BufSize = 4;
    aattr.pValue   = &g726;
    for (chn = 0; chn < 6; chn++) {
        ret = HI_MPI_AENC_CreateChn(chn, &aattr);
        printf("AENC_CreateChn(%d) ret=0x%08X\n", chn, (unsigned)ret);
        if (ret == 0) break;
    }
    if (ret != 0) { printf("!! 所有 AENC 通道都被占用\n"); return 2; }

    printf("\n计划: 0-1s 静音, 1-4s 1kHz, 4-%ds 静音\n", seconds);
    printf("%-7s %-9s %-9s %-7s %-8s %s\n", "t(s)", "aiRMS", "peak", "frames", "bytes", "");
    fflush(stdout);

    t0 = now_ms();
    next = t0;
    while (now_ms() - t0 < (long long)seconds * 1000) {
        long long now = now_ms() - t0;

        if (now_ms() >= next) {
            short pcm[FRAME];
            int tone = (now >= 1000 && now < 4000);
            for (i = 0; i < FRAME; i++) { pcm[i] = tone ? sine8[phase & 7] : 0; phase++; }

            memset(&of, 0, sizeof(of));
            of.u32Len = FRAME * sizeof(short);
            of.pVirAddr[0] = (HI_U8 *)pcm;
            of.enBitwidth = AUDIO_BIT_WIDTH_16;
            of.enSoundmode = AUDIO_SOUND_MODE_MONO;
            ret = HI_MPI_AENC_SendFrame(chn, &of, NULL);
            if (ret != 0) {
                if (efail < 5) printf("  [warn] AENC_SendFrame=0x%08X\n", (unsigned)ret);
                efail++;
            }

            /* 取出编码后的码流，按 rmm 的包格式送 ADEC */
            for (;;) {
                unsigned char obuf[512];
                memset(&ast, 0, sizeof(ast));
                ret = HI_MPI_AENC_GetStream(chn, &ast, HI_FALSE);
                if (ret != 0 || ast.u32Len == 0 || ast.u32Len > 400) {
                    if (ret == 0 && ast.u32Len) HI_MPI_AENC_ReleaseStream(chn, &ast);
                    break;
                }
                obuf[0] = 0; obuf[1] = 1; obuf[2] = 40; obuf[3] = 0;
                memcpy(obuf + 4, ast.pStream, ast.u32Len);
                {
                    AUDIO_STREAM_S s2;
                    memset(&s2, 0, sizeof(s2));
                    s2.pStream  = obuf;
                    s2.u32Len   = ast.u32Len + 4;
                    s2.u32Seq   = 1;
                    ret = HI_MPI_ADEC_SendStream(ADEC_CHN, &s2, HI_TRUE);
                    if (ret != 0 && (now / 20) % 50 == 0)
                        printf("  [warn] ADEC_SendStream=0x%08X len=%u\n",
                               (unsigned)ret, (unsigned)s2.u32Len);
                    nframes++;
                    nbytes += (int)s2.u32Len;
                }
                HI_MPI_AENC_ReleaseStream(chn, &ast);
            }
            next += 20;
            if (next < now_ms() - 200) next = now_ms();
        }

        /* 同时采集，确认麦克风听得到（噪声数据只作参考） */
        {
            fd_set rfds; struct timeval tv;
            FD_ZERO(&rfds); FD_SET(aifd, &rfds);
            tv.tv_sec = 0; tv.tv_usec = 8000;
            if (select(aifd + 1, &rfds, NULL, NULL, &tv) > 0) {
                memset(&f, 0, sizeof(f)); memset(&ref, 0, sizeof(ref));
                if (HI_MPI_AI_GetFrame(AI_DEV, AI_CHN, &f, &ref, HI_FALSE) == 0) {
                    if (f.pVirAddr[0] && f.u32Len > 0) {
                        short *p = (short *)f.pVirAddr[0];
                        int n = (int)(f.u32Len / sizeof(short)), k;
                        for (k = 0; k < n; k++) {
                            int v = p[k], t = v >> 4;
                            sq += (unsigned)(t * t);
                            if (v < 0) { if (-v > pk) pk = -v; }
                            else if (v > pk) pk = v;
                        }
                        nwin += n;
                    }
                    HI_MPI_AI_ReleaseFrame(AI_DEV, AI_CHN, &f, &ref);
                }
            }
        }

        if (now - last_print >= 250) {
            last_print = now;
            if (nwin > 0) {
                unsigned x = (unsigned)(sq * 256 / (unsigned long long)nwin);
                unsigned r = 0, b = 1u << 30;
                while (b > x) b >>= 2;
                while (b) { if (x >= r + b) { x -= r + b; r = (r >> 1) + b; } else r >>= 1; b >>= 2; }
                printf("t=%-6.2f %-9u %-9d %-7d %-8d %s\n", now / 1000.0, r, pk,
                       nframes, nbytes, (now >= 1000 && now < 4000) ? "TONE" : "");
                fflush(stdout);
                sq = 0; nwin = 0; pk = 0;
            }
        }
    }

    HI_MPI_AENC_DestroyChn(chn);
    printf("=== done: frames=%d bytes=%d ===\n", nframes, nbytes);
    return 0;
}
