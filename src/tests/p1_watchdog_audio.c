/*
 * p1_watchdog_audio.c — 看门狗接管 + 独占音频配置验证
 *
 * 背景
 *   rmm 持有 /dev/watchdog（/proc/<pid>/fd/39），杀掉 rmm 后没人喂狗，
 *   硬件看门狗会在约 10 秒内复位整机。因此"停 rmm 独占音频"必须先解决喂狗。
 *
 * 本程序的设计
 *   1. 启动即拉起看门狗线程，循环尝试 open("/dev/watchdog")。
 *      rmm 活着时 open 失败（独占），持续重试；rmm 一死，立刻接管并开始喂狗。
 *   2. 主线程等待"看门狗已接管"这个事件，再初始化 MPP。
 *      这个顺序保证配置音频时 rmm 已经彻底退出，参数不会被锁。
 *   3. 配置 16 kHz 单声道，采 3 秒数据，统计电平与削波，验证通路。
 *   4. 全程不阻塞喂狗线程；喂狗周期 1 s，远短于驱动超时。
 *
 * 用法（外部脚本配合）
 *   ./p1_watchdog_audio &
 *   sleep 2 && killall rmm        # 程序会在 rmm 退出后自动接管
 *   等待程序打印结果
 *
 * 注意
 *   程序退出后若不再有人喂狗，设备会在看门狗超时后复位。
 *   这是预期行为：测试结束 → 自动恢复原厂固件。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>

#include "hi_type.h"
#include "hi_common.h"
#include "hi_comm_aio.h"
#include "mpi_sys.h"
#include "mpi_ai.h"
#include "mpi_ao.h"

#define AI_DEV        0
#define AI_CHN        0
#define AO_DEV        0
#define AO_CHN        0

#define TARGET_RATE   AUDIO_SAMPLE_RATE_16000
#define PT_PER_FRM    320            /* 20 ms @ 16 kHz */
#define FRM_NUM       8
#define CAP_SECONDS   3
#define FRMS_TO_CAP   (CAP_SECONDS * 1000 / 20)

static volatile int g_wd_fd    = -1;
static volatile int g_wd_ready = 0;
static volatile int g_stop     = 0;

/* ---------------- 看门狗 ---------------- */

static void *watchdog_task(void *arg)
{
    int fd;

    (void)arg;
    while (!g_stop) {
        fd = open("/dev/watchdog", O_WRONLY);
        if (fd < 0) {
            usleep(200 * 1000);          /* rmm 还占着，200ms 后再试 */
            continue;
        }

        g_wd_fd    = fd;
        g_wd_ready = 1;
        printf("[wd] 已接管 /dev/watchdog，每 1s 喂狗\n");
        fflush(stdout);

        while (!g_stop) {
            if (write(fd, "*", 1) != 1) {
                printf("[wd] 喂狗失败: %s\n", strerror(errno));
                fflush(stdout);
                break;
            }
            sleep(1);
        }

        g_wd_ready = 0;
        g_wd_fd    = -1;
        close(fd);
        if (!g_stop) {
            sleep(1);
            printf("[wd] 重新尝试接管\n");
            fflush(stdout);
        }
    }
    return NULL;
}

/* ---------------- 音频 ---------------- */

static void fill_attr(AIO_ATTR_S *a)
{
    memset(a, 0, sizeof(*a));
    a->enSamplerate   = TARGET_RATE;
    a->enBitwidth     = AUDIO_BIT_WIDTH_16;
    a->enWorkmode     = AIO_MODE_I2S_MASTER;
    a->enSoundmode    = AUDIO_SOUND_MODE_MONO;
    a->u32EXFlag      = 0;
    a->u32FrmNum      = FRM_NUM;
    a->u32PtNumPerFrm = PT_PER_FRM;
    a->u32ChnCnt      = 1;
    a->u32ClkSel      = 0;
}

static void report(const char *what, HI_S32 ret)
{
    printf("[%s] %-26s ret=%d %s\n",
           ret == 0 ? "OK" : "!!", what, (int)ret, ret == 0 ? "" : "FAIL");
    fflush(stdout);
}

