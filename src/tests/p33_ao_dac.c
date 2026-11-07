/*
 * p33_ao_dac.c —— 自己控 DAC 音量寄存器再送 AO 帧
 *
 * 为什么需要它
 *   hi3518_sio.ko 的 SIODevEnableAoTx 会做防爆音斜坡：
 *   DAC 音量 79/79 -> 69 -> 59 -> ... -> 9/9 (0x09092424)。
 *   如果 HI_MPI_SYS_Init() 触发了这条路径，那么"先用 devmem 把音量写高、
 *   再跑探针"是无效的 —— SYS_Init 会把它压回去。
 *
 *   所以本程序在 SYS_Init 之后、SendFrame 之前，自己用 /dev/mem 直接写
 *   0x20050074（acodec reg[3]，DAC 音量），把音量顶上去再播。
 *
 * 用法
 *   ./p33                默认音量 0x4F4F2424（左/右各 79 = 最大）
 *   ./p33 0x0C802424     rmm 放音态的值
 *   ./p33 0x06802424     rmm 待机态的值
 *
 * 音源：标准正弦表，1kHz，幅度 12000（约 -8.7dBFS），0.3s 响 / 0.2s 静 × 8。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

#include "hi_type.h"
#include "hi_common.h"
#include "hi_comm_aio.h"
#include "mpi_sys.h"
#include "mpi_ao.h"

#define AO_DEV   0
#define AO_CHN   0
#define PTS      320
#define FRAMES   100

#define DACREG_PHYS  0x20050074UL
#define PAGE         0x1000UL

static short g_buf[PTS];

/* 8kHz 下 1kHz 正弦的 8 点查表 */
static const short sine8[8] = { 0, 8485, 12000, 8485, 0, -8485, -12000, -8485 };

static volatile unsigned int *map_reg(unsigned long phys, int *fdp)
{
    unsigned long base = phys & ~(PAGE - 1);
    unsigned long off  = phys - base;
    void *m;

    *fdp = open("/dev/mem", O_RDWR | O_SYNC);
    if (*fdp < 0) { perror("open /dev/mem"); return NULL; }
    m = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, *fdp, (off_t)base);
    if (m == MAP_FAILED) { perror("mmap"); close(*fdp); *fdp = -1; return NULL; }
    return (volatile unsigned int *)((char *)m + off);
}

int main(int argc, char **argv)
{
    HI_S32         ret;
    AUDIO_FRAME_S  f;
    int i, k, ok = 0, fail = 0, fd = -1;
    unsigned int   vol = 0x4F4F2424u;
    volatile unsigned int *reg;

    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc > 1) vol = (unsigned int)strtoul(argv[1], NULL, 0);

    printf("=== p33_ao_dac begin (vol=0x%08X) ===\n", vol);

    ret = HI_MPI_SYS_Init();
    printf("SYS_Init  =0x%08X\n", (unsigned)ret);

    reg = map_reg(DACREG_PHYS, &fd);
    if (reg) {
        printf("DAC reg before = 0x%08X\n", *reg);
        *reg = vol;
        printf("DAC reg after  = 0x%08X (wrote 0x%08X)\n", *reg, vol);
    } else {
        printf("!! cannot map DAC register\n");
    }

    for (i = 0; i < FRAMES; i++) {
        int ms = i * 40;
        int silent = ((ms % 500) >= 300);
        for (k = 0; k < PTS; k++) {
            if (silent) g_buf[k] = 0;
            else        g_buf[k] = sine8[((i * PTS) + k) & 7];
        }
        memset(&f, 0, sizeof(f));
        f.enBitwidth   = AUDIO_BIT_WIDTH_16;
        f.enSoundmode  = AUDIO_SOUND_MODE_MONO;
        f.pVirAddr[0]  = g_buf;
        f.u32Len       = PTS * 2;
        f.u64TimeStamp = (HI_U64)i * 40;
        f.u32Seq       = (HI_U32)i;
        ret = HI_MPI_AO_SendFrame(AO_DEV, AO_CHN, &f, HI_TRUE);
        if (ret == 0) ok++; else fail++;
    }
    printf("--- send ok=%d fail=%d ---\n", ok, fail);

    if (reg) {
        printf("DAC reg at end = 0x%08X\n", *reg);
        close(fd);
    }
    printf("=== p33_ao_dac end ===\n");
    return 0;
}
