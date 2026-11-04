/*
 * p17_gainprobe.c -- 判定 acodec 模拟前端是否真的在起作用
 *
 * 判据（不需要任何人配合）：
 *   如果麦克风信号真的经过 MIC PGA + BOOST 再进 ADC，
 *   那么改变 PGA 增益时，采到的 **直流均值** 与 **RMS** 必须同步变化。
 *   若两者纹丝不动 —— 说明 ADC 的输出来自内部固定电平/噪声，
 *   模拟通路（麦克风 -> PGA -> ADC）实际未接通。
 *
 * 用法: p17_gainprobe
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
#define CAP_FRAMES  25          /* 25 x 40ms = 1 秒 */

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

static int acfd = -1;
static void ac_u32(unsigned long q, unsigned int v) { if (acfd >= 0) ioctl(acfd, q, &v); }
static void ac_vol(unsigned long q, unsigned int v)
{
    ACODEC_VOL_CTRL c; c.vol_ctrl = v; c.vol_ctrl_mute = 0;
    if (acfd >= 0) ioctl(acfd, q, &c);
}

static void drain(int fd)
{
    AUDIO_FRAME_S f; AEC_FRAME_S r; int g = 0;
    while (g++ < 80) {
        memset(&f, 0, sizeof(f)); memset(&r, 0, sizeof(r));
        if (HI_MPI_AI_GetFrame(AI_DEV, AI_CHN, &f, &r, HI_TRUE) != 0) break;
        HI_MPI_AI_ReleaseFrame(AI_DEV, AI_CHN, &f, &r);
    }
    (void)fd;
}

static void measure(int fd, const char *tag, int *out_mean, int *out_rms, int *out_pk)
{
    AUDIO_FRAME_S f; AEC_FRAME_S r;
    long long sq = 0, sum = 0;
    long n = 0;
    int pk = 0, got = 0, i, k;

    usleep(200000);
    drain(fd);

    for (i = 0; i < CAP_FRAMES * 2 && got < CAP_FRAMES; i++) {
        fd_set rfds; struct timeval tv;
        FD_ZERO(&rfds); FD_SET(fd, &rfds);
        tv.tv_sec = 0; tv.tv_usec = 300000;
        if (select(fd + 1, &rfds, NULL, NULL, &tv) <= 0) continue;
        memset(&f, 0, sizeof(f)); memset(&r, 0, sizeof(r));
        if (HI_MPI_AI_GetFrame(AI_DEV, AI_CHN, &f, &r, HI_FALSE) != 0) continue;
        got++;
        if (f.pVirAddr[0] && f.u32Len > 0) {
            short *p = (short *)f.pVirAddr[0];
            int cnt = (int)(f.u32Len / sizeof(short));
            for (k = 0; k < cnt; k++) {
                long long v = p[k];
                sq += v * v; sum += v; n++;
                if (p[k] < 0) { if (-p[k] > pk) pk = -p[k]; }
                else if (p[k] > pk) pk = p[k];
            }
        }
        HI_MPI_AI_ReleaseFrame(AI_DEV, AI_CHN, &f, &r);
    }
    *out_mean = n ? (int)(sum / n) : 0;
    *out_rms  = n ? isqrt64((unsigned long long)(sq / n)) : 0;
    *out_pk   = pk;
    printf("%-28s mean=%6d  rms=%5d  peak=%5d  frames=%d\n", tag, *out_mean, *out_rms, *out_pk, got);
    fflush(stdout);
}

int main(void)
{
    AIO_ATTR_S attr;
    MPP_SYS_CONF_S sc;
    int fd, ret;

    printf("=== p17 gain probe ===\n");
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
    printf("AI fd=%d\n", fd);
    if (fd < 0) return 2;

    acfd = open("/dev/acodec", O_RDWR);
    printf("acodec fd=%d\n\n", acfd);

    /* ADC 上电、解静音、关 HPF，固定 MICIN，只动 PGA / BOOST */
    ac_u32(ACODEC_SET_PD_ADCL, 0);
    ac_u32(ACODEC_SET_PD_ADCR, 0);
    ac_u32(ACODEC_SET_MICL_MUTE, 0);
    ac_u32(ACODEC_SET_MICR_MUTE, 0);
    ac_u32(ACODEC_SET_ADC_HP_FILTER, 0);
    ac_u32(ACODEC_SET_MIXER_MIC, 1);      /* MICIN */
    ac_u32(ACODEC_ENABLE_BOOSTL, 0);
    ac_u32(ACODEC_ENABLE_BOOSTR, 0);
    ac_vol(ACODEC_SET_ADCL_VOL, 1);
    ac_vol(ACODEC_SET_ADCR_VOL, 1);

    printf("--- MICIN, BOOST=0, 只改 PGA ---\n");
    {
        static const int gains[] = { 0, 8, 16, 24, 31 };
        char tag[64];
        int i, m, r, p;
        for (i = 0; i < 5; i++) {
            ac_u32(ACODEC_SET_GAIN_MICL, (unsigned)gains[i]);
            ac_u32(ACODEC_SET_GAIN_MICR, (unsigned)gains[i]);
            snprintf(tag, sizeof(tag), "PGA=%2d", gains[i]);
            measure(fd, tag, &m, &r, &p);
        }
    }

    printf("--- MICIN, PGA=31, 只改 BOOST ---\n");
    ac_u32(ACODEC_SET_GAIN_MICL, 31);
    ac_u32(ACODEC_SET_GAIN_MICR, 31);
    {
        int m, r, p;
        ac_u32(ACODEC_ENABLE_BOOSTL, 0);
        ac_u32(ACODEC_ENABLE_BOOSTR, 0);
        measure(fd, "BOOST=0", &m, &r, &p);
        ac_u32(ACODEC_ENABLE_BOOSTL, 1);
        ac_u32(ACODEC_ENABLE_BOOSTR, 1);
        measure(fd, "BOOST=1", &m, &r, &p);
    }

    printf("--- 对照：切回 LINEIN ---\n");
    {
        int m, r, p;
        ac_u32(ACODEC_SET_MIXER_MIC, 0);
        measure(fd, "LINEIN PGA=31 BOOST=1", &m, &r, &p);
        ac_u32(ACODEC_SET_MIXER_MIC, 1);
        measure(fd, "MICIN  PGA=31 BOOST=1", &m, &r, &p);
    }

    printf("--- 对照：ADC 掉电 ---\n");
    {
        int m, r, p;
        ac_u32(ACODEC_SET_PD_ADCL, 1);
        ac_u32(ACODEC_SET_PD_ADCR, 1);
        measure(fd, "MICIN ADC powered DOWN", &m, &r, &p);
        ac_u32(ACODEC_SET_PD_ADCL, 0);
        ac_u32(ACODEC_SET_PD_ADCR, 0);
        measure(fd, "MICIN ADC powered UP", &m, &r, &p);
    }

    if (acfd >= 0) close(acfd);
    printf("=== done ===\n");
    return 0;
}
