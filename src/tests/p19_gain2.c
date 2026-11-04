/*
 * p19_gain2.c -- 继续找还能用的增益级
 *
 * p17 已经证明 MICIN 下 MIC PGA 无效（0..31 档电平不变）。
 * 这里把 acodec 剩下所有可能影响输入电平的接口逐个拉到极值：
 *   INPUT_VOL(总输入音量) / ADCL_VOL 细扫 / ADCL2DACL 等内部增益 / 各 MUTE
 * 只要有一项能让 rms 或 hf1k 明显变化，就是漏掉的增益级。
 *
 * 用法: p19_gain2
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

#define AI_DEV 0
#define AI_CHN 0
#define CAP_FRAMES 25

static int isqrt64(unsigned long long v)
{
    unsigned long long rem = 0, root = 0; int i;
    for (i = 0; i < 32; i++) {
        root <<= 1; rem = (rem << 2) | (v >> 62); v <<= 2;
        if (root < rem) { rem -= root + 1; root += 2; }
    }
    return (int)(root >> 1);
}

static const int HP_A_Q15 = 24642;
static int hy[2], hx[2];
static void hp_reset(void) { hy[0]=hy[1]=hx[0]=hx[1]=0; }
static int hp2(int v)
{
    int i;
    for (i = 0; i < 2; i++) {
        int y = (int)(((long long)HP_A_Q15 * (hy[i] + v - hx[i])) >> 15);
        hx[i] = v; hy[i] = y; v = y;
    }
    return v;
}

static int acfd = -1, aifd = -1;
static void ac_u32(unsigned long q, unsigned int v) { ioctl(acfd, q, &v); }
static void ac_vol(unsigned long q, unsigned int v)
{
    ACODEC_VOL_CTRL c; c.vol_ctrl = v; c.vol_ctrl_mute = 0; ioctl(acfd, q, &c);
}

static void measure(const char *tag)
{
    AUDIO_FRAME_S f; AEC_FRAME_S r;
    long long sq = 0, sum = 0, sqh = 0;
    long n = 0, nh = 0;
    int pk = 0, got = 0, i, k, g = 0;

    usleep(150000);
    while (g++ < 80) {
        memset(&f, 0, sizeof(f)); memset(&r, 0, sizeof(r));
        if (HI_MPI_AI_GetFrame(AI_DEV, AI_CHN, &f, &r, HI_TRUE) != 0) break;
        HI_MPI_AI_ReleaseFrame(AI_DEV, AI_CHN, &f, &r);
    }
    hp_reset();

    for (i = 0; i < CAP_FRAMES * 2 && got < CAP_FRAMES; i++) {
        fd_set rfds; struct timeval tv;
        FD_ZERO(&rfds); FD_SET(aifd, &rfds);
        tv.tv_sec = 0; tv.tv_usec = 300000;
        if (select(aifd + 1, &rfds, NULL, NULL, &tv) <= 0) continue;
        memset(&f, 0, sizeof(f)); memset(&r, 0, sizeof(r));
        if (HI_MPI_AI_GetFrame(AI_DEV, AI_CHN, &f, &r, HI_FALSE) != 0) continue;
        got++;
        if (f.pVirAddr[0] && f.u32Len > 0) {
            short *p = (short *)f.pVirAddr[0];
            int cnt = (int)(f.u32Len / sizeof(short));
            for (k = 0; k < cnt; k++) {
                long long v = p[k]; int h = hp2(p[k]);
                sq += v * v; sum += v; n++;
                sqh += (long long)h * h; nh++;
                if (p[k] < 0) { if (-p[k] > pk) pk = -p[k]; }
                else if (p[k] > pk) pk = p[k];
            }
        }
        HI_MPI_AI_ReleaseFrame(AI_DEV, AI_CHN, &f, &r);
    }
    printf("%-30s mean=%7d rms=%6d hf1k=%5d peak=%6d fr=%d\n", tag,
           n ? (int)(sum / n) : 0,
           n ? isqrt64((unsigned long long)(sq / n)) : 0,
           nh ? isqrt64((unsigned long long)(sqh / nh)) : 0, pk, got);
    fflush(stdout);
}

static void base_cfg(int mixer)
{
    ac_u32(ACODEC_SET_PD_ADCL, 0);
    ac_u32(ACODEC_SET_PD_ADCR, 0);
    ac_u32(ACODEC_SET_MICL_MUTE, 0);
    ac_u32(ACODEC_SET_MICR_MUTE, 0);
    ac_u32(ACODEC_SET_ADC_HP_FILTER, 0);
    ac_u32(ACODEC_SET_MIXER_MIC, (unsigned)mixer);
    ac_u32(ACODEC_SET_GAIN_MICL, 31);
    ac_u32(ACODEC_SET_GAIN_MICR, 31);
    ac_u32(ACODEC_ENABLE_BOOSTL, 1);
    ac_u32(ACODEC_ENABLE_BOOSTR, 1);
    ac_vol(ACODEC_SET_ADCL_VOL, 1);
    ac_vol(ACODEC_SET_ADCR_VOL, 1);
}

int main(int argc, char **argv)
{
    AIO_ATTR_S attr; MPP_SYS_CONF_S sc;
    unsigned int iv = 0xDEADBEEF;
    int mixer = (argc > 1) ? atoi(argv[1]) : 1;
    static const unsigned int vols[] = { 0, 1, 2, 4, 8, 16, 32, 62 };
    char tag[64];
    int i;

    printf("=== p19 gain2 (mixer=%d) ===\n", mixer);
    memset(&sc, 0, sizeof(sc)); sc.u32AlignWidth = 64;
    HI_MPI_SYS_SetConf(&sc);
    if (HI_MPI_SYS_Init() != 0) return 1;
    memset(&attr, 0, sizeof(attr));
    HI_MPI_AI_GetPubAttr(AI_DEV, &attr);
    HI_MPI_AI_Enable(AI_DEV);
    HI_MPI_AI_EnableChn(AI_DEV, AI_CHN);
    aifd = HI_MPI_AI_GetFd(AI_DEV, AI_CHN);
    acfd = open("/dev/acodec", O_RDWR);
    printf("ai fd=%d acodec fd=%d\n\n", aifd, acfd);
    if (aifd < 0 || acfd < 0) return 2;

    base_cfg(mixer);
    measure("baseline (gain31 boost1 vol1)");

    printf("\n--- INPUT_VOL 总音量 ---\n");
    for (i = 0; i < 5; i++) {
        static const unsigned int t[] = { 0, 0x20, 0x3f, 0x60, 0x7e };
        ac_u32(ACODEC_SET_INPUT_VOL, t[i]);
        snprintf(tag, sizeof(tag), "INPUT_VOL=0x%02x", t[i]);
        measure(tag);
    }
    ioctl(acfd, ACODEC_GET_INPUT_VOL, &iv);
    printf("  (GET_INPUT_VOL now = %u)\n", iv);

    printf("\n--- ADCL_VOL 数字音量细扫 ---\n");
    for (i = 0; i < (int)(sizeof(vols) / sizeof(vols[0])); i++) {
        ac_vol(ACODEC_SET_ADCL_VOL, vols[i]);
        ac_vol(ACODEC_SET_ADCR_VOL, vols[i]);
        snprintf(tag, sizeof(tag), "ADCL_VOL=%u", vols[i]);
        measure(tag);
    }
    ac_vol(ACODEC_SET_ADCL_VOL, 1);
    ac_vol(ACODEC_SET_ADCR_VOL, 1);

    printf("\n--- 内部增益/跟踪选择 ---\n");
    ac_vol(ACODEC_SET_ADCL2DACL_VOL, 62); measure("ADCL2DACL_VOL=62");
    ac_vol(ACODEC_SET_ADCR2DACR_VOL, 62); measure("ADCR2DACR_VOL=62");
    ac_u32(ACODEC_ADCL_SEL_TRACK, 1);     measure("ADCL_SEL_TRACK=1");
    ac_u32(ACODEC_ADCR_SEL_TRACK, 1);     measure("ADCR_SEL_TRACK=1");
    ac_u32(ACODEC_SET_ADC_HP_FILTER, 1);  measure("ADC_HP_FILTER=1");
    ac_u32(ACODEC_SET_ADC_HP_FILTER, 0);

    printf("\n--- 改 I2S 数据位宽 / 采样率（错配会改变电平）---\n");
    ac_u32(ACODEC_SET_I2S1_DATAWIDTH, 16); measure("I2S1_DATAWIDTH=16");
    ac_u32(ACODEC_SET_I2S1_FS, 0x19);      measure("I2S1_FS=0x19(16k)");
    ac_u32(ACODEC_SET_I2S1_FS, 0x18);      measure("I2S1_FS=0x18(8k)");

    if (acfd >= 0) close(acfd);
    printf("=== done ===\n");
    return 0;
}
