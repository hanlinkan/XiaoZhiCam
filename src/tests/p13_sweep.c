/*
 * p13_sweep.c -- acodec 麦克风通路参数矩阵扫描
 *
 * 目的：用电平/频谱特征判断麦克风到底接在 LINEIN 还是 MICIN，
 *       以及 PGA 增益 / BOOST / ADC 数字音量 的真实作用。
 *
 * 判别原理（无需人配合）：
 *   - 真麦克风（有偏置、有器件噪声）在增益提升时，>350Hz 的宽带噪声底会同步上升；
 *   - 悬空输入只会拾取工频，>350Hz 几乎为空（只剩 ADC 量化噪声，不随模拟增益变化）。
 *
 * 每个配置采集 12 帧（12 x 40ms = 480ms），统计：
 *   tot  : 全带 RMS
 *   hp   : 4 级一阶高通(约350Hz)后的 RMS  —— “宽带噪声底”，工频被压低约 68dB
 *   g50  : 50Hz   Goertzel 幅值（工频，直接反映干扰强度）
 *   g1k  : 1000Hz Goertzel 幅值（参考）
 *
 * 用法: p13_sweep [dump.bin]
 *   若给出路径，则把每个配置的 PCM 依次追加写入（便于离线 FFT）。
 *   每段固定 12 帧 x 320 样本 = 3840 样本，段序号与表格行号一致。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/time.h>

#include "hi_type.h"
#include "hi_common.h"
#include "hi_comm_aio.h"
#include "mpi_sys.h"
#include "mpi_ai.h"
#include "acodec.h"

#define AI_DEV      0
#define AI_CHN      0
#define SYS_ALIGN   64
#define FRAMES      12
#define MAXBUF      4096

static int g_fd_ac = -1;

/* ---------- 定点数学 ---------- */

static unsigned int isqrt64(unsigned long long v)
{
    unsigned long long rem = 0, root = 0;
    int i;
    for (i = 0; i < 32; i++) {
        root <<= 1;
        rem = (rem << 2) | (v >> 62);
        v <<= 2;
        if (root < rem) {
            rem -= root + 1;
            root += 2;
        }
    }
    return (unsigned int)(root >> 1);
}

/* 4 级一阶高通 y[n] = a*(y[n-1] + x[n] - x[n-1])，fc≈350Hz@8k
 * a = 1/(1+2*pi*fc/fs) = 0.7844 -> Q15 25700
 * 对 50Hz 每级衰减约 16.9dB，4 级约 68dB */
#define HP_STAGES   4
static const int HP_A_Q15 = 25700;
static int hp_y[HP_STAGES];
static int hp_x[HP_STAGES];

static void hp_reset(void)
{
    int i;
    for (i = 0; i < HP_STAGES; i++) { hp_y[i] = 0; hp_x[i] = 0; }
}

static int hp_process(int x)
{
    int i, v = x;
    for (i = 0; i < HP_STAGES; i++) {
        int y = (int)(((long long)HP_A_Q15 * (hp_y[i] + v - hp_x[i])) >> 15);
        hp_x[i] = v;
        hp_y[i] = y;
        v = y;
    }
    return v;
}

/* Goertzel 单频点幅值（返回正弦峰值幅度，与输入同样量纲）
 * s[n] = x[n] + C*s[n-1] - s[n-2]，C = 2*cos(2*pi*f/fs) 用 Q14
 * power = s1^2 + s2^2 - (C*s1*s2)>>14
 * 峰值幅度 A = 2*sqrt(power)/N */
static unsigned int goertzel(const short *x, int n, int which, int fs)
{
    /* C_Q14: 50Hz -> 32743, 1000Hz -> 23171 */
    const int C = (which == 0) ? 32743 : 23171;
    long long s1 = 0, s2 = 0;
    long long p;
    int i;

    (void)fs;
    for (i = 0; i < n; i++) {
        long long s0 = (long long)x[i] + (s1 * C >> 14) - s2;
        s2 = s1;
        s1 = s0;
    }
    p = s1 * s1 + s2 * s2 - ((s1 * s2 * C) >> 14);
    if (p < 0) p = 0;
    return (unsigned int)((2ULL * isqrt64((unsigned long long)p)) / (unsigned int)n);
}

/* ---------- acodec 操作 ---------- */

static int ac_u32(unsigned long req, unsigned int v)
{
    return ioctl(g_fd_ac, req, &v);
}

static int ac_vol(unsigned long req, unsigned int vol)
{
    ACODEC_VOL_CTRL c;
    c.vol_ctrl = vol;
    c.vol_ctrl_mute = 0;
    return ioctl(g_fd_ac, req, &c);
}

