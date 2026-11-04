/*
 * p22_ao.c -- 证明 AO 通道被 rmm 用 SYS_Bind 占用，UnBind 后即可正常播放
 *
 * 逆向依据（rmm_fast3 <rmm_init_ao>）：
 *     HI_MPI_AO_DisableChn(0,0)
 *     HI_MPI_AO_Disable(0)
 *     填 AIO_ATTR_S: rate=8000 bitwidth=1(i2s) workmode=0 soundmode=0(mono)
 *                    ex=1 frm=30 pt=320 chn=2 clk=1
 *     SetPubAttr → Enable → EnableChn
 *     HI_MPI_SYS_Bind(src={HI_ID_ADEC=24,0,0}, dst={HI_ID_AO=22,0,0})   <-- 这一步
 *
 * 也就是说 AO 的数据源被绑定成了硬件 ADEC，用户态 SendFrame 的数据到不了喇叭。
 * 这也是为什么原厂 speak（ADEC→AO）能出声、我们的 SendFrame 不能。
 *
 * 本程序：先 UnBind(ADEC→AO)，再 Enable，再送 1kHz，同一时刻用麦克风验证。
 *
 * 用法: p22_ao [db]   默认 0 dB；不要碰任何 acodec 寄存器
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/time.h>

#include "hi_type.h"
#include "hi_common.h"
#include "hi_comm_aio.h"
#include "mpi_sys.h"
#include "mpi_ai.h"
#include "mpi_ao.h"

#define AI_DEV   0
#define AI_CHN   0
#define AO_DEV   0
#define AO_CHN   0
#define FRAME    320

static const short sine8[8] = { 0, 8485, 12000, 8485, 0, -8485, -12000, -8485 };

static long long now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* 8 点周期 1kHz 的 I/Q 相关（Q15） */
static const int cos8[8] = { 32768, 23170, 0, -23170, -32768, -23170, 0, 23170 };
static const int sin8[8] = { 0, 23170, 32768, 23170, 0, -23170, -32768, -23170 };

