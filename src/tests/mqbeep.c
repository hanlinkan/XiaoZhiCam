/*
 * mqbeep —— 向 rmm 的 /ipc_rmm 发一条 0x1614，触发它播放
 * /home/forbidden.g726（当前是个符号链接，指向我们的段文件）。
 *
 * 用途：作为"rmm 正常播放"的对照样本，配合 /proc/umap/adec 与
 * /proc/umap/ao 的采样，判断 ADEC / AO 在真实播放时到底是什么状态。
 *
 * 用法：
 *   ./mqbeep            发一条 0x1614
 *   ./mqbeep N          连发 N 条（间隔 100ms）
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <unistd.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    unsigned char msg[16];
    mqd_t mq;
    int i, n = 1;

    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc > 1) n = atoi(argv[1]);
    if (n < 1) n = 1;

    for (i = 0; i < n; i++) {
        mq = mq_open("/ipc_rmm", O_WRONLY | O_NONBLOCK);
        if (mq == (mqd_t)-1) {
            printf("mq_open failed errno=%d\n", errno);
            return 1;
        }
        memset(msg, 0, sizeof(msg));
        msg[8] = 0x14;
        msg[9] = 0x16;                 /* u16 LE 0x1614 */
        if (mq_send(mq, (char *)msg, sizeof(msg), 0) != 0)
            printf("[%d] mq_send failed errno=%d\n", i, errno);
        else
            printf("[%d] sent 0x1614\n", i);
        mq_close(mq);
        if (i + 1 < n) usleep(100000);
    }
    return 0;
}
