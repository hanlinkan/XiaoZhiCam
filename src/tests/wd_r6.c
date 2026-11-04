/*
 * wd_r6.c — 按 rmm 的真实序列接管看门狗
 *
 * 依据：反汇编 rmm_fast3 的 watchdog_enable / watchdog_feed
 *
 *   watchdog_enable:
 *     fd = open("/dev/watchdog", 0);            // flags = 0，即 O_RDONLY
 *     int timeout = 10;
 *     ioctl(fd, WDIOC_SETTIMEOUT, &timeout);    // 0xc0045706
 *     int opt = WDIOS_ENABLECARD;               // 2
 *     ioctl(fd, WDIOC_SETOPTIONS, &opt);         // 0x80045704
 *
 *   watchdog_feed:
 *     ioctl(fd, WDIOC_KEEPALIVE, 0);            // 0x80045705
 *
 * 之前失败的根因：
 *   1. open 用了 O_WRONLY，rmm 用的是 O_RDONLY（flags=0）
 *   2. 只调了 KEEPALIVE，没调 SETTIMEOUT（重置计数）和 SETOPTIONS(ENABLECARD)
 *   3. 更严重的是之前调的是 DISABLECARD，方向反了
 *
 * 驱动语义：HIDOG_SELFCLR 状态下，只有 options 含 WDIOS_ENABLECARD 时
 * hidog 内核线程才会代喂；置于 EXTCLR（用户 open 后）则由用户负责。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>

#define LOG_PATH        "/tmp/hd1/wd6.log"
#define OPEN_TIMEOUT_S  45
#define RUNTIME_SEC     90
#define FEED_MS         3000
#define WD_TIMEOUT_S    10

#define WDIOC_SETOPTIONS     _IOR('W', 4, int)
#define WDIOC_KEEPALIVE      _IOR('W', 5, int)
#define WDIOC_SETTIMEOUT     _IOWR('W', 6, int)
#define WDIOC_GETTIMEOUT     _IOR('W', 7, int)

#define WDIOS_ENABLECARD     0x0002

int main(void)
{
    FILE *log;
    int   fd = -1;
    int   attempt;
    int   timeout = WD_TIMEOUT_S;
    int   opt     = WDIOS_ENABLECARD;
    int   elapsed = 0;
    int   feeds   = 0;

    log = fopen(LOG_PATH, "w");
    if (log == NULL) log = fopen("/tmp/wd6.log", "w");
    if (log == NULL) log = stdout;
    setvbuf(log, NULL, _IOLBF, 0);

    fprintf(log, "=== wd_r6 start pid=%d (复制 rmm 的接管序列) ===\n", (int)getpid());
    fflush(log);

    /* 1. 等待 rmm 释放设备。注意用 O_RDONLY，与 rmm 一致。 */
    for (attempt = 0; attempt < OPEN_TIMEOUT_S * 10; attempt++) {
        fd = open("/dev/watchdog", O_RDONLY);
        if (fd >= 0) break;
        if (attempt % 10 == 0) {
            fprintf(log, "[%3d] open(O_RDONLY) fail errno=%d (%s)\n",
                    attempt, errno, strerror(errno));
            fflush(log);
        }
        usleep(100 * 1000);
    }
    if (fd < 0) {
        fprintf(log, "GIVE UP: %ds 内无法打开\n", OPEN_TIMEOUT_S);
        fflush(log);
        if (log != stdout) fclose(log);
        return 1;
    }
    fprintf(log, "[%3d] OPEN OK fd=%d (O_RDONLY)\n", attempt, fd);
    fflush(log);

    /* 2. SETTIMEOUT —— 重置计数器，这是之前漏掉的关键一步 */
    errno = 0;
    if (ioctl(fd, WDIOC_SETTIMEOUT, &timeout) == 0) {
        fprintf(log, "SETTIMEOUT(%d) ok\n", timeout);
    } else {
        fprintf(log, "SETTIMEOUT fail errno=%d (%s)\n", errno, strerror(errno));
    }
    fflush(log);

    /* 3. ENABLECARD —— 让 hidog 知道看门狗处于启用态 */
    errno = 0;
    if (ioctl(fd, WDIOC_SETOPTIONS, &opt) == 0) {
        fprintf(log, "SETOPTIONS(ENABLECARD) ok\n");
    } else {
        fprintf(log, "SETOPTIONS(ENABLECARD) fail errno=%d (%s)\n",
                errno, strerror(errno));
    }
    fflush(log);

    /* 4. 立刻喂一次，别等 */
    errno = 0;
    fprintf(log, "first KEEPALIVE = %d (errno=%d)\n",
            (int)ioctl(fd, WDIOC_KEEPALIVE, 0), errno);
    fflush(log);

    /* 5. 喂狗循环 */
    while (elapsed < RUNTIME_SEC) {
        usleep(FEED_MS * 1000);
        elapsed += FEED_MS / 1000;
        errno = 0;
        if (ioctl(fd, WDIOC_KEEPALIVE, 0) != 0) {
            fprintf(log, "[%3ds] KEEPALIVE FAIL errno=%d (%s)\n",
                    elapsed, errno, strerror(errno));
            fflush(log);
            break;
        }
        feeds++;
        if ((feeds % 5) == 0) {
            int cur = -1;
            ioctl(fd, WDIOC_GETTIMEOUT, &cur);
            fprintf(log, "[%3ds] alive, feeds=%d, timeout_now=%d\n",
                    elapsed, feeds, cur);
            fflush(log);
        }
    }

    fprintf(log, "=== SURVIVED %ds, feeds=%d ===\n", elapsed, feeds);
    fflush(log);
    close(fd);
    if (log != stdout) fclose(log);
    return 0;
}