int main(int argc, char **argv)
{
    MPP_SYS_CONF_S sc;
    AIO_ATTR_S attr;
    AUDIO_FRAME_S f, of;
    AEC_FRAME_S ref;
    MPP_CHN_S src, dst;
    HI_S32 ret, vol = -9999;
    int aifd, phase = 0, want_db = (argc > 1) ? atoi(argv[1]) : 0;
    long long t0, next = 0, last_print = 0;
    long long sq = 0, ci = 0, cq = 0;
    long long nwin = 0;
    int i;

    printf("=== p22 AO unbind probe ===\n");
    memset(&sc, 0, sizeof(sc));
    sc.u32AlignWidth = 64;
    HI_MPI_SYS_SetConf(&sc);
    ret = HI_MPI_SYS_Init();
    printf("SYS_Init        ret=0x%08X\n", (unsigned)ret);

    /* ---- 先解绑 ADEC -> AO ---- */
    memset(&src, 0, sizeof(src));
    memset(&dst, 0, sizeof(dst));
    src.enModId = HI_ID_ADEC; src.s32DevId = 0; src.s32ChnId = 0;
    dst.enModId = HI_ID_AO;   dst.s32DevId = 0; dst.s32ChnId = 0;
    ret = HI_MPI_SYS_UnBind(&src, &dst);
    printf("UnBind(ADEC->AO) ret=0x%08X  %s\n", (unsigned)ret,
           ret == 0 ? "OK 解绑成功" : "未绑定时返回非0也正常");

    /* ---- AI（只 Enable，不改 acodec）---- */
    memset(&attr, 0, sizeof(attr));
    HI_MPI_AI_GetPubAttr(AI_DEV, &attr);
    HI_MPI_AI_Enable(AI_DEV);
    ret = HI_MPI_AI_EnableChn(AI_DEV, AI_CHN);
    printf("AI_EnableChn    ret=0x%08X\n", (unsigned)ret);
    aifd = HI_MPI_AI_GetFd(AI_DEV, AI_CHN);
    printf("ai_fd=%d\n", aifd);

    /* ---- AO：按 rmm 的顺序 Disable → SetPubAttr → Enable → EnableChn ---- */
    HI_MPI_AO_DisableChn(AO_DEV, AO_CHN);
    HI_MPI_AO_Disable(AO_DEV);

    memset(&attr, 0, sizeof(attr));
    attr.enSamplerate   = AUDIO_SAMPLE_RATE_8000;
    attr.enBitwidth     = AUDIO_BIT_WIDTH_16;
    attr.enWorkmode     = AIO_MODE_I2S_MASTER;
    attr.enSoundmode    = AUDIO_SOUND_MODE_MONO;
    attr.u32EXFlag      = 1;
    attr.u32FrmNum      = 30;
    attr.u32PtNumPerFrm = FRAME;
    attr.u32ChnCnt      = 2;
    attr.u32ClkSel      = 1;
    ret = HI_MPI_AO_SetPubAttr(AO_DEV, &attr);
    printf("AO_SetPubAttr   ret=0x%08X\n", (unsigned)ret);
    ret = HI_MPI_AO_Enable(AO_DEV);
    printf("AO_Enable       ret=0x%08X\n", (unsigned)ret);
    ret = HI_MPI_AO_EnableChn(AO_DEV, AO_CHN);
    printf("AO_EnableChn    ret=0x%08X\n", (unsigned)ret);

    ret = HI_MPI_AO_GetVolume(AO_DEV, AO_CHN, &vol);
    printf("GetVolume       ret=0x%08X vol=%d dB\n", (unsigned)ret, (int)vol);
    ret = HI_MPI_AO_SetVolume(AO_DEV, AO_CHN, want_db);
    printf("SetVolume(%d)   ret=0x%08X\n", want_db, (unsigned)ret);

    printf("\n计划: 0-1s 静音, 1-4s 1kHz(幅度12000), 4-5s 静音\n");
    printf("%-8s %-9s %-9s %-8s %s\n", "t(s)", "aiRMS", "1kHz", "sendRet", "tone");
    fflush(stdout);

    t0 = now_ms();
    next = t0;
    for (;;) {
        long long now = now_ms() - t0;
        int sret = 0;
        if (now >= 5000) break;

        if (now_ms() >= next) {
            short buf[FRAME];
            int tone = (now >= 1000 && now < 4000);
            for (i = 0; i < FRAME; i++) { buf[i] = tone ? sine8[phase & 7] : 0; phase++; }
            memset(&of, 0, sizeof(of));
            of.u32Len = FRAME * sizeof(short);
            of.pVirAddr[0] = (HI_U8 *)buf;
            of.enBitwidth = AUDIO_BIT_WIDTH_16;
            of.enSoundmode = AUDIO_SOUND_MODE_MONO;
            ret = HI_MPI_AO_SendFrame(AO_DEV, AO_CHN, &of, HI_TRUE);
            sret = (int)ret;
            next += 40;
            if (next < now_ms() - 200) next = now_ms();
        }

        {
            fd_set rfds; struct timeval tv;
            FD_ZERO(&rfds); FD_SET(aifd, &rfds);
            tv.tv_sec = 0; tv.tv_usec = 10000;
            if (select(aifd + 1, &rfds, NULL, NULL, &tv) > 0) {
                memset(&f, 0, sizeof(f)); memset(&ref, 0, sizeof(ref));
                if (HI_MPI_AI_GetFrame(AI_DEV, AI_CHN, &f, &ref, HI_FALSE) == 0) {
                    if (f.pVirAddr[0] && f.u32Len > 0) {
                        short *p = (short *)f.pVirAddr[0];
                        int n = (int)(f.u32Len / sizeof(short)), k;
                        for (k = 0; k < n; k++) {
                            int v = p[k];
                            int t = (int)((unsigned)v * (unsigned)v >> 6);
                            int ph = (int)(((nwin + k) & 7));
                            sq += t;
                            ci += (long long)v * cos8[ph];
                            cq += (long long)v * sin8[ph];
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
                long long rms = 0, amp = 0, x, r, b;
                x = sq * 64 / nwin;
                r = 0;
                for (b = 1; b < x && b < (1LL << 46); b <<= 2) { }
                while (b) { if (x >= r + b) { x -= r + b; r = (r >> 1) + b; } else r >>= 1; b >>= 2; }
                rms = r;
                x = ci * ci + cq * cq;
                r = 0;
                for (b = 1; b < x && b < (1LL << 46); b <<= 2) { }
                while (b) { if (x >= r + b) { x -= r + b; r = (r >> 1) + b; } else r >>= 1; b >>= 2; }
                amp = r * 2 / nwin;
                printf("t=%-6.2f %-9lld %-9lld %-8d %s\n", now / 1000.0, rms, amp, sret,
                       (now >= 1000 && now < 4000) ? "TONE" : "");
                fflush(stdout);
                sq = 0; ci = 0; cq = 0; nwin = 0;
            }
        }
    }
    printf("=== done ===\n");
    return 0;
}
