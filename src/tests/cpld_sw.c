/*
 * cpld_sw.c —— 直接操作 cpld_periph 的第一对 GPIO（0x7020 开 / 0x7021 关）
 *
 * 背景
 *   rmm main（反编译 0x39e2c）在放音前 ioctl(cpld, 0x7020)，放音后 ioctl(cpld, 0x7021)。
 *   实测 0x7020 能让 ADEC 播放出声 -> 它至少打开了喇叭功放。
 *   但 p18_micpower.c 曾把 0x7020 列为"可能是麦克风偏置供电"的候选，
 *   所以这对 GPIO 也可能是麦克风偏置 / 音频总电源。
 *
 *   如果 0x7021 关掉的是麦克风供电，那么执行过一次 0x7021 之后，
 *   麦克风会一直没电，而且 CPLD 状态能扛过软重启（只有断电才复位）。
 *
 * 用法
 *   ./cpld_sw on      -> ioctl 0x7020
 *   ./cpld_sw off     -> ioctl 0x7021
 *   ./cpld_sw status  -> 只打开看一眼
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

int main(int argc, char **argv)
{
    const char *m = (argc > 1) ? argv[1] : "status";
    int fd, r;

    setvbuf(stdout, NULL, _IONBF, 0);
    fd = open("/dev/cpld_periph", O_RDWR);
    if (fd < 0) {
        printf("open /dev/cpld_periph failed errno=%d\n", errno);
        return 1;
    }

    if (!strcmp(m, "on")) {
        r = ioctl(fd, 0x7020, 0);
        printf("ioctl 0x7020 ret=%d\n", r);
    } else if (!strcmp(m, "off")) {
        r = ioctl(fd, 0x7021, 0);
        printf("ioctl 0x7021 ret=%d\n", r);
    } else {
        printf("cpld_periph opened fd=%d (no change)\n", fd);
    }

    close(fd);
    return 0;
}