static void ac_show(const char *tag)
{
    ACODEC_VOL_CTRL cl, cr, dl, dr;
    unsigned int gl = 0xDEADBEEF, gr = 0xDEADBEEF, iv = 0xDEADBEEF, ov = 0xDEADBEEF;
    memset(&cl, 0, sizeof(cl)); memset(&cr, 0, sizeof(cr));
    memset(&dl, 0, sizeof(dl)); memset(&dr, 0, sizeof(dr));
    ioctl(g_fd_ac, ACODEC_GET_GAIN_MICL, &gl);
    ioctl(g_fd_ac, ACODEC_GET_GAIN_MICR, &gr);
    ioctl(g_fd_ac, ACODEC_GET_INPUT_VOL, &iv);
    ioctl(g_fd_ac, ACODEC_GET_OUTPUT_VOL, &ov);
    ioctl(g_fd_ac, ACODEC_GET_ADCL_VOL, &cl);
    ioctl(g_fd_ac, ACODEC_GET_ADCR_VOL, &cr);
    ioctl(g_fd_ac, ACODEC_GET_DACL_VOL, &dl);
    ioctl(g_fd_ac, ACODEC_GET_DACR_VOL, &dr);
    printf("[acodec %s]\n", tag);
    printf("   gainL=%u gainR=%u inVol=%u outVol=%u\n", gl, gr, iv, ov);
    printf("   adcL={%u,%u} adcR={%u,%u} dacL={%u,%u} dacR={%u,%u}\n",
           cl.vol_ctrl, cl.vol_ctrl_mute, cr.vol_ctrl, cr.vol_ctrl_mute,
           dl.vol_ctrl, dl.vol_ctrl_mute, dr.vol_ctrl, dr.vol_ctrl_mute);
}

/* ---------- AI ---------- */

static int ai_fd = -1;

static int ai_open(void)
{
    AIO_ATTR_S attr;
    MPP_SYS_CONF_S sc;

    memset(&sc, 0, sizeof(sc));
    sc.u32AlignWidth = SYS_ALIGN;
    printf("SYS_SetConf ret=%d\n", (int)HI_MPI_SYS_SetConf(&sc));
    printf("SYS_Init ret=%d\n", (int)HI_MPI_SYS_Init());

    memset(&attr, 0, sizeof(attr));
    HI_MPI_AI_GetPubAttr(AI_DEV, &attr);
    printf("AI rate=%d bw=%d sound=%d ch=%u pt=%u frm=%u clk=%u\n",
           (int)attr.enSamplerate, (int)attr.enBitwidth, (int)attr.enSoundmode,
           (unsigned)attr.u32ChnCnt, (unsigned)attr.u32PtNumPerFrm,
           (unsigned)attr.u32FrmNum, (unsigned)attr.u32ClkSel);

    printf("AI_Enable ret=%d\n", (int)HI_MPI_AI_Enable(AI_DEV));
    printf("AI_EnableChn ret=%d\n", (int)HI_MPI_AI_EnableChn(AI_DEV, AI_CHN));
    ai_fd = HI_MPI_AI_GetFd(AI_DEV, AI_CHN);
    printf("AI_GetFd fd=%d\n", ai_fd);
    return ai_fd >= 0 ? 0 : -1;
}

/* 丢弃已经排队的帧，保证测到的是新配置下的数据 */
static void ai_drain(void)
{
    AUDIO_FRAME_S f;
    AEC_FRAME_S ref;
    int guard = 0;
    while (guard++ < 80) {
        memset(&f, 0, sizeof(f));
        memset(&ref, 0, sizeof(ref));
        if (HI_MPI_AI_GetFrame(AI_DEV, AI_CHN, &f, &ref, HI_TRUE) != 0)
            break;
        HI_MPI_AI_ReleaseFrame(AI_DEV, AI_CHN, &f, &ref);
    }
}

typedef struct {
    unsigned int rms_total;
    unsigned int rms_hp;
    unsigned int g50;
    unsigned int g1k;
    int vmin, vmax;
    int frames;
    int samples;
} stat_t;

