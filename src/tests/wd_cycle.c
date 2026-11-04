/*
 * wd_cycle.c — 用 open/close 循环喂狗
 *
 * 原理（来自 hidog 驱动分析）
 *   - open()   : 状态 SELFCLR → EXTCLR，内核守护线程 hidog 停止代劳
 *   - close()  : 走 release()，其中会调用 hidog_keepalive() 喂一次狗，
 *                并把状态切回 HIDOG_SELFCLR，hidog 线程重新接管
 *   - 若 nowayout=1，close 时会打印 "Unexpected close, not stopping watchdog!"
 *     但仍会 keepalive，看门狗继续跑
 *
 * 所以只要能周期性 open+close，就能持续喂狗，且不长期霸占设备。
 * 这也解释了之前为什么失败：我们 open 后一直不 close，卡在 EXTCLR，
 * 内核不喂、用户态的 ioctl 又不被接受，等于自己把唯一的保底掐断了。
 *
 * 喂狗周期取 3s：小于 timeout(10s) 的一半，留足调度余量。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#define LOG_PATH     "/tmp/hd1/wd3.log"
#define CYCLE_SEC    3
#define TOTAL_SEC    60

int main(void)
{
    FILE *log;
    int   i;
    int   ok = 0, fail = 0;
    int   cycles = TOTAL_SEC / CYCLE_SEC;

    log = fopen(LOG_PATH, "w");
    if (log == NULL) log = fopen("/tmp/wd3.log", "w");
    if (log == NULL) log = stdout;
    setvbuf(log, NULL, _IOLBF, 0);

    fprintf(log, "=== wd_cycle start pid=%d cycle=%ds total=%ds ===\n",
            (int)getpid(), CYCLE_SEC, TOTAL_SEC);
    fflush(log);

    for (i = 0; i < cycles; i++) {
        int fd;
        errno = 0;
        fd = open("/dev/watchdog", O_WRONLY);
        if (fd >= 0) {
            close(fd);                 /* release() 内会 keepalive 并切回 SELFCLR */
            ok++;
            if (i < 5 || (i % 5) == 0) {
                fprintf(log, "[%2d] open+close ok\n", i);
                fflush(log);
            }
        } else {
            fail++;
            if (i < 5 || (i % 5) == 0) {
                fprintf(log, "[%2d] open fail errno=%d (%s)\n",
                        i, errno, strerror(errno));
                fflush(log);
            }
        }
        sleep(CYCLE_SEC);
    }

    fprintf(log, "=== SURVIVED %ds  ok=%d fail=%d ===\n", TOTAL_SEC, ok, fail);
    fflush(log);
    if (log != stdout) fclose(log);
    return 0;
}
