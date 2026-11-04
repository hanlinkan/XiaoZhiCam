/*
 * wd_takeover.c — 只做一件事：接管 /dev/watchdog
 *
 * 背景
 *   rmm 持有 /dev/watchdog。杀掉 rmm 后如果没人喂狗，整机会在数秒内复位。
 *   前一次尝试在接管的同时还初始化了音频，日志又写在 tmpfs 上，
 *   复位后什么都看不到，无法判断是 open 失败还是 write 无效。
 *
 * 本程序只验证看门狗本身：
 *   1. 每 100ms 尝试一次 open，把 errno 完整打出来（区分 EBUSY / ENOENT / EACCES）
 *   2. open 成功后每 1s write 一字节，记录返回值
 *   3. 日志写 **SD 卡 /tmp/hd1/wd.log**，tmpfs 会被复位清空，SD 卡不会
 *   4. 全部用行缓冲 + fflush，保证崩溃前最后一行落盘
 *
 * 判读方式
 *   日志停在 "open fail"   → 一直没拿到设备，看门狗另有主
 *   日志停在 "feed N"      → 拿到了但 write 不喂狗，需要改用 ioctl(WDIOC_KEEPALIVE)
 *   日志一直写到 60        → 接管成功
 *   日志出现 CLOSED        → 正常释放
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#define LOG_PATH       "/tmp/hd1/wd.log"
#define OPEN_TIMEOUT_S 60
#define FEED_COUNT     60

int main(void)
{
    FILE *log;
    int   fd = -1;
    int   i;
    int   attempt = 0;
    int   max_attempt = OPEN_TIMEOUT_S * 10;

    /* 优先写 SD 卡：复位后仍可读 */
    log = fopen(LOG_PATH, "w");
    if (log == NULL) {
        log = fopen("/tmp/wd.log", "w");
    }
    if (log == NULL) {
        log = stdout;
    }
    setvbuf(log, NULL, _IOLBF, 0);

    fprintf(log, "=== wd_takeover start pid=%d ===\n", (int)getpid());
    fflush(log);

    /* 先看一眼设备节点是否存在、主次设备号是多少 */
    {
        int probe = open("/dev/watchdog", O_WRONLY | O_NONBLOCK);
        if (probe >= 0) {
            fprintf(log, "probe open OK (rmm not holding?)\n");
            close(probe);
        } else {
            fprintf(log, "probe open fail errno=%d (%s)\n", errno, strerror(errno));
        }
        fflush(log);
    }

    /* 循环尝试接管 */
    while (fd < 0 && attempt < max_attempt) {
        fd = open("/dev/watchdog", O_WRONLY);
        if (fd < 0) {
            if (attempt % 10 == 0) {
                fprintf(log, "[%3d] open fail errno=%d (%s)\n",
                        attempt, errno, strerror(errno));
                fflush(log);
            }
            usleep(100 * 1000);
            attempt++;
            continue;
        }
        fprintf(log, "[%3d] OPEN OK fd=%d  (%.1fs after start)\n",
                attempt, fd, attempt * 0.1);
        fflush(log);
    }

    if (fd < 0) {
        fprintf(log, "GIVE UP: %d attempts, cannot open watchdog\n", attempt);
        fflush(log);
        if (log != stdout) fclose(log);
        return 1;
    }

    /* 喂狗 */
    for (i = 0; i < FEED_COUNT; i++) {
        ssize_t n;
        errno = 0;
        n = write(fd, "*", 1);
        fprintf(log, "feed %2d: n=%d errno=%d\n", i, (int)n, errno);
        fflush(log);
        sleep(1);
    }

    /* 标准 magic close：写完 'V' 再关，避免关闭即复位 */
    write(fd, "V", 1);
    close(fd);
    fprintf(log, "=== CLOSED GRACEFULLY ===\n");
    fflush(log);
    if (log != stdout) fclose(log);
    return 0;
}
