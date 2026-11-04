/*
 * p21_noisecap.c -- 长时间录制原始 AI 帧（8 kHz / 单声道 / 16 bit）
 *
 * 用途：给唤醒词训练采集"真实房间底噪"负样本。录的是**原始帧**（不是 afe 输出），
 * 所以可以直接喂给部署链路的特征提取器，不需要担心二次滤波。
 *
 * 两个必须说清楚的细节（都是实测踩出来的）：
 *
 * 1) **不要调 HI_MPI_SYS_SetConf / SYS_Init**。
 *    实测：p15s/p21 加了这两句就 select 永远超时、0 帧；而 p8_client 完全
 *    不碰 SYS_Init，同样的 AI_Enable/EnableChn/GetFd 却能稳定取帧。
 *    原因未完全查清（怀疑是重复 SYS_Init 把本进程的 VB/池配置改掉了），
 *    但结论稳定：跟 p8_client 一致，不初始化。
 *
 * 2) **帧里已经是单声道 320 采样，不要再按 chnCnt 隔点抽取**。
 *    实测 p8_client 的 [frm] 日志：bw=1 sound=0 len=640。
 *    640 字节 = 320 个 short = 正好等于 u32PtNumPerFrm。
 *    虽然 AIO_ATTR 里 u32ChnCnt=2，但 enSoundmode=mono，取到的是单路数据；
 *    按"2 声道交织"去隔点等于把采样率降到 4 kHz，会把语音高频全毁掉。
 *
 * 用法：p21_noisecap <秒> <输出文件> [--sysinit]
 *   例：p21_noisecap 1800 /tmp/hd1/noise30m.raw
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/select.h>
#include <sys/time.h>

#include "hi_type.h"
#include "hi_common.h"
#include "hi_comm_aio.h"
#include "mpi_sys.h"
#include "mpi_ai.h"

#define AI_DEV      0
#define AI_CHN      0
#define SYS_ALIGN   64

static long long now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

int main(int argc, char **argv)
{
    AIO_ATTR_S attr;
    MPP_SYS_CONF_S sc;
    AUDIO_FRAME_S f;
    AEC_FRAME_S ref;
    FILE *fp;
    int fd, ok = 0, timeout = 0, fail = 0, shown = 0, drain = 0, got = 0;
    unsigned int seq_first = 0, seq_last = 0, seq_gap = 0, seq_prev = 0;
    int secs = (argc > 1) ? atoi(argv[1]) : 600;
    const char *out = (argc > 2) ? argv[2] : "/tmp/hd1/noise.raw";
    int do_sysinit = (argc > 3 && strcmp(argv[3], "--sysinit") == 0);
    int blk = (argc > 3 && strcmp(argv[3], "--block") == 0);   /* 绕过 select，阻塞取帧 */
    long long t0, next_report;
    long long total_samples = 0;

    printf("=== p21 noise capture: %d s -> %s (sysinit=%d block=%d) ===\n",
           secs, out, do_sysinit, blk);

    if (do_sysinit) {
        memset(&sc, 0, sizeof(sc));
        sc.u32AlignWidth = SYS_ALIGN;
        printf("SYS_SetConf ret=%d\n", (int)HI_MPI_SYS_SetConf(&sc));
        printf("SYS_Init    ret=%d\n", (int)HI_MPI_SYS_Init());
    }

    memset(&attr, 0, sizeof(attr));
    HI_MPI_AI_GetPubAttr(AI_DEV, &attr);
    printf("AI rate=%d bw=%d sound=%d ch=%u pt=%u frm=%u ex=%u clk=%u\n",
           (int)attr.enSamplerate, (int)attr.enBitwidth, (int)attr.enSoundmode,
           (unsigned)attr.u32ChnCnt, (unsigned)attr.u32PtNumPerFrm,
           (unsigned)attr.u32FrmNum, (unsigned)attr.u32EXFlag,
           (unsigned)attr.u32ClkSel);

    printf("AI_Enable    ret=%d\n", (int)HI_MPI_AI_Enable(AI_DEV));
    printf("AI_EnableChn ret=%d\n", (int)HI_MPI_AI_EnableChn(AI_DEV, AI_CHN));
    fd = HI_MPI_AI_GetFd(AI_DEV, AI_CHN);
    printf("AI fd=%d\n", fd);
    if (fd < 0) return 2;

    fp = fopen(out, "wb");
    if (!fp) { printf("open %s failed errno=%d\n", out, errno); return 3; }

    t0 = now_ms();
    (void)got;
    next_report = t0 + 10000;
    while (now_ms() - t0 < (long long)secs * 1000) {
        fd_set rfds;
        struct timeval tv;
        int r;

        if (!blk) {
            FD_ZERO(&rfds);
            FD_SET(fd, &rfds);
            tv.tv_sec = 1; tv.tv_usec = 0;
            r = select(fd + 1, &rfds, NULL, NULL, &tv);
            if (r == 0) {
                timeout++;
                if (shown < 3) { printf("select timeout (累计 %d)\n", timeout); shown++; }
                continue;
            }
            if (r < 0) { if (errno == EINTR) continue; printf("select errno=%d\n", errno); break; }
        }

        /* 一次 select 唤醒后把队列排空：驱动可能已经把多帧排好但只唤醒一次。
           实测：只取一帧就回去 select，会漏掉约 12% 的帧（序列号不连续）。 */
        for (drain = 0; drain < 32; drain++) {
            memset(&f, 0, sizeof(f));
            memset(&ref, 0, sizeof(ref));
            r = HI_MPI_AI_GetFrame(AI_DEV, AI_CHN, &f, &ref, blk ? 1000 : HI_FALSE);
            if (r != 0) {
                if (drain == 0) {
                    fail++;
                    if (fail <= 3) printf("GetFrame ret=0x%08X\n", (unsigned)r);
                }
                break;   /* 队列空了 */
            }
            got = 1;
            ok++;
            /* 序列号连续性：判断"帧率不足"到底是设备只出这么多帧，还是我们漏读 */
            if (ok == 1) { seq_first = f.u32Seq; }
            else if (f.u32Seq != seq_prev + 1) { seq_gap += (f.u32Seq - seq_prev - 1); }
            seq_prev = f.u32Seq;
            seq_last = f.u32Seq;

            if (ok <= 2)
                printf("frame#%d len=%u seq=%u ts=%llu p0=%p\n", ok,
                       (unsigned)f.u32Len, (unsigned)f.u32Seq,
                       (unsigned long long)f.u64TimeStamp, f.pVirAddr[0]);

            if (f.pVirAddr[0] && f.u32Len > 0) {
                /* 整帧原样落盘：len=640 就是 320 个 mono short（见文件头说明） */
                fwrite(f.pVirAddr[0], 1, f.u32Len, fp);
                total_samples += (long long)(f.u32Len / sizeof(short));
            }
            HI_MPI_AI_ReleaseFrame(AI_DEV, AI_CHN, &f, &ref);
        }   /* end drain loop */

        if (!got) continue;

        if (now_ms() >= next_report) {
            long long el = (now_ms() - t0) / 1000;
            long long sec_of_audio = total_samples / 8000;
            printf("[%lld/%d s] frames=%d 音频=%lld s  速率=%.2fx  timeout=%d\n",
                   el, secs, ok, sec_of_audio,
                   (el > 0) ? (double)sec_of_audio / (double)el : 0.0, timeout);
            fflush(stdout);
            next_report += 10000;
        }
    }
    fclose(fp);
    printf("done: frames=%d timeout=%d fail=%d 音频=%.1f s 期待=%d s\n",
           ok, timeout, fail, (double)total_samples / 8000.0, secs);
    printf("seq: %u..%u 跨度=%u 漏读=%u (%.1f%%)\n",
           seq_first, seq_last, seq_last - seq_first + 1, seq_gap,
           (seq_last - seq_first + 1) > 0
               ? 100.0 * seq_gap / (double)(seq_last - seq_first + 1) : 0.0);
    return 0;
}
