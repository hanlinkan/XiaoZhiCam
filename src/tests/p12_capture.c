/*
 * p12_capture.c -- 最小化 AI 采集落盘，用于判断麦克风原始信号。
 *
 * 只做：SYS_Init -> AI_Enable -> AI_EnableChn(0) -> GetFrame -> 写 raw
 * 不调用 AI_SetPubAttr，不碰 AO/acodec，不 Disable（避免影响 rmm）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/select.h>

#include "hi_type.h"
#include "hi_common.h"
#include "hi_comm_aio.h"
#include "mpi_sys.h"
#include "mpi_ai.h"

#define AI_DEV      0
#define AI_CHN      0
#define SYS_ALIGN   64
#define FRAMES      100
#define OUT         "/tmp/c/p12.raw"

int main(void)
{
    AIO_ATTR_S attr;
    MPP_SYS_CONF_S sc;
    AUDIO_FRAME_S f;
    AEC_FRAME_S ref;
    FILE *fp;
    int fd, i, ok = 0, timeout = 0, fail = 0;
    long long sum_sq = 0;
    long samples = 0;
    short vmin = 32767, vmax = -32768;

    printf("=== p12 capture ===\n");
    memset(&sc, 0, sizeof(sc));
    sc.u32AlignWidth = SYS_ALIGN;
    printf("SYS_SetConf ret=%d\n", (int)HI_MPI_SYS_SetConf(&sc));
    printf("SYS_Init ret=%d\n", (int)HI_MPI_SYS_Init());

    memset(&attr, 0, sizeof(attr));
    printf("AI_GetPubAttr ret=%d\n", (int)HI_MPI_AI_GetPubAttr(AI_DEV, &attr));
    printf("AI rate=%d bw=%d work=%d sound=%d pt=%u frm=%u ch=%u ex=%u clk=%u\n",
           (int)attr.enSamplerate, (int)attr.enBitwidth, (int)attr.enWorkmode,
           (int)attr.enSoundmode, (unsigned)attr.u32PtNumPerFrm,
           (unsigned)attr.u32FrmNum, (unsigned)attr.u32ChnCnt,
           (unsigned)attr.u32EXFlag, (unsigned)attr.u32ClkSel);

    printf("AI_Enable ret=%d\n", (int)HI_MPI_AI_Enable(AI_DEV));
    printf("AI_EnableChn ret=%d\n", (int)HI_MPI_AI_EnableChn(AI_DEV, AI_CHN));
    fd = HI_MPI_AI_GetFd(AI_DEV, AI_CHN);
    printf("AI_GetFd fd=%d\n", fd);
    if (fd < 0) return 2;

    fp = fopen(OUT, "wb");
    if (!fp) { printf("open %s failed errno=%d\n", OUT, errno); return 3; }

    for (i = 0; i < FRAMES; i++) {
        fd_set rfds;
        struct timeval tv;
        int r;

        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        tv.tv_sec = 1; tv.tv_usec = 0;
        r = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (r == 0) { timeout++; continue; }
        if (r < 0) { printf("select errno=%d\n", errno); break; }

        memset(&f, 0, sizeof(f));
        memset(&ref, 0, sizeof(ref));
        if (HI_MPI_AI_GetFrame(AI_DEV, AI_CHN, &f, &ref, HI_FALSE) != 0) {
            fail++;
            continue;
        }
        ok++;
        if (f.pVirAddr[0] && f.u32Len > 0) {
            short *p = (short *)f.pVirAddr[0];
            int n = (int)(f.u32Len / sizeof(short));
            int k;
            fwrite(f.pVirAddr[0], 1, f.u32Len, fp);
            if (i < 3) {
                printf("frm%d bw=%d sound=%d len=%u seq=%u\n",
                       i, (int)f.enBitwidth, (int)f.enSoundmode,
                       (unsigned)f.u32Len, (unsigned)f.u32Seq);
            }
            for (k = 0; k < n; k++) {
                long long v = p[k];
                sum_sq += v * v;
                if (p[k] < vmin) vmin = p[k];
                if (p[k] > vmax) vmax = p[k];
            }
            samples += n;
        }
        HI_MPI_AI_ReleaseFrame(AI_DEV, AI_CHN, &f, &ref);
    }
    fclose(fp);

    printf("frames ok=%d timeout=%d fail=%d samples=%ld\n", ok, timeout, fail, samples);
    if (samples > 0) {
        double ms = (double)sum_sq / (double)samples;
        double y = ms > 0 ? ms / 2.0 : 0;
        for (i = 0; i < 25; i++) y = (y + ms / y) / 2.0;
        printf("rms=%.0f (%.1f%%) min=%d max=%d\n",
               y, y * 100.0 / 32767.0, vmin, vmax);
    }
    printf("raw written to %s\n", OUT);
    return 0;
}
