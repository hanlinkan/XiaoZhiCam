/*
 * p15_mic.c -- 麦克风电平探针（带 LED 指示、带 CPU 自测）
 *
 * 配置（依据本机实测，不是猜测）：
 *   - 通路保持出厂 LINEIN，**绝不切 MICIN**（MICIN 是空引脚，切过去等于接到空气）
 *   - 只改模拟 PGA：GAIN_MICL/R = 20（出厂 31 时直流 31816、正向余量只剩 951）
 *   - BOOST 关、ADC 数字音量保持出厂 24
 *   - 前端用 afe.h：6阶高通200Hz + 6阶低通3kHz + 125/225Hz 陷波，全 int32
 *
 * 输出：每 250ms 一行  t / 原始RMS / 滤波后RMS / 峰值 / AGC / 门 / 噪声底 / 人声
 *       <out>.raw      滤波后、AGC 前的 16bit PCM（最能反映语音频段真实电平）
 *       <out>.orig.pcm 原始 16bit PCM（万一滤波可疑，可离线重算）
 *
 * 退出时打印本进程自身 CPU 占用（读 /proc/self/stat），用来确认不会压垮这颗单核。
 *
 * 用法: p15_mic [秒数] [out.raw]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <sys/time.h>

#include "hi_type.h"
#include "hi_common.h"
#include "hi_comm_aio.h"
#include "mpi_sys.h"
#include "mpi_ai.h"
#include "acodec.h"
#include "afe.h"
#include "led.h"

#define AI_DEV      0
#define AI_CHN      0
#define SYS_ALIGN   64

/* ---- acodec：只改 PGA，其余保持出厂 ---- */
#define ACODEC_MIXER_LINEIN  0
#define ACODEC_GAIN_MIC      20
#define ACODEC_BOOST         0
#define ACODEC_ADC_VOL       24

static int acfd = -1;
static void ac_u32(unsigned long q, unsigned int v) { if (acfd >= 0) ioctl(acfd, q, &v); }
static void ac_vol(unsigned long q, unsigned int v)
{
    ACODEC_VOL_CTRL c; c.vol_ctrl = v; c.vol_ctrl_mute = 0;
    if (acfd >= 0) ioctl(acfd, q, &c);
}

static int isqrt64(unsigned long long v)
{
    unsigned long long rem = 0, root = 0; int i;
    for (i = 0; i < 32; i++) {
        root <<= 1; rem = (rem << 2) | (v >> 62); v <<= 2;
        if (root < rem) { rem -= root + 1; root += 2; }
    }
    return (int)(root >> 1);
}

/* 读 /proc/self/stat 的 utime+stime（单位 jiffies），返回 0 表示失败 */
static long proc_cpu_jiffies(void)
{
    FILE *f = fopen("/proc/self/stat", "r");
    char buf[1024];
    char *p;
    long utime = 0, stime = 0;
    int field = 0;
    if (!f) return 0;
    if (!fgets(buf, sizeof(buf), f)) { fclose(f); return 0; }
    fclose(f);
    /* 第 2 字段是 (comm)，可能含空格，先跳过右括号 */
    p = strrchr(buf, ')');
    if (!p) return 0;
    p++;
    /* 从第 3 字段开始数到 14(utime) / 15(stime) */
    field = 2;
    while (*p && field < 15) {
        while (*p == ' ') p++;
        if (!*p) break;
        field++;
        if (field == 14) utime = atol(p);
        else if (field == 15) { stime = atol(p); break; }
        while (*p && *p != ' ') p++;
    }
    return utime + stime;
}

