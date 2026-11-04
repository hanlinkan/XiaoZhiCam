/*
 * p20_regscan.c -- 直接操作 acodec 硬件寄存器，逐位翻转找出麦克风偏置/使能位
 *
 * 背景：
 *   p17 已经证明 MICIN 下 PGA 从 0 到 31 电平纹丝不动，而 LINEIN 下 PGA 线性放大。
 *   /tmp/hd1/t/acreg 暴露了 acodec 寄存器物理基址 = 0x20050068（8 个 32 位寄存器）。
 *   驱动 soft_reset 的默认值是 00fe2c2c / ff035a4a / 38780001 / 4f4f2424 / 1e1ec001 ...
 *   而当前实测值是 00324040 / ff031a4a / 38780001 / 06802424 / 01010001 ...
 *   说明 +00 和 +04 这两个寄存器被改过多次，麦克风偏置很可能就在里面。
 *
 * 方法：
 *   先读回验证能否写；然后对 +00..+1c 每个寄存器的 32 个 bit 逐个"异或翻转 ->
 *   采 0.5 秒 -> 恢复"，看哪一位会让采集电平/高频底噪发生跳变。
 *   真正的麦克风偏置一旦打开，输入阻抗和本底噪声都会明显变化。
 *
 * 用法: p20_regscan [reg_offset_hex ...]   默认扫 +00 +04
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
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
#define ACODEC_PAGE 0x20050000UL   /* mmap 必须页对齐 */
#define ACODEC_OFF  0x68           /* 寄存器区在页内偏移 */
#define ACODEC_LEN  0x24
#define ACODEC_SLOT (ACODEC_OFF / 4)
#define CAP_FRAMES  10          /* 10 x 40ms = 0.4 秒 */

static int isqrt64(unsigned long long v)
{
    unsigned long long rem = 0, root = 0; int i;
    for (i = 0; i < 32; i++) {
        root <<= 1; rem = (rem << 2) | (v >> 62); v <<= 2;
        if (root < rem) { rem -= root + 1; root += 2; }
    }
    return (int)(root >> 1);
}

/* 2 级一阶高通 @1kHz，量"语音频段宽带底噪" */
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

static volatile unsigned int *g_reg;
static int g_aifd = -1;

static unsigned int reg_rd(int idx) { return g_reg[ACODEC_SLOT + idx]; }
static void reg_wr(int idx, unsigned int v) { g_reg[ACODEC_SLOT + idx] = v; }

static void measure(int *rms_out, int *hf_out, int *mean_out)
{
    AUDIO_FRAME_S f; AEC_FRAME_S r;
    long long sq = 0, sum = 0, sqh = 0;
    long n = 0;
    int got = 0, i, k, g = 0;

    usleep(80000);
    while (g++ < 60) {
        memset(&f, 0, sizeof(f)); memset(&r, 0, sizeof(r));
        if (HI_MPI_AI_GetFrame(AI_DEV, AI_CHN, &f, &r, HI_TRUE) != 0) break;
        HI_MPI_AI_ReleaseFrame(AI_DEV, AI_CHN, &f, &r);
    }
    hp_reset();
    for (i = 0; i < CAP_FRAMES * 3 && got < CAP_FRAMES; i++) {
        fd_set rfds; struct timeval tv;
        FD_ZERO(&rfds); FD_SET(g_aifd, &rfds);
        tv.tv_sec = 0; tv.tv_usec = 250000;
        if (select(g_aifd + 1, &rfds, NULL, NULL, &tv) <= 0) continue;
        memset(&f, 0, sizeof(f)); memset(&r, 0, sizeof(r));
        if (HI_MPI_AI_GetFrame(AI_DEV, AI_CHN, &f, &r, HI_FALSE) != 0) continue;
        got++;
        if (f.pVirAddr[0] && f.u32Len > 0) {
            short *p = (short *)f.pVirAddr[0];
            int cnt = (int)(f.u32Len / sizeof(short));
            for (k = 0; k < cnt; k++) {
                long long v = p[k]; int h = hp2(p[k]);
                sq += v * v; sum += v; n++;
                sqh += (long long)h * h;
            }
        }
        HI_MPI_AI_ReleaseFrame(AI_DEV, AI_CHN, &f, &r);
    }
    *mean_out = n ? (int)(sum / n) : 0;
    *rms_out  = n ? isqrt64((unsigned long long)(sq / n)) : 0;
    *hf_out   = n ? isqrt64((unsigned long long)(sqh / n)) : 0;
}

