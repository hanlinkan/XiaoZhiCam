/*
 * p14_acoustic.c -- 声学回环自检（喇叭发声 -> 麦克风采集）
 *
 * 目的：判断“麦克风通路是否活着”，无需人工配合。
 *   喇叭依次播 800 / 1234 / 1777 / 2500 Hz 四个音，
 *   刻意避开 50Hz 工频及其谐波（这些频率都不是 50 的整数倍），
 *   同时采集 AI 原始 PCM 落盘。离线 FFT 只要在对应频点看到峰，
 *   就证明 喇叭 -> 空气 -> 麦克风 -> ADC -> AI 整条链路是通的。
 *
 * 两种 acodec 通路各跑一遍：
 *   A: mixer=MICIN  gain=31 boost=1 adcVol=1
 *   B: mixer=LINEIN gain=16 boost=0 adcVol=1
 *
 * 用法: p14_acoustic <out.raw>
 * 输出：每段切换时打印 MARK t_ms=.. samples=.. event=..
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
#define TICK_US     40000

/* 每段时长（毫秒）：静音 / 音调 / 静音 / 音调 ... */
static const int SEG_MS[]   = { 3000, 2000, 1000, 2000, 1000, 2000, 1000, 2000, 2000 };
static const int SEG_FREQ[] = {    0,  800,    0, 1234,    0, 1777,    0, 2500,    0 };
#define NSEG ((int)(sizeof(SEG_MS) / sizeof(SEG_MS[0])))

static const short sine64[64] = {
        0,   1176,   2341,   3483,   4592,   5657,   6667,   7613,
     8485,   9276,   9978,  10583,  11087,  11483,  11769,  11942,
    12000,  11942,  11769,  11483,  11087,  10583,   9978,   9276,
     8485,   7613,   6667,   5657,   4592,   3483,   2341,   1176,
        0,  -1176,  -2341,  -3483,  -4592,  -5657,  -6667,  -7613,
    -8485,  -9276,  -9978, -10583, -11087, -11483, -11769, -11942,
   -12000, -11942, -11769, -11483, -11087, -10583,  -9978,  -9276,
    -8485,  -7613,  -6667,  -5657,  -4592,  -3483,  -2341,  -1176,
};

static volatile int g_run = 1;
static volatile long g_samples = 0;
static volatile long g_frames = 0;
static FILE *g_fp;

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

    while (g_run) {
        fd_set rfds;
        struct timeval tv;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        tv.tv_sec = 0; tv.tv_usec = 150000;
        if (select(fd + 1, &rfds, NULL, NULL, &tv) <= 0) continue;
        memset(&f, 0, sizeof(f));
        memset(&ref, 0, sizeof(ref));
        if (HI_MPI_AI_GetFrame(AI_DEV, AI_CHN, &f, &ref, HI_FALSE) != 0) continue;
        if (f.pVirAddr[0] && f.u32Len > 0) {
            short *p = (short *)f.pVirAddr[0];
            fwrite(p, 1, f.u32Len, g_fp);
            fflush(g_fp);
            g_frames++;
            g_samples += (long)(f.u32Len / sizeof(short));
        }
        HI_MPI_AI_ReleaseFrame(AI_DEV, AI_CHN, &f, &ref);
    }
    return NULL;
}

/* 按任意频率生成 FRAME 个样本（相位累加 + 64 点表线性插值） */
static void gen_tone(short *out, int freq, unsigned int *phase)
{
    int k;
    unsigned int step = (unsigned int)(((long long)freq * 64 * 65536) / RATE);
    for (k = 0; k < FRAME; k++) {
        unsigned int ix = (*phase >> 16) & 63;
        unsigned int fr = *phase & 0xFFFF;
        int a = sine64[ix];
        int b = sine64[(ix + 1) & 63];
        out[k] = (short)(a + (((b - a) * (int)fr) >> 16));
        *phase += step;
    }
}

static int acodec_fd = -1;

static void ac_u32(unsigned long req, unsigned int v)
{
    if (acodec_fd >= 0) ioctl(acodec_fd, req, &v);
}

static void ac_vol(unsigned long req, unsigned int v)
{
    ACODEC_VOL_CTRL c;
    c.vol_ctrl = v;
    c.vol_ctrl_mute = 0;
    if (acodec_fd >= 0) ioctl(acodec_fd, req, &c);
}

static void apply_cfg(int mixer, int gain, int boost, unsigned int vol)
{
    ac_u32(ACODEC_SET_PD_ADCL, 0);
    ac_u32(ACODEC_SET_PD_ADCR, 0);
    ac_u32(ACODEC_SET_MICL_MUTE, 0);
    ac_u32(ACODEC_SET_MICR_MUTE, 0);
    ac_u32(ACODEC_SET_ADC_HP_FILTER, 0);
    ac_u32(ACODEC_SET_MIXER_MIC, (unsigned int)mixer);
    ac_u32(ACODEC_SET_GAIN_MICL, (unsigned int)gain);
    ac_u32(ACODEC_SET_GAIN_MICR, (unsigned int)gain);
    ac_u32(ACODEC_ENABLE_BOOSTL, (unsigned int)boost);
    ac_u32(ACODEC_ENABLE_BOOSTR, (unsigned int)boost);
    ac_vol(ACODEC_SET_ADCL_VOL, vol);
    ac_vol(ACODEC_SET_ADCR_VOL, vol);
}