static void led_tick_now(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    led_tick(LEDP_LISTEN, (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000);
}

int main(int argc, char **argv)
{
    AIO_ATTR_S attr;
    MPP_SYS_CONF_S sc;
    AUDIO_FRAME_S f;
    AEC_FRAME_S ref;
    FILE *fp, *fpo;
    char opath[256];
    int fd, ret;
    int seconds = (argc > 1) ? atoi(argv[1]) : 30;
    const char *path = (argc > 2) ? argv[2] : "/tmp/hd1/mic.raw";
    static afe_t afe;
    static short outbuf[AFE_MAX_FRAME];
    long long t_start, last_print = 0;
    unsigned int sq_raw = 0;
    long n_raw = 0, nfrm = 0;
    long cpu0 = proc_cpu_jiffies();

    printf("=== p15 mic probe (LINEIN, PGA=%d, dehum P=%d K=%d + HPF200 + LPF3k) ===\n",
           ACODEC_GAIN_MIC, AFE_PERIOD, AFE_DEHUM_SHIFT);
    memset(&sc, 0, sizeof(sc));
    sc.u32AlignWidth = SYS_ALIGN;
    HI_MPI_SYS_SetConf(&sc);
    ret = HI_MPI_SYS_Init();
    printf("SYS_Init ret=%d\n", (int)ret);
    if (ret) return 1;

    memset(&attr, 0, sizeof(attr));
    HI_MPI_AI_GetPubAttr(AI_DEV, &attr);
    printf("AI rate=%d ch=%u pt=%u\n", (int)attr.enSamplerate,
           (unsigned)attr.u32ChnCnt, (unsigned)attr.u32PtNumPerFrm);
    HI_MPI_AI_Enable(AI_DEV);
    printf("AI_EnableChn ret=%d\n", (int)HI_MPI_AI_EnableChn(AI_DEV, AI_CHN));
    fd = HI_MPI_AI_GetFd(AI_DEV, AI_CHN);
    if (fd < 0) { printf("AI_GetFd failed\n"); return 2; }

    if (led_open() != 0) printf("led_open failed errno=%d\n", errno);

    acfd = open("/dev/acodec", O_RDWR);
    printf("acodec fd=%d\n", acfd);
    ac_u32(ACODEC_SET_PD_ADCL, 0);
    ac_u32(ACODEC_SET_PD_ADCR, 0);
    ac_u32(ACODEC_SET_MICL_MUTE, 0);
    ac_u32(ACODEC_SET_MICR_MUTE, 0);
    ac_u32(ACODEC_SET_ADC_HP_FILTER, 0);
    ac_u32(ACODEC_SET_MIXER_MIC, ACODEC_MIXER_LINEIN);
    ac_u32(ACODEC_SET_GAIN_MICL, ACODEC_GAIN_MIC);
    ac_u32(ACODEC_SET_GAIN_MICR, ACODEC_GAIN_MIC);
    ac_u32(ACODEC_ENABLE_BOOSTL, ACODEC_BOOST);
    ac_u32(ACODEC_ENABLE_BOOSTR, ACODEC_BOOST);
    ac_vol(ACODEC_SET_ADCL_VOL, ACODEC_ADC_VOL);
    ac_vol(ACODEC_SET_ADCR_VOL, ACODEC_ADC_VOL);
    printf("acodec: LINEIN PGA=%d boost=%d adcVol=%d\n", ACODEC_GAIN_MIC,
           ACODEC_BOOST, ACODEC_ADC_VOL);

    fp = fopen(path, "wb");
    snprintf(opath, sizeof(opath), "%s.orig.pcm", path);
    fpo = fopen(opath, "wb");
    printf("dump -> %s (%s)  orig -> %s (%s)\n", path, fp ? "ok" : "FAIL",
           opath, fpo ? "ok" : "FAIL");
    afe_reset(&afe);
    {
        struct timeval tv; gettimeofday(&tv, NULL);
        t_start = (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
    }
    printf("\n%-8s %-9s %-9s %-7s %-7s %-6s %-7s %s\n",
           "t(s)", "rawRMS", "filtRMS", "peak", "agc", "gate", "noise", "voice");
    fflush(stdout);

    for (;;) {
        fd_set rfds; struct timeval tv; long long now;

        led_tick_now();

        FD_ZERO(&rfds); FD_SET(fd, &rfds);
        tv.tv_sec = 0; tv.tv_usec = 150000;
        if (select(fd + 1, &rfds, NULL, NULL, &tv) <= 0) {
            struct timeval t2; gettimeofday(&t2, NULL);
            if ((long long)t2.tv_sec * 1000 + t2.tv_usec / 1000 - t_start >= (long long)seconds * 1000)
                break;
            continue;
        }
        memset(&f, 0, sizeof(f)); memset(&ref, 0, sizeof(ref));
        if (HI_MPI_AI_GetFrame(AI_DEV, AI_CHN, &f, &ref, HI_FALSE) != 0) continue;

        if (f.pVirAddr[0] && f.u32Len > 0) {
            short *pcm = (short *)f.pVirAddr[0];
            int cnt = (int)(f.u32Len / sizeof(short));
            int k;
            if (cnt > AFE_MAX_FRAME) cnt = AFE_MAX_FRAME;
            for (k = 0; k < cnt; k++) {   /* int32 累加，样本先 >>4 防溢出 */
                int t = pcm[k] >> 4;
                sq_raw += (unsigned int)(t * t);
            }
            n_raw += cnt;
            afe_process(&afe, pcm, outbuf, cnt);
            if (fp)  { fwrite(afe.tmp, sizeof(short), (size_t)cnt, fp);  fflush(fp); }
            if (fpo) { fwrite(pcm, sizeof(short), (size_t)cnt, fpo); fflush(fpo); }
            nfrm++;
        }
        HI_MPI_AI_ReleaseFrame(AI_DEV, AI_CHN, &f, &ref);

        {
            struct timeval t2;
            gettimeofday(&t2, NULL);
            now = (long long)t2.tv_sec * 1000 + t2.tv_usec / 1000 - t_start;
        }
        if (now - last_print >= 250) {
            last_print = now;
            if (n_raw > 0) {
                printf("t=%-6.2f %-9d %-9d %-7d %-7d %-6d %-7d %s\n",
                       now / 1000.0,
                       isqrt64((unsigned long long)(sq_raw / (unsigned int)n_raw)) << 4,
                       afe.last_raw_rms, afe.last_peak,
                       afe.agc_q10 >> 10, afe.gate_q10 >> 10,
                       afe.noise_rms,
                       afe.in_speech ? "SPEECH" : "-");
                fflush(stdout);
                sq_raw = 0; n_raw = 0;
            }
        }
        if (now >= (long long)seconds * 1000) break;
    }

    /* 自身 CPU 占用：jiffies 默认 100Hz */
    {
        long cpu1 = proc_cpu_jiffies();
        double wall = 0;
        struct timeval t2; gettimeofday(&t2, NULL);
        wall = ((long long)t2.tv_sec * 1000 + t2.tv_usec / 1000 - t_start) / 1000.0;
        if (wall > 0.5) {
            double cpu = (cpu1 - cpu0) / 100.0;
            printf("\n[stat] frames=%ld wall=%.1fs cpu=%.3fs  => %.1f%% of one core, %.3f ms/frame\n",
                   nfrm, wall, cpu, cpu / wall * 100.0,
                   (nfrm > 0) ? (cpu * 1000.0 / nfrm) : 0.0);
        }
    }

    if (fp) fclose(fp);
    if (fpo) fclose(fpo);
    led_alloff();
    led_close();
    if (acfd >= 0) close(acfd);
    printf("=== done ===\n");
    return 0;
}
