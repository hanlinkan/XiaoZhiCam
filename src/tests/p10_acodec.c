/*
 * p10_acodec.c -- 查看/调整 /dev/acodec。
 *
 * 用法:
 *   p10_acodec                 只读打印
 *   p10_acodec hpf 0|1         ADC 高通滤波
 *   p10_acodec mixer 0|1       0=linein, 1=micin
 *   p10_acodec gainl 0..31     左声道 MIC PGA 增益
 *   p10_acodec gainr 0..31     右声道 MIC PGA 增益
 *   p10_acodec adcl vol mute   左 ADC 数字音量 (vol 0..0x7e, mute 0/1)
 *   p10_acodec adcr vol mute   右 ADC 数字音量
 *   p10_acodec dacl vol mute   左 DAC 音量
 *   p10_acodec dacr vol mute   右 DAC 音量
 *   p10_acodec invol 0..0x7e   输入总音量
 *   p10_acodec outvol 0..0x7e  输出总音量
 *
 * 不带参数时绝不写寄存器。
 */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>

#include "acodec.h"

static void show_u32(int fd, const char *name, unsigned long req)
{
    unsigned int v = 0xDEADBEEF;
    int r = ioctl(fd, req, &v);
    printf("%-22s ret=%d errno=%d value=0x%08x (%u)\n",
           name, r, r ? errno : 0, v, v);
}

static void show_vol(int fd, const char *name, unsigned long req)
{
    ACODEC_VOL_CTRL vol;
    memset(&vol, 0, sizeof(vol));
    if (ioctl(fd, req, &vol) == 0)
        printf("%-22s vol=0x%02x (%u) mute=%u\n",
               name, vol.vol_ctrl, vol.vol_ctrl, vol.vol_ctrl_mute);
    else
        printf("%-22s failed errno=%d\n", name, errno);
}

static int set_u32(int fd, const char *name, unsigned long req, unsigned int v)
{
    int r = ioctl(fd, req, &v);
    printf("SET %-18s %u -> ret=%d errno=%d\n", name, v, r, r ? errno : 0);
    return r;
}

static int set_vol(int fd, const char *name, unsigned long req,
                   unsigned int vol, unsigned int mute)
{
    ACODEC_VOL_CTRL v;
    int r;
    v.vol_ctrl = vol;
    v.vol_ctrl_mute = mute;
    r = ioctl(fd, req, &v);
    printf("SET %-18s vol=%u mute=%u -> ret=%d errno=%d\n",
           name, vol, mute, r, r ? errno : 0);
    return r;
}

int main(int argc, char **argv)
{
    int fd = open("/dev/acodec", O_RDWR);
    unsigned int v;

    if (fd < 0) {
        printf("open /dev/acodec failed: %s\n", strerror(errno));
        return 1;
    }

    if (argc >= 3) {
        const char *cmd = argv[1];
        if (!strcmp(cmd, "hpf")) {
            set_u32(fd, "ADC_HP_FILTER", ACODEC_SET_ADC_HP_FILTER,
                    (unsigned int)atoi(argv[2]));
        } else if (!strcmp(cmd, "mixer")) {
            set_u32(fd, "MIXER_MIC(0=line,1=mic)", ACODEC_SET_MIXER_MIC,
                    (unsigned int)atoi(argv[2]));
        } else if (!strcmp(cmd, "gainl")) {
            set_u32(fd, "GAIN_MICL", ACODEC_SET_GAIN_MICL,
                    (unsigned int)atoi(argv[2]));
        } else if (!strcmp(cmd, "gainr")) {
            set_u32(fd, "GAIN_MICR", ACODEC_SET_GAIN_MICR,
                    (unsigned int)atoi(argv[2]));
        } else if (!strcmp(cmd, "adcl")) {
            set_vol(fd, "ADCL_VOL", ACODEC_SET_ADCL_VOL,
                    (unsigned int)strtoul(argv[2], NULL, 0),
                    argc > 3 ? (unsigned int)atoi(argv[3]) : 0);
        } else if (!strcmp(cmd, "adcr")) {
            set_vol(fd, "ADCR_VOL", ACODEC_SET_ADCR_VOL,
                    (unsigned int)strtoul(argv[2], NULL, 0),
                    argc > 3 ? (unsigned int)atoi(argv[3]) : 0);
        } else if (!strcmp(cmd, "dacl")) {
            set_vol(fd, "DACL_VOL", ACODEC_SET_DACL_VOL,
                    (unsigned int)strtoul(argv[2], NULL, 0),
                    argc > 3 ? (unsigned int)atoi(argv[3]) : 0);
        } else if (!strcmp(cmd, "dacr")) {
            set_vol(fd, "DACR_VOL", ACODEC_SET_DACR_VOL,
                    (unsigned int)strtoul(argv[2], NULL, 0),
                    argc > 3 ? (unsigned int)atoi(argv[3]) : 0);
        } else if (!strcmp(cmd, "invol")) {
            set_u32(fd, "INPUT_VOL", ACODEC_SET_INPUT_VOL,
                    (unsigned int)strtoul(argv[2], NULL, 0));
        } else if (!strcmp(cmd, "outvol")) {
            set_u32(fd, "OUTPUT_VOL", ACODEC_SET_OUTPUT_VOL,
                    (unsigned int)strtoul(argv[2], NULL, 0));
        } else {
            printf("unknown command: %s\n", cmd);
            close(fd);
            return 2;
        }
    }

    printf("=== acodec current settings ===\n");
    show_u32(fd, "GET_INPUT_VOL", ACODEC_GET_INPUT_VOL);
    show_u32(fd, "GET_OUTPUT_VOL", ACODEC_GET_OUTPUT_VOL);
    show_u32(fd, "GET_GAIN_MICL", ACODEC_GET_GAIN_MICL);
    show_u32(fd, "GET_GAIN_MICR", ACODEC_GET_GAIN_MICR);
    show_vol(fd, "GET_ADCL_VOL", ACODEC_GET_ADCL_VOL);
    show_vol(fd, "GET_ADCR_VOL", ACODEC_GET_ADCR_VOL);
    show_vol(fd, "GET_DACL_VOL", ACODEC_GET_DACL_VOL);
    show_vol(fd, "GET_DACR_VOL", ACODEC_GET_DACR_VOL);

    (void)v;
    close(fd);
    return 0;
}