static int ai_measure(stat_t *st, FILE *dump)
{
    AUDIO_FRAME_S f;
    AEC_FRAME_S ref;
    long long sq = 0, sqhp = 0;
    static short buf[MAXBUF];
    int nb = 0;
    long nsamp = 0;
    int i, k, ok = 0;

    memset(st, 0, sizeof(*st));
    st->vmin = 32767;
    st->vmax = -32768;
    hp_reset();

    for (i = 0; i < FRAMES; i++) {
        fd_set rfds;
        struct timeval tv;
        int r;

        FD_ZERO(&rfds);
        FD_SET(ai_fd, &rfds);
        tv.tv_sec = 0; tv.tv_usec = 500000;
        r = select(ai_fd + 1, &rfds, NULL, NULL, &tv);
        if (r <= 0) continue;

        memset(&f, 0, sizeof(f));
        memset(&ref, 0, sizeof(ref));
        if (HI_MPI_AI_GetFrame(AI_DEV, AI_CHN, &f, &ref, HI_FALSE) != 0)
            continue;
        ok++;
        if (f.pVirAddr[0] && f.u32Len > 0) {
            short *p = (short *)f.pVirAddr[0];
            int n = (int)(f.u32Len / sizeof(short));
            for (k = 0; k < n; k++) {
                long long v = p[k];
                int h = hp_process(p[k]);
                sq += v * v;
                sqhp += (long long)h * h;
                if (p[k] < st->vmin) st->vmin = p[k];
                if (p[k] > st->vmax) st->vmax = p[k];
                if (nb < MAXBUF) buf[nb++] = p[k];
                nsamp++;
            }
            if (dump) {
                fwrite(f.pVirAddr[0], 1, f.u32Len, dump);
                fflush(dump);
            }
        }
        HI_MPI_AI_ReleaseFrame(AI_DEV, AI_CHN, &f, &ref);
    }

    st->frames = ok;
    st->samples = (int)nsamp;
    if (nsamp > 0 && nb > 0) {
        st->rms_total = isqrt64((unsigned long long)(sq / nsamp));
        st->rms_hp = isqrt64((unsigned long long)(sqhp / nsamp));
        st->g50 = goertzel(buf, nb, 0, 8000);
        st->g1k = goertzel(buf, nb, 1, 8000);
    }
    return ok;
}

int main(int argc, char **argv)
{
    static const int mixers[2] = { 0, 1 };
    static const int gains[3] = { 0, 16, 31 };
    static const int boosts[2] = { 0, 1 };
    static const unsigned int vols[3] = { 1, 0x40, 0x7e };
    FILE *dump = NULL;
    int mi, gi, bi, vi;
    int row = 0;

    printf("=== p13 acodec sweep ===\n");
    if (argc > 1) {
        dump = fopen(argv[1], "wb");
        printf("dump -> %s (%s)\n", argv[1], dump ? "ok" : "FAIL");
    }
    if (ai_open() != 0) return 2;

    g_fd_ac = open("/dev/acodec", O_RDWR);
    printf("open /dev/acodec fd=%d\n", g_fd_ac);
    if (g_fd_ac < 0) return 3;

    ac_show("boot-default");

    printf("AI_DisableVqe ret=%d\n", (int)HI_MPI_AI_DisableVqe(AI_DEV, AI_CHN));

    printf("\n%-4s %-6s %-5s %-6s %-5s | %-7s %-7s %-7s %-7s %-15s %s\n",
           "#", "mixer", "gain", "boost", "vol", "rmsTot", "rmsHP", "g50", "g1k",
           "min/max", "frms");
    printf("--------------------------------------------------------------------------------------\n");

    for (mi = 0; mi < 2; mi++) {
        for (gi = 0; gi < 3; gi++) {
            for (bi = 0; bi < 2; bi++) {
                for (vi = 0; vi < 3; vi++) {
                    stat_t st;

                    /* ADC 上电、不静音、关高通关、不软静音 */
                    ac_u32(ACODEC_SET_PD_ADCL, 0);
                    ac_u32(ACODEC_SET_PD_ADCR, 0);
                    ac_u32(ACODEC_SET_MICL_MUTE, 0);
                    ac_u32(ACODEC_SET_MICR_MUTE, 0);
                    ac_u32(ACODEC_SET_ADC_HP_FILTER, 0);
                    ac_u32(ACODEC_SET_MIXER_MIC, (unsigned int)mixers[mi]);
                    ac_u32(ACODEC_SET_GAIN_MICL, (unsigned int)gains[gi]);
                    ac_u32(ACODEC_SET_GAIN_MICR, (unsigned int)gains[gi]);
                    ac_u32(ACODEC_ENABLE_BOOSTL, (unsigned int)boosts[bi]);
                    ac_u32(ACODEC_ENABLE_BOOSTR, (unsigned int)boosts[bi]);
                    ac_vol(ACODEC_SET_ADCL_VOL, vols[vi]);
                    ac_vol(ACODEC_SET_ADCR_VOL, vols[vi]);

                    usleep(150000);
                    ai_drain();
                    ai_measure(&st, dump);

                    row++;
                    printf("%-4d %-6d %-5d %-6d 0x%02x  | %-7u %-7u %-7u %-7u %6d/%-8d %d\n",
                           row, mixers[mi], gains[gi], boosts[bi], vols[vi],
                           st.rms_total, st.rms_hp, st.g50, st.g1k,
                           st.vmin, st.vmax, st.frames);
                    fflush(stdout);
                }
            }
        }
    }

    printf("--------------------------------------------------------------------------------------\n");
    ac_show("after-sweep");

    if (dump) fclose(dump);
    close(g_fd_ac);
    return 0;
}
