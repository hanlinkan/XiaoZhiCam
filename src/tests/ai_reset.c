/*
 * ai_reset.c —— 直接对 /dev/ai 发 ioctl，绕过 libmpi 的状态检查
 *
 * 依据（/home/3518/hi3518_ai.ko 反编译，见 XiaoZhiCam/docs/13）
 *   ai_open / ai_close 都是空函数（return 0），不存在引用计数。
 *   NOT_PERM(0xA0158009) 来自 AiSetDevAttr 的两个判断：
 *       g_stAiDev[dev].inited  == 0   -> NOT_PERM
 *       g_stAiDev[dev].enabled != 0   -> "must disable AudioDevId when config dev attr"
 *   rmm 被 SIGKILL 后 enabled 留在 1，而 HI_MPI_AI_Disable() 在库层就返回了错误，
 *   根本没把 0x5a04 发下来。本程序直接发原始 ioctl 试试。
 *
 * ioctl 编号（来自 ai_ioctl 反编译）
 *   0x40045a00  写 file->private_data 的 dev/chn 字段（后续命令都靠它定位设备）
 *   0x5a04      AiDisableDev
 *   0x5a03      AiEnableDev
 *
 * 只做 Disable，不碰任何配置；失败也只是返回错误码，不改状态。
 */

#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>

#define AI_DEV_PATH   "/dev/ai"
#define AI_SET_DEVCHN 0x40045a00UL
#define AI_DISABLE    0x5a04UL
#define AI_ENABLE     0x5a03UL
#define AI_DISABLE_CHN 0x5a0fUL
#define AI_CLR_ATTR    0x5a17UL

/* 关键：AI 设备是 2 通道（u32ChnCnt=2）。
   AiDisableDev 里有一句 "AiDev %d AiChn %d not disabled,
   must disable all chn before disable AI device" —— 只要还有任一通道没关，
   它就直接返回 0xA0158009。之前只关了 chn0，chn1 一直开着，所以永远失败。 */
int main(int argc, char **argv)
{
    int fd, v, ret;
    int do_enable = (argc > 1 && strcmp(argv[1], "enable") == 0);

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== ai_reset begin ===\n");

    fd = open(AI_DEV_PATH, O_RDWR);
    printf("open(%s) = %d  errno=%d\n", AI_DEV_PATH, fd, errno);
    if (fd < 0) return 1;

    /* devchn 编码：dev = v/2，chn = v%2。设备 0 的通道 0/1 分别是 0 和 1。 */
    v = 0;
    errno = 0;
    ret = ioctl(fd, AI_SET_DEVCHN, &v);
    printf("SET_DEVCHN(0)   ret=%d\n", ret);
    errno = 0;
    ret = ioctl(fd, AI_DISABLE_CHN, 0);
    printf("DisableChn(0,0) ret=%d (0x%08X)\n", ret, (unsigned)ret);

    v = 1;
    errno = 0;
    ret = ioctl(fd, AI_SET_DEVCHN, &v);
    printf("SET_DEVCHN(1)   ret=%d\n", ret);
    errno = 0;
    ret = ioctl(fd, AI_DISABLE_CHN, 0);
    printf("DisableChn(0,1) ret=%d (0x%08X)\n", ret, (unsigned)ret);

    v = 0;
    errno = 0;
    ret = ioctl(fd, AI_SET_DEVCHN, &v);
    printf("SET_DEVCHN(0)   ret=%d\n", ret);
    errno = 0;
    ret = ioctl(fd, AI_DISABLE, 0);
    printf("AiDisableDev    ret=%d (0x%08X)\n", ret, (unsigned)ret);

    /* AiClrAttr：把硬件属性整个清掉（要求 enabled != 1，上一步已满足）。
       不清的话，紧接着 SetPubAttr(16000) 会返回 0xA0158003 ILLEGAL_PARAM。 */
    errno = 0;
    ret = ioctl(fd, AI_CLR_ATTR, 0);
    printf("AiClrAttr       ret=%d (0x%08X)\n", ret, (unsigned)ret);

    if (do_enable) {
        errno = 0;
        ret = ioctl(fd, AI_ENABLE, 0);
        printf("AiEnableDev     ret=%d (0x%08X)\n", ret, (unsigned)ret);
    }

    close(fd);
    printf("=== ai_reset end ===\n");
    return 0;
}
