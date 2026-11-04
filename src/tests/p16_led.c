/*
 * p16_led.c -- 蓝/黄双色 LED 控制与状态演示
 *
 * 逆向依据：
 *   /home/led_ctl.off 反汇编 ->
 *       set_blue_led(on)   : ioctl(fd, 0x7022/0x7023, 0)
 *       set_orange_led(on) : ioctl(fd, 0x7024/0x7025, 0)
 *   cpld_periph.ko 的 cpld_ioctl 跳表证实 0x7022..0x7025 是两对 GPIO 开关。
 *
 * 用法:
 *   p16_led demo            按 p8 客户端的完整状态映射演示一遍（推荐先跑这个）
 *   p16_led on  blue|yel    常亮
 *   p16_led off blue|yel    熄灭
 *   p16_led alloff          两个都灭
 *   p16_led show <pattern>  单个模式，pattern = off|boot|wifi|connect|idle|listen|think|speak|error
 *   p16_led hold <ms>       保持当前不动若干毫秒
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/time.h>

#include "led.h"

static void run_pattern(led_pattern_t p, int ms)
{
    long long t0 = 0, now;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    t0 = (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
    printf("  %-24s %dms\n", led_pattern_name(p), ms);
    fflush(stdout);
    for (;;) {
        gettimeofday(&tv, NULL);
        now = (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
        if (now - t0 >= ms) break;
        led_tick(p, now - t0);
        usleep(20000);
    }
}

int main(int argc, char **argv)
{
    const char *mode = (argc > 1) ? argv[1] : "demo";

    if (led_open() != 0) {
        printf("open %s failed errno=%d\n", CPLD_DEV, errno);
        return 1;
    }
    printf("led opened, fd=%d\n", g_led_fd);

    if (!strcmp(mode, "alloff")) {
        led_alloff();
        printf("all off\n");
    } else if (!strcmp(mode, "on") && argc >= 3) {
        if (!strcmp(argv[2], "blue"))      led_apply(1, 0);
        else if (!strcmp(argv[2], "yel"))  led_apply(0, 1);
        else { printf("bad color\n"); return 2; }
        printf("on %s\n", argv[2]);
    } else if (!strcmp(mode, "off") && argc >= 3) {
        if (!strcmp(argv[2], "blue"))      led_apply(0, 0);
        else if (!strcmp(argv[2], "yel"))  led_apply(0, 0);
        else { printf("bad color\n"); return 2; }
        printf("off %s\n", argv[2]);
    } else if (!strcmp(mode, "hold") && argc >= 3) {
        int ms = atoi(argv[2]);
        usleep((useconds_t)ms * 1000);
    } else if (!strcmp(mode, "color")) {
        /* 颜色辨识：黄常亮 10s -> 灭 2s -> 蓝常亮 10s -> 灭 2s，共 5 轮 */
        int r;
        int rounds = (argc >= 3) ? atoi(argv[2]) : 5;
        printf("=== 颜色辨识：先 YEL 10s，再 BLUE 10s，共 %d 轮 ===\n", rounds);
        for (r = 0; r < rounds; r++) {
            printf("  round %d: YELLOW on (0x7024)\n", r + 1); fflush(stdout);
            led_apply(0, 1); sleep(10);
            printf("  round %d: all off\n", r + 1); fflush(stdout);
            led_apply(0, 0); sleep(2);
            printf("  round %d: BLUE on (0x7022)\n", r + 1); fflush(stdout);
            led_apply(1, 0); sleep(10);
            printf("  round %d: all off\n", r + 1); fflush(stdout);
            led_apply(0, 0); sleep(2);
        }
        led_apply(0, 0);
        printf("=== 结束，已全灭 ===\n");
    } else if (!strcmp(mode, "show") && argc >= 3) {
        const char *n = argv[2];
        led_pattern_t p = LEDP_OFF;
        if (!strcmp(n, "off"))          p = LEDP_OFF;
        else if (!strcmp(n, "boot"))    p = LEDP_BOOT;
        else if (!strcmp(n, "wifi"))    p = LEDP_WIFI;
        else if (!strcmp(n, "connect")) p = LEDP_CONNECT;
        else if (!strcmp(n, "idle"))    p = LEDP_IDLE;
        else if (!strcmp(n, "listen"))  p = LEDP_LISTEN;
        else if (!strcmp(n, "think"))   p = LEDP_THINK;
        else if (!strcmp(n, "speak"))   p = LEDP_SPEAK;
        else if (!strcmp(n, "error"))   p = LEDP_ERROR;
        else { printf("bad pattern\n"); return 2; }
        run_pattern(p, (argc >= 4) ? atoi(argv[3]) : 3000);
    } else {
        /* demo：完整状态映射，每段 2.5 秒 */
        printf("=== LED 状态映射演示 ===\n");
        run_pattern(LEDP_BOOT,    2000);   /* 蓝黄同亮：开机自检 */
        run_pattern(LEDP_WIFI,    2500);   /* 蓝常亮：联网中 */
        run_pattern(LEDP_CONNECT, 2500);   /* 蓝快闪：握手/重连 */
        run_pattern(LEDP_IDLE,    2500);   /* 黄常亮：待命 */
        run_pattern(LEDP_LISTEN,  3000);   /* 黄 2Hz：正在录音 */
        run_pattern(LEDP_THINK,   2500);   /* 黄 1Hz：等待识别 */
        run_pattern(LEDP_SPEAK,   3000);   /* 蓝 2Hz：正在播放 */
        run_pattern(LEDP_ERROR,   2000);   /* 黄蓝交替：错误 */
        led_apply(0, 0);
        printf("=== 演示结束，已全灭 ===\n");
    }

    led_close();
    return 0;
}
