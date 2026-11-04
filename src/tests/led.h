/*
 * led.h -- 蓝/黄双色 LED 状态指示
 *
 * 逆向来源（两个独立证据，互相印证）：
 *
 *   1) /home/led_ctl（原厂工具，现场改名为 led_ctl.off）反汇编：
 *        set_blue_led(on)   : ioctl(fd, on ? 0x7022 : 0x7023, 0)
 *        set_orange_led(on) : ioctl(fd, on ? 0x7024 : 0x7025, 0)
 *        fd = open("/dev/cpld_periph", O_RDWR)
 *
 *   2) cpld_periph.ko 的 cpld_ioctl 跳表：
 *        0x7022 -> gpio(1)   0x7023 -> gpio(0)   （蓝灯 开/关）
 *        0x7024 -> gpio(1)   0x7025 -> gpio(0)   （黄灯 开/关）
 *
 * LED 只有"亮/灭"，没有硬件呼吸或调光；闪烁一律由软件定时切换实现，
 * 这样闪烁频率可以随状态任意定义，也不额外占用一个 led_ctl 进程。
 *
 * 用法：
 *   led_open();
 *   loop { ... led_tick(LEDP_LISTEN, now_ms()); ... }
 */
#ifndef LED_H
#define LED_H

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#define CPLD_DEV            "/dev/cpld_periph"
#define CPLD_LED_BLUE_ON    0x7022UL
#define CPLD_LED_BLUE_OFF   0x7023UL
#define CPLD_LED_YEL_ON     0x7024UL
#define CPLD_LED_YEL_OFF    0x7025UL

typedef enum {
    LEDP_OFF = 0,     /* 全灭：进程未就绪 */
    LEDP_BOOT,        /* 蓝黄同亮：开机自检 */
    LEDP_WIFI,        /* 蓝灯常亮：联网中 */
    LEDP_CONNECT,     /* 蓝灯快闪 5Hz：TLS/WS 握手、重连 */
    LEDP_IDLE,        /* 黄灯常亮：已连服务端，待命可说话 */
    LEDP_LISTEN,      /* 黄灯闪 2Hz：正在录音 */
    LEDP_THINK,       /* 黄灯慢闪 1Hz：说话结束，等识别/思考 */
    LEDP_SPEAK,       /* 蓝灯闪 2Hz：正在播放 TTS */
    LEDP_ERROR,       /* 黄蓝交替 4Hz：出错 */
    LEDP_NUM
} led_pattern_t;

static int g_led_fd = -1;
static int g_led_blue = -1;      /* 上次实际写入的状态，-1 = 未知 */
static int g_led_yel = -1;

static int led_open(void)
{
    g_led_fd = open(CPLD_DEV, O_RDWR);
    g_led_blue = -1;
    g_led_yel = -1;
    return g_led_fd >= 0 ? 0 : -1;
}

static void led_alloff(void)
{
    if (g_led_fd >= 0) {
        ioctl(g_led_fd, CPLD_LED_BLUE_OFF, 0);
        ioctl(g_led_fd, CPLD_LED_YEL_OFF, 0);
    }
    g_led_blue = 0;
    g_led_yel = 0;
}

/* 只关 fd，不改变 LED 状态（避免单次命令设好状态后又立刻被关掉） */
static void led_close(void)
{
    if (g_led_fd >= 0) {
        close(g_led_fd);
        g_led_fd = -1;
        g_led_blue = -1;
        g_led_yel = -1;
    }
}

/* 只在状态变化时下发 ioctl，避免每 60ms 打一次系统调用 */
static void led_apply(int blue, int yellow)
{
    if (g_led_fd < 0) return;
    if (blue != g_led_blue) {
        ioctl(g_led_fd, blue ? CPLD_LED_BLUE_ON : CPLD_LED_BLUE_OFF, 0);
        g_led_blue = blue;
    }
    if (yellow != g_led_yel) {
        ioctl(g_led_fd, yellow ? CPLD_LED_YEL_ON : CPLD_LED_YEL_OFF, 0);
        g_led_yel = yellow;
    }
}

/*
 * 根据模式和当前时刻计算并下发 LED 状态。
 * 调用频率建议 20~50ms 一次（主循环顺带即可）。
 */
static void led_tick(led_pattern_t p, long long now_ms)
{
    int blue = 0, yellow = 0;

    switch (p) {
    case LEDP_OFF:
        break;
    case LEDP_BOOT:
        blue = 1; yellow = 1;
        break;
    case LEDP_WIFI:
        blue = 1;
        break;
    case LEDP_CONNECT:                       /* 5Hz 快闪 */
        blue = (int)((now_ms / 100) & 1);
        break;
    case LEDP_IDLE:                          /* 黄常亮 */
        yellow = 1;
        break;
    case LEDP_LISTEN:                        /* 黄 2Hz */
        yellow = (int)((now_ms / 250) & 1);
        break;
    case LEDP_THINK:                         /* 黄 1Hz */
        yellow = (int)((now_ms / 500) & 1);
        break;
    case LEDP_SPEAK:                         /* 蓝 2Hz */
        blue = (int)((now_ms / 250) & 1);
        break;
    case LEDP_ERROR:                         /* 黄蓝交替 4Hz */
        if ((now_ms / 125) & 1) yellow = 1;
        else                    blue = 1;
        break;
    default:
        break;
    }
    led_apply(blue, yellow);
}

static const char *led_pattern_name(led_pattern_t p)
{
    switch (p) {
    case LEDP_OFF:     return "off";
    case LEDP_BOOT:    return "boot(both)";
    case LEDP_WIFI:    return "wifi(blue solid)";
    case LEDP_CONNECT: return "connect(blue 5Hz)";
    case LEDP_IDLE:    return "idle(yellow solid)";
    case LEDP_LISTEN:  return "listen(yellow 2Hz)";
    case LEDP_THINK:   return "think(yellow 1Hz)";
    case LEDP_SPEAK:   return "speak(blue 2Hz)";
    case LEDP_ERROR:   return "error(alt 4Hz)";
    default:           return "?";
    }
}

#endif /* LED_H */
