/*
 * p18_micpower.c -- 逐个试探 CPLD 里未识别的 GPIO 开关，找麦克风偏置供电
 *
 * 假设（来自 p17 的实测）：
 *   - LINEIN 通路的 PGA 有效，但输入是悬空工频 -> 麦克风可能接在 LINEIN，
 *     只是没有偏置，驻极体没供电就等于断路，表现为"悬空拾取工频"
 *   - MICIN 通路的 PGA 完全无效   -> 那个引脚确实什么都没接
 *
 * 判据：
 *   真正的麦克风一旦通电，会同时出现 ①宽带噪声底上升 ②50Hz 工频相对占比下降。
 *   所以除了 RMS，还要看 1kHz 以上的宽带能量（真实麦克风的本底噪声）。
 *
 * 用法: p18_micpower [mixer]     mixer 默认 0 = LINEIN
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/ioctl.h>

#include "hi_type.h"
#include "hi_common.h"
#include "hi_comm_aio.h"
#include "mpi_sys.h"
#include "mpi_ai.h"
#include "acodec.h"

#define AI_DEV      0
#define AI_CHN      0
#define SYS_ALIGN   64
#define CAP_FRAMES  25          /* 1 秒 */

static int isqrt64(unsigned long long v)
{
    unsigned long long rem = 0, root = 0;
    int i;
    for (i = 0; i < 32; i++) {
        root <<= 1;
        rem = (rem << 2) | (v >> 62);
        v <<= 2;
        if (root < rem) { rem -= root + 1; root += 2; }
    }
    return (int)(root >> 1);
}

/* 2 级一阶高通 @1kHz，用来拿"1kHz 以上宽带能量" */
static const int HP_A_Q15 = 24642;   /* 1/(1+2*pi*1000/8000) */
static int hp_y[2], hp_x[2];
static void hp_reset(void) { hp_y[0]=hp_y[1]=hp_x[0]=hp_x[1]=0; }
static int hp2(int v)
{
    int i;
    for (i = 0; i < 2; i++) {
        int y = (int)(((long long)HP_A_Q15 * (hp_y[i] + v - hp_x[i])) >> 15);
        hp_x[i] = v; hp_y[i] = y; v = y;
    }
    return v;
}

static int acfd = -1;
static int cpldfd = -1;
static void ac_u32(unsigned long q, unsigned int v) { if (acfd >= 0) ioctl(acfd, q, &v); }
static void ac_vol(unsigned long q, unsigned int v)
{
    ACODEC_VOL_CTRL c; c.vol_ctrl = v; c.vol_ctrl_mute = 0;
    if (acfd >= 0) ioctl(acfd, q, &c);
}

static int g_fd = -1;

static void drain(void)
{
    AUDIO_FRAME_S f; AEC_FRAME_S r; int g = 0;
    while (g++ < 80) {
        memset(&f, 0, sizeof(f)); memset(&r, 0, sizeof(r));
        if (HI_MPI_AI_GetFrame(AI_DEV, AI_CHN, &f, &r, HI_TRUE) != 0) break;
        HI_MPI_AI_ReleaseFrame(AI_DEV, AI_CHN, &f, &r);
    }
}

static void measure(const char *tag)
{
    AUDIO_FRAME_S f; AEC_FRAME_S r;
    long long sq = 0, sum = 0, sqh = 0;
    long n = 0, nh = 0;
    int pk = 0, got = 0, i, k;

    usleep(150000);
    drain();
    hp_reset();

    for (i = 0; i < CAP_FRAMES * 2 && got < CAP_FRAMES; i++) {
        fd_set rfds; struct timeval tv;
        FD_ZERO(&rfds); FD_SET(g_fd, &rfds);
        tv.tv_sec = 0; tv.tv_usec = 300000;
        if (select(g_fd + 1, &rfds, NULL, NULL, &tv) <= 0) continue;
        memset(&f, 0, sizeof(f)); memset(&r, 0, sizeof(r));
        if (HI_MPI_AI_GetFrame(AI_DEV, AI_CHN, &f, &r, HI_FALSE) != 0) continue;
        got++;
        if (f.pVirAddr[0] && f.u32Len > 0) {
            short *p = (short *)f.pVirAddr[0];
            int cnt = (int)(f.u32Len / sizeof(short));
            for (k = 0; k < cnt; k++) {
                long long v = p[k];
                int h = hp2(p[k]);
                sq += v * v; sum += v; n++;
                sqh += (long long)h * h; nh++;
                if (p[k] < 0) { if (-p[k] > pk) pk = -p[k]; }
                else if (p[k] > pk) pk = p[k];
            }
        }
        HI_MPI_AI_ReleaseFrame(AI_DEV, AI_CHN, &f, &r);
    }
    {
        int mean = n ? (int)(sum / n) : 0;
        int rms  = n ? isqrt64((unsigned long long)(sq / n)) : 0;
        int hf   = nh ? isqrt64((unsigned long long)(sqh / nh)) : 0;
        printf("%-26s mean=%7d rms=%6d hf1k=%5d peak=%6d fr=%d\n",
               tag, mean, rms, hf, pk, got);
        fflush(stdout);
    }
}