int main(int argc, char **argv)
{
    AIO_ATTR_S attr;
    MPP_SYS_CONF_S sc;
    AUDIO_FRAME_S f;
    pthread_t th;
    short buf[FRAME];
    int i, k, ret, seg, cfg;
    long long t0, tick;
    unsigned int phase = 0;
    static const int cfg_mixer[2] = { 1, 0 };
    static const int cfg_gain[2]  = { 31, 16 };
    static const int cfg_boost[2] = { 1, 0 };
    const char *path = (argc > 1) ? argv[1] : "/tmp/hd1/acoustic.raw";

    printf("=== p14 acoustic loopback ===\n");
    memset(&sc, 0, sizeof(sc));
    sc.u32AlignWidth = SYS_ALIGN;
    HI_MPI_SYS_SetConf(&sc);
    ret = HI_MPI_SYS_Init();
    printf("SYS_Init ret=%d\n", (int)ret);
    if (ret) return 1;

    memset(&attr, 0, sizeof(attr));
    HI_MPI_AI_GetPubAttr(AI_DEV, &attr);
    printf("AI rate=%d bw=%d sound=%d pt=%u ch=%u\n",
           (int)attr.enSamplerate, (int)attr.enBitwidth,
           (int)attr.enSoundmode, (unsigned)attr.u32PtNumPerFrm,
           (unsigned)attr.u32ChnCnt);
    printf("AI_Enable ret=%d\n", (int)HI_MPI_AI_Enable(AI_DEV));
    printf("AI_EnableChn ret=%d\n", (int)HI_MPI_AI_EnableChn(AI_DEV, AI_CHN));
    printf("AI_DisableVqe ret=%d\n", (int)HI_MPI_AI_DisableVqe(AI_DEV, AI_CHN));

    memset(&attr, 0, sizeof(attr));
    HI_MPI_AO_GetPubAttr(AO_DEV, &attr);
    printf("AO rate=%d bw=%d sound=%d pt=%u ch=%u\n",
           (int)attr.enSamplerate, (int)attr.enBitwidth,
           (int)attr.enSoundmode, (unsigned)attr.u32PtNumPerFrm,
           (unsigned)attr.u32ChnCnt);
    printf("AO_Enable ret=%d\n", (int)HI_MPI_AO_Enable(AO_DEV));
    printf("AO_EnableChn ret=%d\n", (int)HI_MPI_AO_EnableChn(AO_DEV, AO_CHN));

    acodec_fd = open("/dev/acodec", O_RDWR);
    printf("open acodec fd=%d\n", acodec_fd);

    g_fp = fopen(path, "wb");
    if (!g_fp) { printf("open %s failed errno=%d\n", path, errno); return 2; }
    printf("dump -> %s\n", path);
    fflush(stdout);

    pthread_create(&th, NULL, cap_thread, NULL);

    for (cfg = 0; cfg < 2; cfg++) {
        AO_CHN_STATE_S st;
        memset(&st, 0, sizeof(st));

        apply_cfg(cfg_mixer[cfg], cfg_gain[cfg], cfg_boost[cfg], 1);
        printf("MARK cfg=%d mixer=%d gain=%d boost=%d vol=1 samples=%ld\n",
               cfg, cfg_mixer[cfg], cfg_gain[cfg], cfg_boost[cfg], g_samples);
        fflush(stdout);
        usleep(300000);

        t0 = now_ms();
        tick = 0;
        phase = 0;
        for (seg = 0; seg < NSEG; seg++) {
            long long seg_end = 0;
            int s;
            for (s = 0; s < seg; s++) seg_end += SEG_MS[s];
            printf("MARK cfg=%d seg=%d freq=%d t_ms=%lld samples=%ld\n",
                   cfg, seg, SEG_FREQ[seg], (long long)seg_end, g_samples);
            fflush(stdout);
            while (now_ms() - t0 < seg_end) {
                memset(buf, 0, sizeof(buf));
                if (SEG_FREQ[seg] > 0)
                    gen_tone(buf, SEG_FREQ[seg], &phase);
                memset(&f, 0, sizeof(f));
                f.enBitwidth = AUDIO_BIT_WIDTH_16;
                f.enSoundmode = AUDIO_SOUND_MODE_MONO;
                f.pVirAddr[0] = buf;
                f.u32Len = FRAME * sizeof(short);
                ret = HI_MPI_AO_SendFrame(AO_DEV, AO_CHN, &f, HI_FALSE);
                if (ret != 0 && tick < 3)
                    printf("AO_SendFrame ret=%d\n", (int)ret);
                tick++;
                /* 绝对时刻对齐，避免累计漂移 */
                {
                    long long due = t0 + tick * (TICK_US / 1000);
                    long long d = due - now_ms();
                    if (d > 0) usleep((useconds_t)(d * 1000));
                }
            }
        }
        ret = HI_MPI_AO_QueryChnStat(AO_DEV, AO_CHN, &st);
        printf("MARK cfg=%d total=%u busy=%u ret=%d samples=%ld\n",
               cfg, (unsigned)st.u32ChnTotalNum, (unsigned)st.u32ChnBusyNum,
               (int)ret, g_samples);
        fflush(stdout);
    }

    g_run = 0;
    pthread_join(th, NULL);
    fclose(g_fp);
    if (acodec_fd >= 0) close(acodec_fd);

    printf("frames=%ld samples=%ld\n", g_frames, g_samples);
    printf("=== done ===\n");
    return 0;
}