int main(void)
{
    pthread_t     th;
    HI_S32        ret;
    AIO_ATTR_S    attr;
    AIO_ATTR_S    got;
    AUDIO_FRAME_S frame;
    AEC_FRAME_S   aec;
    int           i, n_ok = 0;
    long          peak = 0;
    double        sum_sq = 0;
    long          total_samples = 0;
    int           clipped = 0;

    printf("=== p1 watchdog + audio begin ===\n");
    printf("pid=%d target=%dHz mono 16bit %dpt/frm\n",
           (int)getpid(), (int)TARGET_RATE, PT_PER_FRM);
    fflush(stdout);

    if (pthread_create(&th, NULL, watchdog_task, NULL) != 0) {
        printf("!! 看门狗线程创建失败\n");
        return 1;
    }
    pthread_detach(th);

    /* 等 rmm 退出、我们接管看门狗。最多等 20 秒。 */
    printf("等待接管看门狗（请在此期间 killall rmm）...\n");
    fflush(stdout);
    for (i = 0; i < 200 && !g_wd_ready; i++)
        usleep(100 * 1000);

    if (!g_wd_ready) {
        printf("!! 20s 内未能接管看门狗，rmm 仍存活？中止以免复位\n");
        return 2;
    }

    /* 此刻 rmm 已退出，音频参数不再被锁定 */
    ret = HI_MPI_SYS_Init();
    report("HI_MPI_SYS_Init", ret);
    if (ret != 0) {
        printf("!! MPP 初始化失败\n");
        return 3;
    }

    fill_attr(&attr);
    ret = HI_MPI_AI_SetPubAttr(AI_DEV, &attr);
    report("AI_SetPubAttr(16k)", ret);

    ret = HI_MPI_AI_Enable(AI_DEV);
    report("AI_Enable", ret);

    ret = HI_MPI_AI_EnableChn(AI_DEV, AI_CHN);
    report("AI_EnableChn", ret);

    memset(&got, 0, sizeof(got));
    if (HI_MPI_AI_GetPubAttr(AI_DEV, &got) == 0) {
        printf("     AI actual: rate=%d width=%d sound=%d ch=%u pt=%u frm=%u\n",
               (int)got.enSamplerate, (int)got.enBitwidth, (int)got.enSoundmode,
               (unsigned)got.u32ChnCnt, (unsigned)got.u32PtNumPerFrm,
               (unsigned)got.u32FrmNum);
        fflush(stdout);
    }

    /* 采 3 秒，统计电平 */
    printf("开始采集 %d 帧 (%d 秒)...\n", FRMS_TO_CAP, CAP_SECONDS);
    fflush(stdout);

    for (i = 0; i < FRMS_TO_CAP; i++) {
        memset(&frame, 0, sizeof(frame));
        memset(&aec, 0, sizeof(aec));
        ret = HI_MPI_AI_GetFrame(AI_DEV, AI_CHN, &frame, &aec, HI_TRUE);
        if (ret != 0) {
            if (i < 5)
                printf("     GetFrame[%d] ret=%d\n", i, (int)ret);
            continue;
        }

        n_ok++;
        if (frame.pVirAddr[0] && frame.u32Len > 0) {
            short *pcm = (short *)frame.pVirAddr[0];
            int    cnt = (int)(frame.u32Len / sizeof(short));
            int    k;
            for (k = 0; k < cnt; k++) {
                long v = pcm[k];
                long a = v < 0 ? -v : v;
                if (a > peak) peak = a;
                if (a >= 32767) clipped++;
                sum_sq += (double)(v * v);
            }
            total_samples += cnt;
        }
        HI_MPI_AI_ReleaseFrame(AI_DEV, AI_CHN, &frame, &aec);
    }

    printf("\n===== 采集统计 =====\n");
    printf("成功取帧      : %d / %d\n", n_ok, FRMS_TO_CAP);
    printf("样本总数      : %ld\n", total_samples);
    if (total_samples > 0) {
        double rms = 0;
        long   guard = total_samples;
        rms = (sum_sq / (double)guard);
        /* 简易整数 sqrt，避免浮点库依赖 */
        {
            double x = rms, y = 0;
            int it;
            if (x > 0) {
                y = x / 2.0;
                for (it = 0; it < 20; it++) y = (y + x / y) / 2.0;
            }
            printf("RMS 电平      : %.0f (满量程 32767)\n", y);
            printf("RMS 百分比    : %.1f%%\n", y * 100.0 / 32767.0);
        }
        printf("峰值          : %ld (%.1f%%)\n", peak, (double)peak * 100.0 / 32767.0);
        printf("削波样本      : %d\n", clipped);
    }

    printf("\n===== 结论 =====\n");
    if (n_ok > 0 && total_samples > 0)
        printf("音频采集通路已打通，16kHz 可用。\n");
    else
        printf("采集失败，需要排查。\n");
    fflush(stdout);

    /* 不主动关闭 AI：本程序退出后设备将复位恢复原厂，无需清理 */
    printf("=== p1 end ===\n");

    /* 保持喂狗若干秒，便于外部读取日志；随后退出，设备看门狗复位 */
    sleep(8);
    g_stop = 1;
    sleep(2);
    return 0;
}