int main(int argc, char **argv)
{
    AIO_ATTR_S attr;
    MPP_SYS_CONF_S sc;
    int mixer = (argc > 1) ? atoi(argv[1]) : 0;
    int i, ret;

    printf("=== p18 mic power probe (mixer=%d) ===\n", mixer);
    memset(&sc, 0, sizeof(sc));
    sc.u32AlignWidth = SYS_ALIGN;
    HI_MPI_SYS_SetConf(&sc);
    ret = HI_MPI_SYS_Init();
    printf("SYS_Init ret=%d\n", (int)ret);
    if (ret) return 1;

    memset(&attr, 0, sizeof(attr));
    HI_MPI_AI_GetPubAttr(AI_DEV, &attr);
    HI_MPI_AI_Enable(AI_DEV);
    printf("AI_EnableChn ret=%d\n", (int)HI_MPI_AI_EnableChn(AI_DEV, AI_CHN));
    g_fd = HI_MPI_AI_GetFd(AI_DEV, AI_CHN);
    printf("AI fd=%d\n", g_fd);
    if (g_fd < 0) return 2;

    acfd = open("/dev/acodec", O_RDWR);
    cpldfd = open("/dev/cpld_periph", O_RDWR);
    printf("acodec fd=%d, cpld fd=%d\n\n", acfd, cpldfd);

    ac_u32(ACODEC_SET_PD_ADCL, 0);
    ac_u32(ACODEC_SET_PD_ADCR, 0);
    ac_u32(ACODEC_SET_MICL_MUTE, 0);
    ac_u32(ACODEC_SET_MICR_MUTE, 0);
    ac_u32(ACODEC_SET_ADC_HP_FILTER, 0);
    ac_u32(ACODEC_SET_MIXER_MIC, (unsigned)mixer);
    ac_u32(ACODEC_SET_GAIN_MICL, 10);      /* 故意不用 31，避免工频削顶掩盖变化 */
    ac_u32(ACODEC_SET_GAIN_MICR, 10);
    ac_u32(ACODEC_ENABLE_BOOSTL, 0);
    ac_u32(ACODEC_ENABLE_BOOSTR, 0);
    ac_vol(ACODEC_SET_ADCL_VOL, 1);
    ac_vol(ACODEC_SET_ADCR_VOL, 1);

    printf("--- 基线（所有 GPIO 未动）---\n");
    measure("baseline");

    {
        static const unsigned long on_cmd[]  = { 0x7020, 0x7028, 0x702e, 0x7030 };
        static const unsigned long off_cmd[] = { 0x7021, 0x7029, 0x702f, 0x702f };
        char tag[64];

        printf("\n--- 逐个打开 CPLD GPIO，看是否给麦克风供电 ---\n");
        for (i = 0; i < 4; i++) {
            snprintf(tag, sizeof(tag), "ioctl 0x%04lX ON", on_cmd[i]);
            ioctl(cpldfd, on_cmd[i], 0);
            measure(tag);
            snprintf(tag, sizeof(tag), "  -> 0x%04lX OFF", off_cmd[i]);
            ioctl(cpldfd, off_cmd[i], 0);
            measure(tag);
        }
    }

    printf("\n--- 全部同时打开 ---\n");
    ioctl(cpldfd, 0x7020, 0);
    ioctl(cpldfd, 0x7028, 0);
    ioctl(cpldfd, 0x702e, 0);
    ioctl(cpldfd, 0x7030, 0);
    measure("all ON");

    printf("\n--- 再叠加 BOOST + PGA31（最高灵敏度）---\n");
    ac_u32(ACODEC_ENABLE_BOOSTL, 1);
    ac_u32(ACODEC_ENABLE_BOOSTR, 1);
    ac_u32(ACODEC_SET_GAIN_MICL, 31);
    ac_u32(ACODEC_SET_GAIN_MICR, 31);
    measure("all ON + maxgain");

    printf("\n--- 收尾：全部关闭 ---\n");
    ioctl(cpldfd, 0x7021, 0);
    ioctl(cpldfd, 0x7029, 0);
    ioctl(cpldfd, 0x702f, 0);
    measure("all OFF");

    if (cpldfd >= 0) close(cpldfd);
    if (acfd >= 0) close(acfd);
    printf("=== done ===\n");
    return 0;
}
