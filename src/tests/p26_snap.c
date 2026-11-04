/*
 * p26_snap.c -- 验证 rmm mq 命令 0x1610（JPEG 抓拍）是否可用
 *
 * 背景：rmm 一直在跑且已创建 VENC JPEG 通道（chn 3），0x1610 的处理逻辑是
 *   memcpy(name, msg+0x10, *(u32*)(msg+0xc))
 *   fopen("<name>.tmp","wb") -> VENC_StartRecvPic(3) -> select -> GetStream
 *   -> fwrite -> (见 rmm_msg_handle)
 * 所以只要发 16+len 字节的 mq 消息就能拿到一张全画质 JPEG，
 * 完全不用碰 MPP，也不用担心和 rmm 抢 VI/VPSS。
 *
 * 消息布局（与 0x1614 一致，见 p25_beep.c）：
 *   +0  .. +7   保留
 *   +8  u16 LE  命令号 0x1610
 *   +0xc u32 LE 文件名长度（不含结尾 0）
 *   +0x10      文件名
 *
 * 编译：
 *   TC=~/theplace/XiaoZhiCam/toolchains/arm-hisiv100nptl-linux/arm-hisiv100-linux/bin/arm-hisiv100-linux-uclibcgnueabi-gcc
 *   $TC -Os -mcpu=arm926ej-s -mfloat-abi=soft -o p26s p26_snap.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <sys/stat.h>
#include <time.h>

#define MQ_NAME   "/ipc_rmm"
#define CMD_SNAP  0x1610

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/tmp/snap.jpg";
    unsigned char msg[512];
    mqd_t mq;
    int len = (int)strlen(path);
    long long t0;
    struct stat st;

    if (len < 1 || len > 480) {
        fprintf(stderr, "path too long\n");
        return 2;
    }

    unlink(path);
    {
        char tmp[520];
        snprintf(tmp, sizeof(tmp), "%s.tmp", path);
        unlink(tmp);
    }

    memset(msg, 0, sizeof(msg));
    msg[8] = (unsigned char)(CMD_SNAP & 0xff);
    msg[9] = (unsigned char)((CMD_SNAP >> 8) & 0xff);
    msg[0xc] = (unsigned char)(len & 0xff);
    msg[0xd] = (unsigned char)((len >> 8) & 0xff);
    msg[0xe] = (unsigned char)((len >> 16) & 0xff);
    msg[0xf] = (unsigned char)((len >> 24) & 0xff);
    memcpy(msg + 0x10, path, (size_t)len);

    mq = mq_open(MQ_NAME, O_WRONLY | O_NONBLOCK);
    if (mq == (mqd_t)-1) {
        printf("mq_open %s failed: %s\n", MQ_NAME, strerror(errno));
        return 1;
    }
    if (mq_send(mq, (char *)msg, (size_t)(0x10 + len), 0) != 0) {
        printf("mq_send failed: %s\n", strerror(errno));
        mq_close(mq);
        return 1;
    }
    mq_close(mq);
    printf("sent 0x1610 path=%s len=%d\n", path, len);

    t0 = now_ms();
    while (now_ms() - t0 < 5000) {
        if (stat(path, &st) == 0 && st.st_size > 0) {
            /* 确认不是还在写的 .tmp */
            char tmp[520];
            struct stat stt;
            snprintf(tmp, sizeof(tmp), "%s.tmp", path);
            if (stat(tmp, &stt) != 0) {
                printf("OK %s size=%ld after %lld ms\n",
                       path, (long)st.st_size, now_ms() - t0);
                return 0;
            }
        }
        usleep(100 * 1000);
    }

    if (stat(path, &st) == 0)
        printf("TIMEOUT but file exists size=%ld\n", (long)st.st_size);
    else
        printf("TIMEOUT no file\n");
    return 1;
}
