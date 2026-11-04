/*
 * wd_ioctl_test.c — 验证海思看门狗的喂狗方式
 *
 * 上一轮实测结论：
 *   write(fd, "*", 1) 返回 1、errno=0，但设备仍在 9 秒后复位。
 *   → 普通 write 不构成喂狗，必须改用 ioctl(WDIOC_KEEPALIVE)。
 *
 * 本程序在接管成功后依次做三件事，日志写 SD 卡（复位后仍可读）：
 *   1. ioctl(WDIOC_GETTIMEOUT)   读出驱动的真实超时值（决定喂狗周期）
 *   2. ioctl(WDIOC_GETSUPPORT)   读出驱动名与支持的选项（确认是不是标准 wdt）
 *   3. 每 500ms 一次 ioctl(WDIOC_KEEPALIVE)，持续 30 秒
 *
 * 判读：
 *   存活满 30 秒 → ioctl 正确，方案成立
 *   仍在数秒后复位 → 该驱动连 ioctl 也不喂，需查 /proc 或改用 gpio 复位对策
 *
 * 注意：喂狗周期用 500ms，比可能的超时短得多，先保证续命再谈优化。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>

#define LOG_PATH        "/tmp/hd1/wd2.log"
#define OPEN_TIMEOUT_S  45
#define KEEPALIVE_SEC   30
#define FEED_INTERVAL_US (500 * 1000)

/* 目标 uClibc 工具链不带 linux/watchdog.h，这里按内核 uapi 手写常量。
 * 数值来自 include/uapi/linux/watchdog.h，_IOR/_IOWR 由 sys/ioctl.h 展开。 */
#define WDIOC_GETSTATUS      _IOR('W', 1, int)
#define WDIOC_SETOPTIONS     _IOR('W', 4, int)
#define WDIOC_KEEPALIVE      _IOR('W', 5, int)
#define WDIOC_SETTIMEOUT     _IOWR('W', 6, int)
#define WDIOC_GETTIMEOUT     _IOR('W', 7, int)

#define WDIOS_DISABLECARD    0x0001
#define WDIOS_ENABLECARD     0x0002

/* WDIOC_GETSUPPORT 的返回结构，只取前两个字段，够识别驱动 */
struct wd_info {
    unsigned int  options;
    unsigned int  firmware_version;
    char          identity[32];
};

/* 必须放在 struct 之后：_IOR 的第三个参数决定 sizeof，此处为 40 字节 */
#define WDIOC_GETSUPPORT     _IOR('W', 0, struct wd_info)

int main(void)
{
    FILE *log;
    int   fd = -1;
    int   attempt;
    int   i;
    int   feeds;
    int   tout = -1;
    int   disabled = 0;
    struct wd_info info;

    log = fopen(LOG_PATH, "w");
    if (log == NULL) log = fopen("/tmp/wd2.log", "w");
    if (log == NULL) log = stdout;
    setvbuf(log, NULL, _IOLBF, 0);

    fprintf(log, "=== wd_ioctl_test start pid=%d ===\n", (int)getpid());
    fflush(log);

    /* 接管 */
    for (attempt = 0; attempt < OPEN_TIMEOUT_S * 10; attempt++) {
        fd = open("/dev/watchdog", O_WRONLY);
        if (fd >= 0) break;
        if (attempt % 10 == 0) {
            fprintf(log, "[%3d] open fail errno=%d (%s)\n",
                    attempt, errno, strerror(errno));
            fflush(log);
        }
        usleep(100 * 1000);
    }
    if (fd < 0) {
        fprintf(log, "GIVE UP after %ds\n", OPEN_TIMEOUT_S);
        fflush(log);
        if (log != stdout) fclose(log);
        return 1;
    }
    fprintf(log, "[%3d] OPEN OK fd=%d (%.1fs)\n", attempt, fd, attempt * 0.1);
    fflush(log);

    /* 读驱动信息 */
    memset(&info, 0, sizeof(info));
    if (ioctl(fd, WDIOC_GETSUPPORT, &info) == 0) {
        fprintf(log, "driver identity=\"%s\" options=0x%x fw=%u\n",
                info.identity, info.options, info.firmware_version);
    } else {
        fprintf(log, "GETSUPPORT failed errno=%d\n", errno);
    }

    /* 读超时 */
    tout = -1;
    if (ioctl(fd, WDIOC_GETTIMEOUT, &tout) == 0) {
        fprintf(log, "timeout = %d s\n", tout);
    } else {
        fprintf(log, "GETTIMEOUT failed errno=%d (%s)\n", errno, strerror(errno));
    }
    fflush(log);

    /* 关键尝试：直接关闭看门狗。
     *
     * 实测发现 WDIOC_KEEPALIVE 返回 0 却不重置倒计时：
     * 超时 10s，rmm 最后一次喂狗在它被杀前约 4s，
     * 我们 open 后连续喂了 5.5s，仍在第 5.7s 复位——
     * 说明这颗驱动不吃 KEEPALIVE，倒计时一旦启动就不回头。
     *
     * 那就干脆关掉。options=0x8180 含 WDIOF_MAGICCLOSE，
     * 正常情况下允许 DISABLECARD。 */
    {
        int opt = WDIOS_DISABLECARD;
        errno = 0;
        if (ioctl(fd, WDIOC_SETOPTIONS, &opt) == 0) {
            disabled = 1;
            fprintf(log, "*** DISABLECARD ok -> watchdog stopped ***\n");
        } else {
            fprintf(log, "DISABLECARD failed errno=%d (%s)\n", errno, strerror(errno));
            opt = WDIOS_ENABLECARD;
            if (ioctl(fd, WDIOC_SETOPTIONS, &opt) == 0)
                fprintf(log, "fallback ENABLE ok, will keep feeding\n");
            else
                fprintf(log, "fallback ENABLE failed errno=%d\n", errno);
        }
        fflush(log);
    }

    /* 主循环：未关闭则喂狗，已关闭则只打心跳 */
    feeds = KEEPALIVE_SEC * 1000000 / FEED_INTERVAL_US;
    for (i = 0; i < feeds; i++) {
        if (disabled) {
            if ((i % 10) == 0) {
                fprintf(log, "tick %3d  (watchdog disabled, not feeding)\n", i);
                fflush(log);
            }
        } else {
            int rc_k, rc_w;
            errno = 0;
            rc_k = ioctl(fd, WDIOC_KEEPALIVE, 0);
            rc_w = (int)write(fd, "\0", 1);
            if (i < 4 || (i % 10) == 0) {
                fprintf(log, "feed %3d  ioctl=%d(w=%d)  write=%d\n",
                        i, rc_k, errno, rc_w);
                fflush(log);
            }
        }
        usleep(FEED_INTERVAL_US);
    }

    fprintf(log, "=== SURVIVED %ds, closing ===\n", KEEPALIVE_SEC);
    fflush(log);
    write(fd, "V", 1);
    close(fd);
    if (log != stdout) fclose(log);
    return 0;
}
