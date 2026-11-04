/*
 * p11_tone_loop.c -- 声学回环自检：一边播 1kHz 音，一边采 AI。
 *
 * 目的：确认摄像头麦克风到底有没有信号。
 *   - 如果采集频谱在 1kHz 处有明显峰值，说明 mic + speaker 都活着；
 *   - 如果只有 50Hz 工频，没有任何 1kHz，说明 mic 通路/静音/增益有问题。
 *
 * 不 kill rmm，沿用 rmm 已配置的 8k/mono/320 点参数。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <sys/time.h>

#include "hi_type.h"
#include "hi_common.h"
#include "hi_comm_aio.h"
#include "mpi_sys.h"
#include "mpi_ai.h"
#include "mpi_ao.h"
#include "acodec.h"

#define AI_DEV      0
#define AI_CHN      0
#define AO_DEV      0
#define AO_CHN      0
#define SYS_ALIGN   64
#define RATE        8000
#define FRAME       320
#define TONE_HZ     1000
#define TONE_AMP    12000
#define CAP_SECONDS 6
#define TONE_START_MS 1000
#define TONE_MS     2000

static volatile int g_run = 1;
static FILE *g_fp;
static long g_frames, g_samples;
static long long g_sum_sq;
static AI_VQE_CONFIG_S g_vqe;
static int g_vqe_restore;

static long long now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static void *cap_thread(void *arg)
{
    AUDIO_FRAME_S f;
    AEC_FRAME_S ref;
    int fd = HI_MPI_AI_GetFd(AI_DEV, AI_CHN);
    (void)arg;
    printf("[cap] fd=%d\n", fd);

    while (g_run) {
        fd_set rfds;
        struct timeval tv;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        tv.tv_sec = 0; tv.tv_usec = 200000;
        if (select(fd + 1, &rfds, NULL, NULL, &tv) <= 0) continue;

        memset(&f, 0, sizeof(f));
        memset(&ref, 0, sizeof(ref));
        if (HI_MPI_AI_GetFrame(AI_DEV, AI_CHN, &f, &ref, HI_FALSE) != 0) continue;
        if (f.pVirAddr[0] && f.u32Len > 0) {
            short *p = (short *)f.pVirAddr[0];
            int n = (int)(f.u32Len / sizeof(short));
            int i;
            fwrite(p, 1, f.u32Len, g_fp);
            g_frames++;
            g_samples += n;
            for (i = 0; i < n; i++)
                g_sum_sq += (long long)p[i] * p[i];
        }
        HI_MPI_AI_ReleaseFrame(AI_DEV, AI_CHN, &f, &ref);
    }
    return NULL;
}

int main(void)
{
    AIO_ATTR_S attr;
    MPP_SYS_CONF_S sc;
    AUDIO_FRAME_S f;
    pthread_t th;
    short buf[FRAME];
    int i, k, ret;
    int acodec = -1;
    long long t0;
    ACODEC_VOL_CTRL vol;

    printf("=== p11 tone loop ===\n");
    memset(&sc, 0, sizeof(sc));
    sc.u32AlignWidth = SYS_ALIGN;
    printf("SYS_SetConf ret=%d\n", (int)HI_MPI_SYS_SetConf(&sc));
    ret = HI_MPI_SYS_Init();
    printf("SYS_Init ret=%d\n", (int)ret);
    if (ret) return 1;

    memset(&attr, 0, sizeof(attr));
    printf("AI_GetPubAttr ret=%d\n", (int)HI_MPI_AI_GetPubAttr(AI_DEV, &attr));
    printf("AI rate=%d bw=%d sound=%d pt=%u ch=%u\n",
           (int)attr.enSamplerate, (int)attr.enBitwidth,
           (int)attr.enSoundmode, (unsigned)attr.u32PtNumPerFrm,
           (unsigned)attr.u32ChnCnt);
    printf("AI_Enable ret=%d\n", (int)HI_MPI_AI_Enable(AI_DEV));
    printf("AI_EnableChn ret=%d\n", (int)HI_MPI_AI_EnableChn(AI_DEV, AI_CHN));

    {
        memset(&g_vqe, 0, sizeof(g_vqe));
        ret = HI_MPI_AI_GetVqeAttr(AI_DEV, AI_CHN, &g_vqe);
        printf("AI_GetVqeAttr ret=%d aec=%d anr=%d alc=%d rate=%d frm=%d "
               "alc(%d..%d maxgain=%d)\n",
               (int)ret, (int)g_vqe.bAecOpen, (int)g_vqe.bAnrOpen,
               (int)g_vqe.bAlcOpen, (int)g_vqe.s32SampleRate,
               (int)g_vqe.s32FrameSample, (int)g_vqe.stAlcCfg.s32MinLev,
               (int)g_vqe.stAlcCfg.s32MaxLev, (int)g_vqe.stAlcCfg.u32MaxGain);
        ret = HI_MPI_AI_DisableVqe(AI_DEV, AI_CHN);
        printf("AI_DisableVqe ret=%d (VQE 关闭后再采集)\n", (int)ret);
        g_vqe_restore = (ret == 0);
    }

    memset(&attr, 0, sizeof(attr));
    printf("AO_GetPubAttr ret=%d\n", (int)HI_MPI_AO_GetPubAttr(AO_DEV, &attr));
    printf("AO rate=%d bw=%d sound=%d pt=%u ch=%u\n",
           (int)attr.enSamplerate, (int)attr.enBitwidth,
           (int)attr.enSoundmode, (unsigned)attr.u32PtNumPerFrm,
           (unsigned)attr.u32ChnCnt);
    printf("AO_Enable ret=%d\n", (int)HI_MPI_AO_Enable(AO_DEV));
    printf("AO_EnableChn ret=%d\n", (int)HI_MPI_AO_EnableChn(AO_DEV, AO_CHN));

    {
        int db = 0;
        ret = HI_MPI_AO_SetVolume(AO_DEV, AO_CHN, db);
        printf("AO_SetVolume(0dB) ret=%d\n", (int)ret);
        ret = HI_MPI_AO_GetVolume(AO_DEV, AO_CHN, &db);
        printf("AO_GetVolume ret=%d db=%d\n", (int)ret, db);
    }

    acodec = open("/dev/acodec", O_RDWR);
    if (acodec >= 0) {
        unsigned int outvol = 0x60;
        vol.vol_ctrl = 0x60;
        vol.vol_ctrl_mute = 0;
        printf("acodec DACL_VOL=0x60 ret=%d\n",
               (int)ioctl(acodec, ACODEC_SET_DACL_VOL, &vol));
        printf("acodec DACR_VOL=0x60 ret=%d\n",
               (int)ioctl(acodec, ACODEC_SET_DACR_VOL, &vol));
        printf("acodec OUTPUT_VOL=0x60 ret=%d\n",
               (int)ioctl(acodec, ACODEC_SET_OUTPUT_VOL, &outvol));
    } else {
        printf("open acodec failed errno=%d\n", errno);
    }

    g_fp = fopen("/tmp/c/loop.raw", "wb");
    if (!g_fp) { printf("open loop.raw failed\n"); return 2; }
    pthread_create(&th, NULL, cap_thread, NULL);

    t0 = now_ms();
    while (now_ms() - t0 < CAP_SECONDS * 1000) {
        long long el = now_ms() - t0;
        if (el >= TONE_START_MS && el < TONE_START_MS + TONE_MS) {
            static int phase;
            for (k = 0; k < FRAME; k++) {
                /* 1kHz 正弦查表（8k/1k = 8 点一周期） */
                static const short sine8[8] = { 0, 8485, 12000, 8485,
                                                0, -8485, -12000, -8485 };
                buf[k] = sine8[phase & 7];
                phase++;
            }
            memset(&f, 0, sizeof(f));
            f.enBitwidth = AUDIO_BIT_WIDTH_16;
            f.enSoundmode = AUDIO_SOUND_MODE_MONO;
            f.pVirAddr[0] = buf;
            f.u32Len = FRAME * sizeof(short);
            ret = HI_MPI_AO_SendFrame(AO_DEV, AO_CHN, &f, HI_FALSE);
            if (ret != 0) printf("AO_SendFrame ret=%d\n", (int)ret);
        }
        usleep(40000);
    }

    g_run = 0;
    pthread_join(th, NULL);
    fclose(g_fp);
    if (g_vqe_restore) {
        int r = HI_MPI_AI_SetVqeAttr(AI_DEV, AI_CHN, AO_DEV, AO_CHN, &g_vqe);
        printf("AI_SetVqeAttr(restore) ret=%d\n", r);
        r = HI_MPI_AI_EnableVqe(AI_DEV, AI_CHN);
        printf("AI_EnableVqe(restore) ret=%d\n", r);
    }
    if (acodec >= 0) close(acodec);

    printf("frames=%ld samples=%ld\n", g_frames, g_samples);
    if (g_samples) {
        double ms = (double)g_sum_sq / (double)g_samples;
        double y = ms > 0 ? ms / 2.0 : 0;
        for (i = 0; i < 25; i++) y = (y + ms / y) / 2.0;
        printf("overall RMS=%.0f (%.1f%%)\n", y, y * 100.0 / 32767.0);
    }
    /* 不 Disable，避免影响 rmm */
    return 0;
}