int main(int argc, char **argv)
{
    MPP_SYS_CONF_S sc;
    AIO_ATTR_S attr;
    int memfd;
    void *map;
    int regs[8], nreg = 0, i, bit;
    int base_rms, base_hf, base_mean;
    int rms, hf, mean;

    printf("=== p20 acodec reg scan ===\n");
    if (argc > 1) {
        for (i = 1; i < argc && nreg < 8; i++) {
            int off = (int)strtol(argv[i], NULL, 0);
            if ((off & 3) == 0 && off >= 0 && off < ACODEC_LEN) regs[nreg++] = off / 4;
        }
    } else {
        regs[nreg++] = 0;   /* +00 */
        regs[nreg++] = 1;   /* +04 */
    }

    memfd = open("/dev/mem", O_RDWR);
    if (memfd < 0) { printf("open /dev/mem failed errno=%d\n", errno); return 1; }
    map = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, ACODEC_PAGE);
    if (map == MAP_FAILED) { printf("mmap failed errno=%d\n", errno); return 1; }
    g_reg = (volatile unsigned int *)map;
    printf("mapped acodec @0x%08lX (page 0x%08lX + 0x%x)\n", ACODEC_PAGE + ACODEC_OFF, ACODEC_PAGE, ACODEC_OFF);
    for (i = 0; i < 7; i++)
        printf("  +%02x = %08x\n", i * 4, reg_rd(i));
    printf("\n");

    /* 写回验证：置 bit0 再读回，然后恢复 */
    {
        unsigned int old = reg_rd(0);
        reg_wr(0, old | 1u);
        if (reg_rd(0) != (old | 1u)) {
            printf("!! 寄存器写入无效（只读），无法继续\n");
            return 2;
        }
        reg_wr(0, old);
        printf("写入权限 OK\n\n");
    }

    memset(&sc, 0, sizeof(sc)); sc.u32AlignWidth = 64;
    HI_MPI_SYS_SetConf(&sc);
    if (HI_MPI_SYS_Init() != 0) { printf("SYS_Init failed\n"); return 3; }
    memset(&attr, 0, sizeof(attr));
    HI_MPI_AI_GetPubAttr(AI_DEV, &attr);
    HI_MPI_AI_Enable(AI_DEV);
    if (HI_MPI_AI_EnableChn(AI_DEV, AI_CHN) != 0) { printf("AI_EnableChn failed\n"); return 4; }
    g_aifd = HI_MPI_AI_GetFd(AI_DEV, AI_CHN);
    if (g_aifd < 0) { printf("AI_GetFd failed\n"); return 4; }
    printf("AI fd=%d rate=%d ch=%u pt=%u\n\n", g_aifd, (int)attr.enSamplerate,
           (unsigned)attr.u32ChnCnt, (unsigned)attr.u32PtNumPerFrm);

    measure(&base_rms, &base_hf, &base_mean);
    printf("baseline: mean=%d rms=%d hf1k=%d\n\n", base_mean, base_rms, base_hf);
    printf("%-10s %-5s %-10s %-7s %-7s %-7s %s\n",
           "reg", "bit", "newval", "mean", "rms", "hf1k", "delta_rms");
    printf("---------------------------------------------------------------\n");

    for (i = 0; i < nreg; i++) {
        int idx = regs[i];
        unsigned int orig = reg_rd(idx);
        for (bit = 0; bit < 32; bit++) {
            unsigned int v = orig ^ (1u << bit);
            reg_wr(idx, v);
            measure(&rms, &hf, &mean);
            reg_wr(idx, orig);
            printf("+%02x        %-5d 0x%08x %-7d %-7d %-7d %+d%s\n",
                   idx * 4, bit, v, mean, rms, hf, rms - base_rms,
                   (abs(rms - base_rms) > base_rms / 2 + 60) ? "  <<< 变化明显" : "");
            fflush(stdout);
        }
        printf("+%02x 恢复为 0x%08x\n\n", idx * 4, reg_rd(idx));
    }

    printf("=== done ===\n");
    munmap(map, 0x1000);
    close(memfd);
    return 0;
}
