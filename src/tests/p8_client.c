/*
 * p8_client.c  --  完整语音链路客户端（第一版）
 *
 * 把已验证的四块拼起来：
 *   [AI 采集] → 上采样 8k→16k → Opus 编码 → WebSocket → 小智服务端
 *   小智服务端 → WebSocket → Opus 解码 24k → 降采样 8k → [AO 播放]
 *
 * 线程模型（单核 240MHz，优先级写死不靠运行时试探）：
 *   th_play  SCHED_FIFO 20   只做「取 PCM → AO SendFrame」
 *   th_cap   SCHED_FIFO 15   只做「AI GetFrame → 上采样 → 写环」
 *   th_net   SCHED_OTHER     TLS 收发 + Opus 编解码（独占 ssl 上下文，不加锁）
 *   main     SCHED_OTHER     状态机 + VAD 判定
 *
 * 设计约束来自实测：
 *   - rmm 独占音频但允许共存，参数锁在 8kHz/mono/320点/40ms/30帧
 *   - 绝不能 kill rmm，否则硬件看门狗复位整机
 *   - 设备空闲内存约 2MB，所有缓冲必须小
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <pthread.h>
#include <sched.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <dirent.h>
#include <mqueue.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>

#include "hi_type.h"
#include "hi_common.h"
#include "hi_comm_aio.h"
#include "hi_comm_adec.h"
#include "mpi_sys.h"
#include "mpi_adec.h"
#include "mpi_ai.h"
#include "acodec.h"

#include "afe.h"
#include "g726.h"
#include "dsfir.h"
#include "led.h"
#include "kws.h"

#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/base64.h"
#include "mbedtls/error.h"
#include "mbedtls/version.h"

#include "opus.h"

/* ===================== 配置 ===================== */

#define HOST        "api.tenclass.net"
#define PORT        "443"
#define WS_PATH     "/xiaozhi/v1/"

/* ---- 设备身份（默认值，启动时会被 load_identity() 覆盖）----
 * Device-Id 必须每台设备唯一：服务端靠它区分设备，硬编码会让所有人的
 * 相机在服务端变成同一台。默认值仅作为兜底，正常路径是读本机 ra0 的 MAC。
 * 需要指定时，在 SD 卡 hack/identity.conf 里写 device_id=/client_id=。 */
#define DEVICE_ID   "00:11:22:33:44:55"
#define CLIENT_ID   "00000000-0000-4000-8000-000000000000"
#define TOKEN       "test-token"

#define AI_DEV      0
#define AI_CHN      0

#define CAP_RATE      8000
#define CAP_FRAME     320                 /* 40ms @8k，rmm 锁定的值 */

/* ---- TTS 播放：rmm 原厂提示音通路（逆向记录第 9 章） ----
 * PCM8k -> G726-16k(2bit MSB) -> 80B/包 -> 段文件 -> symlink -> mq 0x1614
 * 绝不触碰 rmm 的 ADEC/AO/Bind。 */
/* 段长：16 包 = 640ms。
 * 不能取 12（480ms）：实测触发间隔中位 521ms（th_play 的轮询粒度会多攒
 * 20~40ms），段音频只有 480ms，74% 的段会在 ADEC 队列里留下 40ms 空洞，
 * AO 欠载后 DMA 重同步 = 可听的咔哒。段长必须大于实际 gap。
 * 上限受 ADEC BufSize=20 包约束，16 留 4 包余量。 */
#define PB_SEG_PKTS   16
/* 收尾段的总包数：75 包 = 3 秒。
 *
 * 为什么需要这么长：rmm 每播完一个文件都会写 DAC 待机寄存器
 * (0x20050074 <- 0x06802424，见 docs/03)，此刻 AO 里尚未播出的部分会被
 * 静默消耗掉 —— 这就是"最后几个字没了"。
 *
 * 实测标定（2026-11-03）：
 *   补 16 包 → 停留 368ms，仍丢"几个字"
 *   补 50 包 → 停留约 1.66s，只丢"约一个字"（SendFail=0，包没丢）
 * 说明 AO 实际缓冲比 /proc 报告的 30 帧(1.2s)更深，或 rmm 包间隔短于 23ms。
 * 取 3 秒留足余量：dmesg 里那 1.2s 只是设备属性，真实链路还有 DMA 双缓冲。
 *
 * 上限：段文件 75*80 = 6000B，ADEC 队列仍是 20 包，后 55 包走阻塞发送。
 * 阻塞期间 g_pb_busy 一直为真，主线程会等 SendCnt 追平才回 idle，不会截断。 */
#define PB_TAIL_PKTS  75
#define PB_SLOTS      12                    /* 段文件名槽位环形复用 */
#define PB_DIR        "/tmp/pushseg"
#define PB_LINK       "/home/forbidden.g726"
#define PB_MID        PB_DIR "/mid.g726"    /* 第二跳，在 tmpfs 上原子替换 */
#define PB_FIFO       PB_DIR "/stream.g726"

/* ---- FIFO 流式播放（fifo_mode.on 开关）----
 * rmm 的 send_ao_beep 是 fopen + 循环 fread(80B) → ADEC_SendStream，
 * 读到 EOF 才返回（逆向记录已确认没有 fstat/fseek）。
 * 所以把 PB_LINK 指向一个 FIFO，rmm 就会阻塞在读上等我们喂数据：
 *   - DAC 全程保持播放态，不会每次播完就关
 *   - 没有"段"，也就没有段边界
 *   - TTS 结束补静音再 close，AO 的 30 帧缓冲能自然排空
 * 这是为了根治两个顽疾：段边界咔哒、以及播完关 DAC 丢掉 AO 缓冲尾巴。 */
#define PB_FIFO_START 6400                  /* 起播水位（样本，0.8s = 16 包，
                                             * 与段模式一次送的量对齐，ADEC 才不至于
                                             * 因为队列太空而迟迟不出声） */
#define PB_FIFO_PAD   40                    /* 收尾补静音包数（1.6s） */
#define PB_TAIL_MS    1000                  /* 收尾排空余量（ADEC 20 包队列） */
/* 首段起播前的 ring 水位（样本）：19200 = 2.4 秒。
 *
 * 为什么要这么深：段音频时长恒等于"攒够 N 包所需的时间"，端到端余量为 0。
 * ADEC 队列只有 20 包（800ms），一段 640ms 送完后队列剩 272ms，
 * 而下次触发要 640ms 后才来 —— 队列必然在下一次触发时恰好排空，
 * gap 只要多 1ms 就欠载（实测 gap 中位 641ms，74% 的段在欠载），
 * AO 欠载后 DMA 重同步就是咔哒声。
 *
 * 攒 2.4 秒储备后，攒包不再需要等数据到达（ring 里已有），
 * gap 能稳定落在段长之内，队列因此始终留有余量。
 * 代价是首字延迟从 1.2s 增到 2.4s；短回答不受影响 —— tts_ended 一到
 * 就立即起播，不等水位。 */
#define PB_STARTUP_RING 19200

#define NET_RATE      16000
#define NET_FRAME     960                 /* 60ms @16k，协议帧长 */
#define NET_FRAME_MS  60

#define UP_RING_SEC   3                   /* 上行环形缓冲 3 秒 */
#define DOWN_RING_SEC 4                   /* 下行环形缓冲 4 秒 */

#define VAD_SILENCE_FRMS  18              /* 连续 18 帧静音（720ms）算说完 */
#define LISTEN_MAX_MS     15000           /* 单轮最长录音 15 秒 */

/* ---- acodec 实测最优配置（2026-10-29 声学回环验证） ----
 * rmm 的 Rock_Acodec_CfgAudio 只设 SOFT_RESET / I2S1_FS / ADCL_VOL / ADCR_VOL 四项，
 * 从不设 MIXER / GAIN_MIC / BOOST，所以一路沿用驱动默认值。
 * 本文件显式设 MIXER = LINEIN，并按实测拉满增益：
 *   出厂 MIC 增益 31 时直流 31816、正向余量只剩 951；
 *   降到 20 后直流 10600，原始信号 RMS≈3965、max 6728。
 * 保留 787 左右的直流偏置，由 afe 滤除；强 50 Hz 工频（奇次谐波延伸到 1.3 kHz）
 * 由 afe 的同步平均去除，见 afe.h。 */
#define ACODEC_MIXER_LINEIN  0            /* ACODEC_MIXER_E：LINEIN=0，MICIN=1 */
#define ACODEC_GAIN_MIC      20           /* 出厂 31 时直流 31816、余量只剩 951；20 档直流 10600 */
#define ACODEC_BOOST         0            /* 出厂关，保持不动 */
#define ACODEC_ADC_VOL       24           /* 保持出厂值；越小越响，1 会削顶 */

#define OPUS_BITRATE      16000
#define OPUS_COMPLEXITY   1

/* ===================== 日志 ===================== */

static FILE *g_log;
static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static long long now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void logf_(const char *fmt, ...)
{
    va_list ap;
    pthread_mutex_lock(&g_log_lock);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
    if (g_log) {
        va_start(ap, fmt);
        vfprintf(g_log, fmt, ap);
        va_end(ap);
        fprintf(g_log, "\n");
        fflush(g_log);
    }
    pthread_mutex_unlock(&g_log_lock);
}

/* ===================== 设备身份 ===================== */

static char g_device_id[20] = DEVICE_ID;
static char g_client_id[40] = CLIENT_ID;

/* 由 Device-Id 派生一个稳定的 Client-Id（FNV-1a，同输入恒定）。
 * 不需要真的 UUID：服务端只要求同设备稳定、不同设备不同。 */
static void derive_client_id(const char *seed, char *out, int cap)
{
    unsigned int h1 = 2166136261u, h2 = 2166136261u;
    const char *p;

    for (p = seed; *p; p++) {
        h1 ^= (unsigned char)*p; h1 *= 16777619u;
    }
    for (p = seed; *p; p++) {
        h2 ^= (unsigned char)*p; h2 *= 16777619u; h2 ^= (h1 >> 7);
    }
    snprintf(out, cap, "%08x-%04x-4%03x-8%03x-%08x%04x",
             h1, (h2 >> 16) & 0xffffu, h2 & 0xfffu,
             (h1 >> 18) & 0xfffu, h2, h1 & 0xffffu);
}

static int read_first_line(const char *path, char *out, int cap)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    if (!fgets(out, cap, f)) { fclose(f); return -1; }
    fclose(f);
    out[strcspn(out, "\r\n")] = '\0';
    return out[0] ? 0 : -1;
}

/*
 * 确定本机的 Device-Id / Client-Id。优先级：
 *   1. SD 卡 hack/identity.conf（device_id= / client_id=，可只写一项）
 *   2. 本机网卡 MAC（ra0，无线；其次 eth0）
 *   3. 编译期默认值（仅在以上都拿不到时兜底）
 *
 * 刻意保留一处兼容：若最终 Device-Id 与编译默认值相同（即这就是作者那台机器），
 * 且配置未显式给 client_id，则沿用原 Client-Id，避免服务端把它当成新客户端。
 */
static void load_identity(void)
{
    static const char *conf_paths[] = {
        "/tmp/hd1/hack/identity.conf",
        "/tmp/hd1/identity.conf",
        NULL
    };
    static const char *mac_paths[] = {
        "/sys/class/net/ra0/address",
        "/sys/class/net/eth0/address",
        NULL
    };
    char line[128];
    int i, dev_set = 0, cli_set = 0;

    for (i = 0; conf_paths[i]; i++) {
        FILE *f = fopen(conf_paths[i], "r");
        if (!f) continue;
        while (fgets(line, sizeof line, f)) {
            char *v;
            line[strcspn(line, "\r\n")] = '\0';
            if (line[0] == '#' || line[0] == '\0') continue;
            if (!strncmp(line, "device_id=", 10)) {
                v = line + 10;
                if (strlen(v) >= 11) {
                    snprintf(g_device_id, sizeof g_device_id, "%s", v);
                    dev_set = 1;
                }
            } else if (!strncmp(line, "client_id=", 10)) {
                v = line + 10;
                if (strlen(v) >= 32) {
                    snprintf(g_client_id, sizeof g_client_id, "%s", v);
                    cli_set = 1;
                }
            }
        }
        fclose(f);
        break;
    }

    if (!dev_set) {
        for (i = 0; mac_paths[i]; i++) {
            if (read_first_line(mac_paths[i], line, sizeof line) == 0 &&
                strlen(line) >= 11) {
                snprintf(g_device_id, sizeof g_device_id, "%s", line);
                dev_set = 1;
                break;
            }
        }
    }

    if (!cli_set && strcmp(g_device_id, DEVICE_ID) != 0) {
        derive_client_id(g_device_id, g_client_id, sizeof g_client_id);
    }

    logf_("[id] device=%s client=%s (%s)", g_device_id, g_client_id,
          dev_set ? "本机" : "默认值");
}

/* ===================== 环形缓冲（单生产/单消费） ===================== */

typedef struct {
    short          *data;
    unsigned int    mask;     /* cap-1，cap 必须 2 的幂 */
    volatile unsigned int wr;
    volatile unsigned int rd;
} ring_t;

static int ring_init(ring_t *r, unsigned int cap_pow2)
{
    r->data = (short *)malloc(cap_pow2 * sizeof(short));
    if (!r->data) return -1;
    r->mask = cap_pow2 - 1;
    r->wr = r->rd = 0;
    return 0;
}

static inline unsigned int ring_avail(ring_t *r) { return r->wr - r->rd; }
static inline unsigned int ring_space(ring_t *r) { return (r->mask + 1) - (r->wr - r->rd); }

static unsigned int ring_write(ring_t *r, const short *src, unsigned int n)
{
    unsigned int sp = ring_space(r);
    if (n > sp) n = sp;
    for (unsigned int i = 0; i < n; i++)
        r->data[(r->wr + i) & r->mask] = src[i];
    r->wr += n;
    return n;
}

static unsigned int ring_read(ring_t *r, short *dst, unsigned int n)
{
    unsigned int av = ring_avail(r);
    if (n > av) n = av;
    for (unsigned int i = 0; i < n; i++)
        dst[i] = r->data[(r->rd + i) & r->mask];
    r->rd += n;
    return n;
}

/* ===================== 全局状态 ===================== */

static volatile int g_running = 1;

/* 音频 */
static int g_ai_enabled = 0;
static ring_t g_up_ring;        /* 16k mono，采集线程写，网络线程读 */
static ring_t g_down_ring;      /* 8k mono，网络线程写，播放线程读 */

/* VAD（采集线程算，主线程读） */
static volatile int g_vad_speech_frms = 0;   /* 连续有声帧数 */
static volatile int g_vad_silence_frms = 0;  /* 连续静音帧数 */
static volatile int g_vad_has_spoken = 0;    /* 本轮是否已检测到人声 */
static volatile int g_vad_rms = 0;
static afe_t g_afe;                          /* 音频前端状态，仅采集线程访问 */
static FILE *g_raw;
static int   g_raw_frames = 0;
static int   g_raw_max = 0;        /* 0 = 不落盘；只有 CAP_RAW+CAP_SECS 同时给才录 */

/* 唤醒词检测（P2：只读接入——不改会话状态机，只记录命中与耗时）
 *
 * 为什么先只读：先把"真机 CPU 开销"和"真机误触发"量出来，再决定阈值与
 * 状态机怎么改。docs/04 §5.6 的方案依赖这两个数字，离线测不出来。 */
static volatile int g_kws_on = 1;
static volatile int g_kws_frames = 0;      /* 已处理帧数 */
static volatile int g_kws_hits = 0;        /* 命中累计 */
static volatile long long g_kws_us = 0;    /* 累计耗时 */
/* 主状态机状态（供采集线程判断是否停喂 kws——播放期自闻误报，见 docs/09 修正） */
enum { ST_CONNECTING, ST_IDLE, ST_LISTEN, ST_WAIT_TTS, ST_SPEAK };

static volatile int g_sm_state = -1;       /* 命中时主状态机处在哪个状态 */
static int g_kws_muted;                    /* 1 = 正处于停喂期（SPEAK/WAIT_TTS） */
static volatile int g_wake_hit;            /* 唤醒门控：IDLE 态本地命中置 1，主循环消费 */
static long long g_boot_ms = 0;

/* 会话 */
static volatile int g_tx_enable = 0;         /* 是否推流 */
static volatile long long g_tx_start_ms = 0;
static volatile int g_playing = 0;           /* TTS 会话进行中 */
static volatile int g_tts_session = 0;       /* TTS 会话号：start 时 +1 */
static volatile int g_tts_ended = 0;         /* 服务端 tts stop 已收到 */
static volatile int g_pb_busy = 0;           /* 播放线程有未触发/待触发的段 */
static volatile int g_pb_abort = 0;          /* 服务端主动 abort 时跳过播完 tail */
static volatile int g_pb_fd = -1;            /* FIFO 写端；打断时由主线程 close 解除阻塞 */
static volatile long long g_pb_last_trig_ms = 0;

/* ===================== 音频设备 ===================== */

/* ===================== acodec 初始化 ===================== */

static int g_acodec_fd = -1;

static void ac_u32(unsigned long req, unsigned int v)
{
    if (g_acodec_fd >= 0) ioctl(g_acodec_fd, req, &v);
}

static void ac_vol(unsigned long req, unsigned int v)
{
    ACODEC_VOL_CTRL c;
    c.vol_ctrl = v;
    c.vol_ctrl_mute = 0;
    if (g_acodec_fd >= 0) ioctl(g_acodec_fd, req, &c);
}

/*
 * 按实测最优配置初始化 acodec：显式设 MIXER，并把四项增益拉满。
 *
 * 背景：反汇编 rmm 的 Rock_Acodec_CfgAudio 后确认它只做四件事：
 *   ACODEC_SOFT_RESET_CTRL -> SET_I2S1_FS -> SET_ADCL_VOL{1,0} -> SET_ADCR_VOL{1,0}
 * 从不设 MIXER_MIC / GAIN_MIC / BOOST，所以一路沿用驱动默认值。
 * 本函数把这些补上，具体数值见上方 ACODEC_* 宏。
 *
 * 注意：ADCL/ADCR_VOL 是「衰减」寄存器，数值越大越安静，
 *       0x40 及以上实测直接静音；合法范围 0..62，1 约为最大音量。
 */
static int acodec_open(void)
{
    unsigned int gl = 0, gr = 0, iv = 0;
    ACODEC_VOL_CTRL cl, cr;

    g_acodec_fd = open("/dev/acodec", O_RDWR);
    if (g_acodec_fd < 0) {
        logf_("[ERR] open /dev/acodec failed errno=%d", errno);
        return -1;
    }

    memset(&cl, 0, sizeof(cl));
    memset(&cr, 0, sizeof(cr));
    ioctl(g_acodec_fd, ACODEC_GET_GAIN_MICL, &gl);
    ioctl(g_acodec_fd, ACODEC_GET_GAIN_MICR, &gr);
    ioctl(g_acodec_fd, ACODEC_GET_INPUT_VOL, &iv);
    ioctl(g_acodec_fd, ACODEC_GET_ADCL_VOL, &cl);
    ioctl(g_acodec_fd, ACODEC_GET_ADCR_VOL, &cr);
    logf_("[acodec] before: gainL=%u gainR=%u inVol=%u adcL={%u,%u} adcR={%u,%u}",
          gl, gr, iv, cl.vol_ctrl, cl.vol_ctrl_mute, cr.vol_ctrl, cr.vol_ctrl_mute);

    /* ADC 上电 + 解除静音 + 关掉 codec 内部高通（我们自己在 afe 里做得更彻底） */
    ac_u32(ACODEC_SET_PD_ADCL, 0);
    ac_u32(ACODEC_SET_PD_ADCR, 0);
    ac_u32(ACODEC_SET_MICL_MUTE, 0);
    ac_u32(ACODEC_SET_MICR_MUTE, 0);
    ac_u32(ACODEC_SET_ADC_HP_FILTER, 0);

    ac_u32(ACODEC_SET_MIXER_MIC, ACODEC_MIXER_LINEIN);
    ac_u32(ACODEC_SET_GAIN_MICL, ACODEC_GAIN_MIC);
    ac_u32(ACODEC_SET_GAIN_MICR, ACODEC_GAIN_MIC);
    ac_u32(ACODEC_ENABLE_BOOSTL, ACODEC_BOOST);
    ac_u32(ACODEC_ENABLE_BOOSTR, ACODEC_BOOST);
    ac_vol(ACODEC_SET_ADCL_VOL, ACODEC_ADC_VOL);
    ac_vol(ACODEC_SET_ADCR_VOL, ACODEC_ADC_VOL);

    /* 回读确认 */
    gl = gr = 0;
    memset(&cl, 0, sizeof(cl));
    memset(&cr, 0, sizeof(cr));
    ioctl(g_acodec_fd, ACODEC_GET_GAIN_MICL, &gl);
    ioctl(g_acodec_fd, ACODEC_GET_GAIN_MICR, &gr);
    ioctl(g_acodec_fd, ACODEC_GET_ADCL_VOL, &cl);
    ioctl(g_acodec_fd, ACODEC_GET_ADCR_VOL, &cr);
    logf_("[acodec] after : LINEIN gain=%u/%u adcL={%u,%u} adcR={%u,%u} boost=%d",
          gl, gr, cl.vol_ctrl, cl.vol_ctrl_mute, cr.vol_ctrl, cr.vol_ctrl_mute,
          ACODEC_BOOST);

    if (gl != ACODEC_GAIN_MIC || cl.vol_ctrl != ACODEC_ADC_VOL) {
        logf_("[warn] acodec 回读与写入不一致，可能被 rmm 覆盖");
        return -2;
    }
    return 0;
}

static void boost_rmm_adec(void);
static int  pb_adec_init(void);
static int  pb_trigger_via_rmm(void);
static int audio_open(void)
{
    HI_S32 ret;
    AIO_ATTR_S attr;
    /* rmm 已把参数配好（8k/mono/320点/30帧），这里只读取确认，
       绝不 SetPubAttr —— 会被拒绝且可能污染驱动状态 */
    memset(&attr, 0, sizeof(attr));
    ret = HI_MPI_AI_GetPubAttr(AI_DEV, &attr);
    if (ret != 0) {
        logf_("[ERR] AI_GetPubAttr ret=0x%08X", (unsigned)ret);
        return -1;
    }
    logf_("[AI] rate=%d bw=%d sound=%d ch=%d pt=%d frm=%d ex=%u clk=%u",
          (int)attr.enSamplerate, (int)attr.enBitwidth, (int)attr.enSoundmode,
          (int)attr.u32ChnCnt, (int)attr.u32PtNumPerFrm,
          (int)attr.u32FrmNum, (unsigned)attr.u32EXFlag, (unsigned)attr.u32ClkSel);

    ret = HI_MPI_AI_Enable(AI_DEV);
    if (ret != 0) { logf_("[ERR] AI_Enable 0x%08X", (unsigned)ret); return -2; }
    g_ai_enabled = 1;

    ret = HI_MPI_AI_EnableChn(AI_DEV, AI_CHN);
    if (ret != 0) { logf_("[ERR] AI_EnableChn 0x%08X", (unsigned)ret); return -3; }

    /* VQE AEC 实验已撤（2026-10-31 用户指令"取消打断"）：
       配置全绿（/proc/umap/ai bAec=enable AecAo=(0,0)）但实测未消回声，
       逆向记录 7.11/7.12。回声问题的根治待续（参考链路诊断未完成）。 */

    /* 把 codec 切到 MICIN 并拉满增益。
       实测：rmm 的 AI_SetHpfAttr 返回 0 但 /proc/umap/ai 仍显示 bHpf disable，
       且频谱无变化 —— codec 内部高通不可靠，改用 afe.h 的数字高通 + 隔直。 */
    if (acodec_open() != 0)
        logf_("[warn] acodec 配置失败，采集电平可能极低");

    /* AO 不碰：播放走 rmm 的 beep 通路（逆向记录第 9 章），
       AO 的 Enable/SetPubAttr/SendFrame 一律不用 —— 那是无声死路且会
       破坏 rmm 的通道状态（第 4/7 章结论）。 */

    /* CPLD 音频通路总开关（cpld_periph 的第一对 GPIO）。
     *
     * 逆向 rmm main(0x39e2c)：CLI 放音模式会在放音后 ioctl(0x7021) 把它关掉；
     * 而 daemon 模式启动时也会先 ioctl(0x7021) 回到待机。
     *
     * 实测（2026-11-07）：0x7021 关掉的不只是喇叭功放，**麦克风偏置供电也一起没了**
     * —— KWS 立刻变聋（日志里只剩 saturating 的开机误触发，之后 0 hit），
     * 而且 CPLD 状态能扛过软重启，reboot 也救不回来，只有 0x7020 能恢复。
     *
     * 所以这里在启动时无条件拉高一次，保证麦克风与喇叭都上电。
     * 约定：本程序此后**绝不再调 0x7021**，宁可让功放常开。 */
    {
        int cfd = open("/dev/cpld_periph", O_RDWR);
        if (cfd >= 0) {
            int r = ioctl(cfd, 0x7020, 0);
            logf_("[cpld] audio power on (0x7020) ret=%d", r);
            close(cfd);
        } else {
            logf_("[warn] open /dev/cpld_periph failed errno=%d", errno);
        }
    }

    /* 自己接管播放的初始化默认关闭。
     *
     * 原因（2026-11-07 实测）：ADEC_CreateChn / SYS_Bind 都要求先调
     * HI_MPI_SYS_Init()，而在本进程里调 SYS_Init 会打乱 rmm 已经建好的
     * MPP/AI 状态 —— 麦克风电平不变但 KWS 再不出命中，等于把语音弄哑。
     *
     * 所以默认走原来的 rmm 播放（功能完好）；直连播放的完整配方与代码保留在
     * pb_adec_init / pb_direct_thread / pb_trigger 里，等找到不破坏 AI 的
     * 初始化方式（例如 fork 出独立进程去做 ADEC）再启用。 */
    if (getenv("PB_DIRECT")) pb_adec_init();

    logf_("[OK] audio devices ready (capture only, playback via rmm)");

    /* 播放无断音的前提：rmm 的发包线程不被任何实时线程饿死。
       把它提到 FIFO 10（高于本进程全部线程），幂等。 */
    boost_rmm_adec();
    return 0;
}

static void audio_close(void)
{
    if (g_acodec_fd >= 0) {
        close(g_acodec_fd);
        g_acodec_fd = -1;
    }
    if (g_ai_enabled) {
        HI_MPI_AI_DisableChn(AI_DEV, AI_CHN);
        HI_MPI_AI_Disable(AI_DEV);
        g_ai_enabled = 0;
    }
}

/* ===================== 采集线程 ===================== */

/* 实时优先级铁律（launch.sh 旧固件实测 + 2026-10-31 结论）：
 * rmm 的 adec_sendao 线程（喂 G.726 的循环，每 23ms 一拍）如果被
 * SCHED_FIFO 高优先级线程抢占超过 ~17ms，ADEC 缓冲抽干 = 30ms 断音。
 * 因此：p8s 的实时线程一律低于 rmm（rmm 侧由启动脚本提 FIFO 10）：
 *   th_play = FIFO 4（编码/落盘，忙时可被 rmm 抢）
 *   th_cap  = FIFO 3（采集，40ms 一拍，余量大）
 * 旧值 15/20 高于 rmm，是"偶尔卡顿"的来源之一。 */
static void set_realtime(int prio)
{
    struct sched_param sp;
    sp.sched_priority = prio;
    if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0)
        logf_("[warn] sched_setscheduler(%d) failed errno=%d", prio, errno);
}

/* rmm 的 adec_sendao 提到 SCHED_FIFO 10（幂等，p8s 每次启动做一次）。
 * 依据：launch.sh 2026-10-23 声学实测——rmm=无缝、被固件 FIFO 抢占=30ms
 * 深凹陷；rmm FIFO 10 + 固件 FIFO 4 组合 0 凹陷。 */
static void boost_rmm_adec(void)
{
    char path[64], comm[64];
    DIR *dir;
    struct dirent *e;
    FILE *f;
    int rpid = -1, n;
    struct sched_param sp;

    /* pidof 无 libc 直连，扫 /proc 找 cmdline 以 "rmm" 开头的进程 */
    dir = opendir("/proc");
    if (!dir) return;
    while ((e = readdir(dir)) != NULL) {
        char cl[256];
        long pid = atol(e->d_name);
        if (pid <= 0) continue;
        snprintf(path, sizeof(path), "/proc/%ld/cmdline", pid);
        f = fopen(path, "r");
        if (!f) continue;
        n = (int)fread(cl, 1, sizeof(cl) - 1, f);
        fclose(f);
        if (n <= 0) continue;
        cl[n] = 0;
        if (strstr(cl, "rmm") != NULL) { rpid = (int)pid; break; }  /* cmdline 是 ./rmm */
    }
    closedir(dir);
    if (rpid < 0) { logf_("[warn] rmm not found, skip adec boost"); return; }

    snprintf(path, sizeof(path), "/proc/%d/task", rpid);
    dir = opendir(path);
    if (!dir) return;
    sp.sched_priority = 10;
    while ((e = readdir(dir)) != NULL) {
        int tid = atoi(e->d_name);
        if (tid <= 0) continue;
        snprintf(comm, sizeof(comm), "/proc/%d/task/%d/comm", rpid, tid);
        f = fopen(comm, "r");
        if (!f) continue;
        n = (int)fread(comm, 1, sizeof(comm) - 1, f);
        fclose(f);
        if (n <= 0) continue;
        comm[n] = 0;
        if (strncmp(comm, "adec_sendao", 11) == 0) {
            if (sched_setscheduler(tid, SCHED_FIFO, &sp) == 0)
                logf_("[ok] rmm adec_sendao tid=%d -> SCHED_FIFO 10", tid);
            else
                logf_("[warn] chrt adec_sendao tid=%d failed errno=%d", tid, errno);
        }
    }
    closedir(dir);
}

static void log_hex(const char *tag, const unsigned char *p, int n)
{
    char line[160];
    int i, off = 0;
    if (!p || n <= 0) { logf_("%s (null)", tag); return; }
    if (n > 32) n = 32;
    for (i = 0; i < n && off < (int)sizeof(line) - 4; i++)
        off += snprintf(line + off, sizeof(line) - off, "%02x", p[i]);
    logf_("%s %s", tag, line);
}

static void *th_cap(void *arg)
{
    AUDIO_FRAME_S frame;
    AEC_FRAME_S   ref;
    short up[CAP_FRAME * 2 + 8];
    short pcm_out[CAP_FRAME];
    int   fd = HI_MPI_AI_GetFd(AI_DEV, AI_CHN);
    int   dbg_tick = 0;
    int   frame_dbg = 0;

    (void)arg;
    set_realtime(3);    /* 低于 rmm adec_sendao(FIFO 10)：见 set_realtime 处说明 */
    afe_reset(&g_afe);
    logf_("[cap] started, ai_fd=%d", fd);

    while (g_running) {
        fd_set rfds;
        struct timeval tv;
        int ret;

        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        tv.tv_sec = 0; tv.tv_usec = 200000;

        ret = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (ret <= 0) continue;

        memset(&frame, 0, sizeof(frame));
        memset(&ref, 0, sizeof(ref));
        ret = HI_MPI_AI_GetFrame(AI_DEV, AI_CHN, &frame, &ref, HI_FALSE);
        if (ret != 0) continue;

        if (frame_dbg < 3 && g_raw_max > 0) {
            frame_dbg++;
            logf_("[frm] bw=%d sound=%d len=%u seq=%u p0=%p p1=%p ts=%llu",
                  (int)frame.enBitwidth, (int)frame.enSoundmode,
                  (unsigned)frame.u32Len, (unsigned)frame.u32Seq,
                  frame.pVirAddr[0], frame.pVirAddr[1],
                  (unsigned long long)frame.u64TimeStamp);
            log_hex("[raw0]", (const unsigned char *)frame.pVirAddr[0], 32);
            log_hex("[raw1]", (const unsigned char *)frame.pVirAddr[1], 32);
        }

        if (frame.pVirAddr[0] && frame.u32Len > 0) {
            short *pcm = (short *)frame.pVirAddr[0];
            int    cnt = (int)(frame.u32Len / sizeof(short));
            int    n = 0, i;
            int    voiced;

            if (g_raw && g_raw_frames < g_raw_max) {
                fwrite(frame.pVirAddr[0], 1, frame.u32Len, g_raw);
                if ((g_raw_frames & 0x3F) == 0) fflush(g_raw);   /* 每 64 帧刷一次，别拖慢采集 */
                g_raw_frames++;
                if (g_raw_frames == g_raw_max) {
                    fflush(g_raw);
                    logf_("[cap] 原始落盘完成：%d 帧 %.1f s", g_raw_frames,
                          (double)g_raw_frames * 320.0 / 8000.0);
                }
            }

            if (cnt > CAP_FRAME) cnt = CAP_FRAME;
            if (cnt <= 0) { HI_MPI_AI_ReleaseFrame(AI_DEV, AI_CHN, &frame, &ref); continue; }

            /* 音频前端：同步平均去工频 -> 6阶高通200Hz -> 6阶低通3kHz -> 噪声门 -> AGC
               输入是 LINEIN 原始 PCM（含 50Hz 周期尖峰串，RMS≈3965），
               输出是可直接编码的语音（实测静音残余≈64，打铃 SNR +19~30dB） */
            voiced = afe_process(&g_afe, pcm, pcm_out, cnt);

            /* 唤醒词检测：取 afe 的 taps（AGC / 噪声门之前）。
             * 为什么必须是 AGC 之前：训练用的就是这个抽头，取 AGC 之后等于把
             * 包络交给一个会随底噪泵动的自动增益，特征域就变了（docs/08）。 */
            {
                static int dbg_once = 0;
                if (!dbg_once && cnt >= 8) {
                    dbg_once = 1;
                    logf_("[kws] dbg cnt=%d pcm=%d,%d,%d,%d tmp=%d,%d,%d,%d",
                          cnt, pcm[0], pcm[1], pcm[2], pcm[3],
                          g_afe.tmp[0], g_afe.tmp[1], g_afe.tmp[2], g_afe.tmp[3]);
                }
            }
            if (g_kws_on) {
                int sm = g_sm_state;
                /* 播放/等 TTS 期间停喂：抽头无 AEC，喇叭漏进麦克风（实测一段回应
                 * 7 连中，24/27 命中发生在 SPEAK 态——docs/09 修正节）。
                 * 静音/恢复边界各 reset 一次前端，避免断续拼接被当成词。
                 * 状态位经 g_sm_state 传播有一帧（40ms）盲区，可接受。 */
                if (sm == ST_SPEAK || sm == ST_WAIT_TTS) {
                    if (!g_kws_muted) {
                        kws_reset();
                        g_kws_muted = 1;
                        logf_("[kws] mute on (state=%d)", sm);
                    }
                } else {
                    long long t0, dt;
                    int hit;
                    if (g_kws_muted) {
                        kws_reset();
                        g_kws_muted = 0;
                        logf_("[kws] mute off (state=%d)", sm);
                    }
                    t0 = now_us();
                    hit = kws_feed8k((const int16_t *)g_afe.tmp, cnt);
                    dt = now_us() - t0;

                    g_kws_us += dt;
                    g_kws_frames++;
                    if (hit) {
                        kws_diag_t d;
                        kws_diag(&d);
                        g_kws_hits++;
                        logf_("[kws] HIT #%d t=%llds state=%d prob=%d peak=%d rel=%.1f floor=%.1f",
                              g_kws_hits, (now_ms() - g_boot_ms) / 1000, g_sm_state,
                              d.prob, d.prob_peak,
                              d.rel_db10 / 10.0, d.floor_db10 / 10.0);
                        kws_arm(0);      /* 一次发声只报一次；不动前端（见 kws.c） */
                        /* 唤醒门控：IDLE（或正在重连）态、且本进程已过热机期才放行。
                         * 实测误报全部集中在进程启动后前 4 分钟（afe 电平归一化 /
                         * 底噪估计未收敛，docs/09 八与补记七），3 分钟后零误报。
                         * CONNECTING 也算：60 s 空闲断连的重连窗口里喊唤醒词，
                         * 命中要能留住，等 session 起来立刻开监听。 */
                        if ((g_sm_state == ST_IDLE || g_sm_state == ST_CONNECTING) &&
                            (now_ms() - g_boot_ms) > 180000) {
                            g_wake_hit = 1;
                        }
                    }
                }
            }

            /* 上采样 2x（8k→16k）：线性插值，零成本且自带平滑 */
            for (i = 0; i < cnt; i++) {
                short a = pcm_out[i];
                short b = (i + 1 < cnt) ? pcm_out[i + 1] : pcm_out[i];
                up[n++] = a;
                up[n++] = (short)(((int)a + (int)b) >> 1);
            }
            /* 半双工：仅 listen 态上行（打断实验已按用户指令移除，
             * 服务端不 VAD + 回声问题，见逆向记录 7.9-7.12） */
            if (g_tx_enable) ring_write(&g_up_ring, up, (unsigned)n);

            /* VAD 状态由 afe 统一维护，这里只同步给主状态机 */
            g_vad_rms = g_afe.last_out_rms;
            if (voiced) {
                if (g_vad_speech_frms < 1000) g_vad_speech_frms++;
                g_vad_silence_frms = 0;
                if (g_vad_speech_frms >= AFE_SPEECH_FRMS) g_vad_has_spoken = 1;
            } else {
                if (g_vad_silence_frms < 1000) g_vad_silence_frms++;
                g_vad_speech_frms = 0;
            }

            /* 每帧都记日志太吵：每 25 帧（1 秒）打一次 */
            if ((++dbg_tick % 25) == 0) {
                if (g_tx_enable)
                    logf_("[afe] raw=%d peak=%d agc=%d.%02dx gate=%d.%02dx out=%d nf=%d sp=%d",
                          g_afe.last_raw_rms, g_afe.last_peak,
                          g_afe.agc_q10 >> 10, (g_afe.agc_q10 & 1023) * 100 / 1024,
                          g_afe.gate_q10 >> 10, (g_afe.gate_q10 & 1023) * 100 / 1024,
                          g_afe.last_out_rms, g_afe.noise_rms, g_afe.in_speech);
                if (g_kws_on && g_kws_frames > 0) {
                    kws_diag_t d;
                    int nf = g_kws_frames;
                    double per = (double)g_kws_us / (double)nf;
                    kws_diag(&d);
                    /* 40 ms 一帧 → 每帧耗时 / 40000 us 就是单核占用比 */
                    logf_("[kws] %d 帧 %.0f us/帧 = %.1f%% 单核 in=%d mel=%d 峰值=%d hot=%d/%d rel=%.1f floor=%.1f tr=%d 命中=%d",
                          nf, per, per / 400.0, d.in_rms, d.mel_frames,
                          d.prob_peak, d.hot, d.hot_gated,
                          d.rel_db10 / 10.0, d.floor_db10 / 10.0,
                          d.transients, g_kws_hits);
                    g_kws_frames = 0;
                    g_kws_us = 0;
                }
            }
        }
        HI_MPI_AI_ReleaseFrame(AI_DEV, AI_CHN, &frame, &ref);
    }
    logf_("[cap] exit");
    return NULL;
}

/* ===================== 播放线程（rmm beep 通路） ===================== */

static G726Ctx g_pb_ctx;        /* G726-16k 编码器状态，仅播放线程访问 */
static FILE *g_down_raw;        /* 诊断：下行 PCM 落盘（两种播放模式共用） */

/* 读 /proc/umap/adec 的 SendCnt（解析 PlType=g726 那行）；失败返回 -1 */
static int adec_sendcnt(void)
{
    FILE *f = fopen("/proc/umap/adec", "r");
    char line[256];
    int v = -1;

    if (!f) return -1;
    while (fgets(line, sizeof(line), f)) {
        char id[16], pt[16], bs[16];
        int send;
        if (sscanf(line, "%15s %15s %15s %d", id, pt, bs, &send) == 4 &&
            !strcmp(pt, "g726")) {
            v = send;
            break;
        }
    }
    fclose(f);
    return v;
}

/* ================= 自己接管播放（不再经 rmm） =================
 *
 * 逆向 rmm 得到的完整配方（send_ao_beep @0x2ca1c + main @0x39e2c，2026-11-07 实测）：
 *
 *   open("/dev/cpld_periph"); ioctl(0x7020, 0)      // 音频电源（含麦克风偏置）
 *   ADEC_CreateChn(0, {PT_G726, u32BufSize=20, ADEC_MODE_PACK, {enG726bps=4}})
 *   SYS_Bind(ADEC0 -> AO0)                          // 解码结果直送 AO
 *   每包: 4 字节头 {0,1,0x28,0} + 80 字节 G.726, u32Len=84, 间隔 23ms
 *
 * 头部 byte[2]=0x28 是关键：DecodeG726Frm 用 total=(byte[2]+2)*2=84 当消费长度，
 * 送裸 G.726 会被算错长度，返回 0xA0188040（那是"解码回调返回非零"的通用包装码）。
 *
 * 为什么用线程：保持和以前 mq_send 一样"立即返回"的语义。主循环靠
 * /proc/umap/adec 的 SendCnt 追平 expected 来判断播放完成 —— 我们自己发的包
 * 每包让 SendCnt +1，所以那套进度逻辑原样成立（以前 rmm 发时它恒为 0，
 * 每次播放都要等 10 秒超时才放行）。
 */

#ifndef PB_LINK
#define PB_LINK "/home/forbidden.g726"
#endif

#define PB_DIRECT_PKT    80                 /* 每包 G.726 载荷字节数，与 rmm 一致 */
#define PB_DIRECT_MAX    (512 * 1024)

static int              g_adec_ok;          /* ADEC 通道与绑定是否就绪 */
static volatile int     g_pb_direct_busy;   /* 上一段是否还在播 */

struct pb_job {
    unsigned char *data;
    int            len;
};

/* 回退路径：原来的 mq 0x1614，让 rmm 播 PB_LINK。ADEC 初始化失败时用。 */
static int pb_trigger_via_rmm(void)
{
    unsigned char msg[16];
    mqd_t mq;
    int ret;

    mq = mq_open("/ipc_rmm", O_WRONLY | O_NONBLOCK);
    if (mq == (mqd_t)-1) {
        logf_("[pb] mq_open /ipc_rmm failed errno=%d", errno);
        return -1;
    }
    memset(msg, 0, sizeof(msg));
    msg[8] = 0x14; msg[9] = 0x16;
    ret = mq_send(mq, (char *)msg, sizeof(msg), 0);
    mq_close(mq);
    if (ret != 0) {
        logf_("[pb] mq_send failed errno=%d", errno);
        return -1;
    }
    return 0;
}

static int pb_adec_init(void)
{
    HI_S32           ret;
    ADEC_CHN_ATTR_S  attr;
    ADEC_ATTR_G726_S g726;
    MPP_CHN_S        src, dst;

    /* ADEC_CreateChn / SYS_Bind 都要求本进程先 SYS_Init（否则 0xA0188003 /
       0xA0028003 ILLEGAL_PARAM）。本程序原本依赖 rmm 拉起的 MPP、从不初始化，
       这里补上。放在采集线程启动之前调用，避免影响正在跑的 AI 取帧。 */
    ret = HI_MPI_SYS_Init();
    logf_("[pb] SYS_Init=0x%08X", (unsigned)ret);

    memset(&g726, 0, sizeof(g726));
    g726.enG726bps = (G726_BPS_E)4;          /* 与 rmm 的 pValue={4} 一致 */

    memset(&attr, 0, sizeof(attr));
    attr.enType     = PT_G726;
    attr.u32BufSize = 20;                    /* rmm 的值；2048 会 ILLEGAL_PARAM */
    attr.enMode     = ADEC_MODE_PACK;
    attr.pValue     = &g726;

    /* 本进程不调 HI_MPI_SYS_Init()（一直依赖 rmm 把 MPP 拉起来），
       因此 CreateChn 会返回 0xA0188003 ILLEGAL_PARAM。但这不要紧：
       rmm 启动时已经在内核里建好了 chn0（g726 / buf20 / PACK / bps4），
       我们要的就是复用同一条通道 —— 直接跳过，接着做 Bind 即可。 */
    ret = HI_MPI_ADEC_CreateChn(0, &attr);
    if (ret != 0 && ret != 0xA0188004u)
        logf_("[pb] ADEC_CreateChn=0x%08X（复用 rmm 已建的 chn0）", (unsigned)ret);

    memset(&src, 0, sizeof(src));
    memset(&dst, 0, sizeof(dst));
    src.enModId = HI_ID_ADEC; src.s32DevId = 0; src.s32ChnId = 0;
    dst.enModId = HI_ID_AO;   dst.s32DevId = 0; dst.s32ChnId = 0;
    ret = HI_MPI_SYS_Bind(&src, &dst);
    if (ret != 0) {
        logf_("[pb] SYS_Bind(ADEC->AO)=0x%08X（回退 rmm 播放）", (unsigned)ret);
        return -1;
    }

    g_adec_ok = 1;
    logf_("[pb] direct ADEC ready（自己接管播放，不经 rmm）");
    return 0;
}

/* 发包线程：按 rmm 的节奏 23ms 一包，靠 ADEC 队列反压自动适配真实码率 */
static void *pb_direct_thread(void *arg)
{
    struct pb_job  *j = (struct pb_job *)arg;
    unsigned char   pkt[PB_DIRECT_PKT + 4];
    AUDIO_STREAM_S  st;
    int             off = 0, seq = 0, sent = 0, fail = 0;

    while (off + PB_DIRECT_PKT <= j->len) {
        pkt[0] = 0; pkt[1] = 1; pkt[2] = 0x28; pkt[3] = 0;
        memcpy(pkt + 4, j->data + off, PB_DIRECT_PKT);

        memset(&st, 0, sizeof(st));
        st.pStream = pkt;
        st.u32Len  = PB_DIRECT_PKT + 4;      /* 84，含 4 字节头 */
        st.u32Seq  = (HI_U32)seq++;

        if (HI_MPI_ADEC_SendStream(0, &st, HI_TRUE) == 0) sent++; else fail++;

        usleep(23000);
        off += PB_DIRECT_PKT;
    }

    if (fail) logf_("[pb] direct send: ok=%d fail=%d", sent, fail);

    /* 一段全失败说明通道根本不可用（复用假设不成立）——永久回退到 rmm，
       避免一直静音。只在"一包都没成功"时才降级。 */
    if (sent == 0 && fail > 0) {
        g_adec_ok = 0;
        logf_("[pb] direct ADEC 不可用，永久回退 rmm 播放");
    }
    free(j->data);
    free(j);
    g_pb_direct_busy = 0;
    return NULL;
}

/* 触发播放：把 PB_LINK 指向的段文件整段读进来，交给发包线程。
 * 返回 0 表示已经开始播（不等待播完），语义与原来的 mq_send 一致。 */
static int pb_trigger(void)
{
    struct stat    sb;
    struct pb_job *j;
    pthread_t      th;
    int            fd, n, got;

    if (!g_adec_ok) return pb_trigger_via_rmm();

    if (g_pb_direct_busy) {
        logf_("[pb] direct busy, drop trigger");
        return -1;
    }

    fd = open(PB_LINK, O_RDONLY);
    if (fd < 0) {
        logf_("[pb] open %s failed errno=%d", PB_LINK, errno);
        return -1;
    }
    if (fstat(fd, &sb) != 0 || sb.st_size <= 0) {
        close(fd);
        logf_("[pb] %s size invalid", PB_LINK);
        return -1;
    }
    n = (int)sb.st_size;
    if (n > PB_DIRECT_MAX) n = PB_DIRECT_MAX;

    j = (struct pb_job *)malloc(sizeof(*j));
    if (!j) { close(fd); return -1; }
    j->data = (unsigned char *)malloc(n);
    if (!j->data) { free(j); close(fd); return -1; }

    got = (int)read(fd, j->data, n);
    close(fd);
    if (got != n) {
        free(j->data); free(j);
        logf_("[pb] read %s short (%d/%d)", PB_LINK, got, n);
        return -1;
    }
    j->len = n;

    g_pb_direct_busy = 1;
    if (pthread_create(&th, NULL, pb_direct_thread, j) != 0) {
        g_pb_direct_busy = 0;
        free(j->data); free(j);
        logf_("[pb] pthread_create failed errno=%d", errno);
        return -1;
    }
    pthread_detach(th);
    return 0;
}

/* ===================== FIFO 流式播放 =====================
 *
 * 存在意义：把"一段一段喂给 rmm"换成"一条连续的流"。
 *
 * 段模式的三个固有问题（都实测过）：
 *   1. 每段结束 rmm 都会写 DAC 待机寄存器（0x20050074 <- 0x06802424），
 *      段间 DAC 反复开关，段边界有可听的咔哒；
 *   2. 最后一次关闭时，AO 的 30 帧（1.2s）缓冲里未播完的音频被丢弃，
 *      表现为"最后几个字没了"；
 *   3. 段音频时长恒等于攒包时间，ADEC 队列没有余量，gap 稍大就欠载。
 *
 * FIFO 模式：把 PB_LINK 指向命名管道，rmm 的 send_ao_beep 用 fopen+fread
 * 循环读（逆向确认无 fstat/fseek），会阻塞在读上等我们喂数据 ——
 * DAC 全程保持播放态，缓冲自然排空，close 后 rmm 读 EOF 才返回。
 *
 * 由 /tmp/hd1/hack/fifo_mode.on 开关；不存在时走原来的段模式。
 */
/* FIFO 写端是否可写（非阻塞探测）。满时就先不取数据，
 * 否则会出现"已经编码完却写不进去"，那包数据只能丢。 */
static int fifo_writable(void)
{
    fd_set wfds;
    struct timeval tv;
    int fd = g_pb_fd;

    if (fd < 0) return 0;
    FD_ZERO(&wfds);
    FD_SET(fd, &wfds);
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    return select(fd + 1, NULL, &wfds, NULL, &tv) > 0;
}

static void *th_play_fifo(void *arg)
{
    static unsigned char pkt[80];
    static short pcm[CAP_FRAME];
    int cur_session = 0;
    int triggered = 0;          /* 0=未起播 1=已握手 -1=本轮放弃 */
    int trigger_sent = 0;       /* 已向 rmm 发过触发，等它 fopen */
    int pad_left = 0, pad_total = 0;
    long long t_trig = 0;

    (void)arg;
    set_realtime(3);

    mkdir(PB_DIR, 0777);
    /* 复用已有的 FIFO，不要 unlink 重建。
     * 重建会换 inode，而 rmm 可能还持有上一轮那个 inode 的读端 —— 它永远等不到
     * 数据，我们 open 新 inode 又没有读端，双方互等到超时，最后把 /ipc_rmm 灌满。 */
    if (access(PB_FIFO, F_OK) != 0) {
        if (mkfifo(PB_FIFO, 0666) != 0) {
            logf_("[pb] mkfifo %s failed errno=%d", PB_FIFO, errno);
            return NULL;
        }
    }
    unlink(PB_LINK);
    if (symlink(PB_FIFO, PB_LINK) != 0) {
        logf_("[pb] symlink %s -> fifo failed errno=%d", PB_LINK, errno);
        return NULL;
    }
    logf_("[pb] FIFO mode: %s -> %s", PB_LINK, PB_FIFO);

    {
        const char *dr = getenv("DOWN_RAW");
        if (!dr || !dr[0]) {
            if (access("/tmp/hd1/hack/down_raw.on", F_OK) == 0)
                dr = "/tmp/pushseg/down.pcm";
        }
        if (dr && dr[0]) {
            g_down_raw = fopen(dr, "wb");
            if (g_down_raw) logf_("[pb] downlink PCM capture -> %s", dr);
        }
    }

    while (g_running) {
        /* 新会话：关掉上一轮写端，让 rmm 收 EOF 退出；编码器一并复位，
         * 否则 ADPCM 的预测器状态会跨会话累积 */
        if (g_tts_session != cur_session) {
            cur_session = g_tts_session;
            if (g_pb_fd >= 0) { close(g_pb_fd); g_pb_fd = -1; }
            triggered = 0;
            trigger_sent = 0;
            pad_left = pad_total = 0;
            g726_init(&g_pb_ctx, 2, G726_PACK_MSB);
        }

        /* 打断：立刻断流，rmm 读到 EOF 就返回 */
        if (g_pb_abort && g_pb_fd >= 0) {
            close(g_pb_fd); g_pb_fd = -1;
            triggered = 0;
            trigger_sent = 0;
            pad_left = pad_total = 0;
            logf_("[pb] fifo abort -> close");
        }

        /* 起播：水位够或 TTS 已结束，先触发再打开写端 */
        if (triggered == 0 &&
            (ring_avail(&g_down_ring) >= PB_FIFO_START || g_tts_ended)) {
            /* 触发只发一次，然后耐心等 rmm fopen。
             * 反复触发会把 /ipc_rmm 塞满（mq_send errno=11），而队列满之后
             * rmm 更处理不过来 —— 正反馈死锁，2026-11-03 踩过。 */
            if (!trigger_sent) {
                if (pb_trigger() == 0) {
                    trigger_sent = 1;
                    t_trig = now_ms();
                } else {
                    usleep(100000);     /* 队列满：退避，下一轮再试一次 */
                }
            } else {
                int fd = open(PB_FIFO, O_WRONLY | O_NONBLOCK);
                if (fd >= 0) {
                    int pre = 0;
                    g_pb_fd = fd;
                    triggered = 1;
                    trigger_sent = 0;
                    /* 预灌：把 ring 里已有的数据一次写完。
                     * FIFO 是实时供给（40ms/包），rmm 读得更快（23ms/包），
                     * 队列会长期只剩 0~1 包 —— ADEC 凑不够启动量，出声就拖。
                     * 先把攒下的这批灌进去，队列立刻填满。 */
                    while (ring_avail(&g_down_ring) >= CAP_FRAME && fifo_writable()) {
                        ring_read(&g_down_ring, pcm, CAP_FRAME);
                        if (g_down_raw) fwrite(pcm, sizeof(short), CAP_FRAME, g_down_raw);
                        g726_encode_stream(&g_pb_ctx, pcm, CAP_FRAME, pkt);
                        if (write(g_pb_fd, pkt, 80) != 80) break;
                        pre++;
                    }
                    logf_("[pb] fifo opened, preload %d pkts (%dms)", pre, pre * 40);
                } else if (now_ms() - t_trig > 3000) {
                    /* 3 秒还没握手：rmm 没在消费。删掉开关并让进程退出，
                     * wrapper 5 秒后重启时自动回到段模式 —— 自愈，
                     * 不需要人去拔卡。丢一轮对话，总比整机没声音强。 */
                    logf_("[pb] fifo open timeout, revert to segment mode");
                    unlink("/tmp/hd1/hack/fifo_mode.on");
                    sync();
                    exit(1);
                }
            }
        }

        /* 喂一包。每轮只写一包且不 continue —— 2026-11-03 的段错误就出在
         * 补静音循环用 continue 跳过了循环顶部的会话检查：网络重连把
         * g_tts_session 加一后，这里还拿着旧 fd 继续写，fd 被关闭/复用后踩崩。
         * 现在所有路径都回到循环顶部，会话检查和 abort 检查必经。 */
        if (g_pb_fd >= 0 && fifo_writable()) {
            if (ring_avail(&g_down_ring) >= CAP_FRAME) {
                ring_read(&g_down_ring, pcm, CAP_FRAME);
                if (g_down_raw) fwrite(pcm, sizeof(short), CAP_FRAME, g_down_raw);
                g726_encode_stream(&g_pb_ctx, pcm, CAP_FRAME, pkt);
                if (write(g_pb_fd, pkt, 80) != 80) {
                    logf_("[pb] fifo write errno=%d", errno);
                    close(g_pb_fd); g_pb_fd = -1; triggered = 0;
                } else if (pad_total) {
                    pad_left = pad_total = 0;   /* 又有真实数据了，取消收尾 */
                }
            } else if (g_tts_ended) {
                if (pad_total == 0) {
                    pad_left = pad_total = PB_FIFO_PAD;
                    logf_("[pb] fifo tail pad %d pkts (%dms)",
                          PB_FIFO_PAD, PB_FIFO_PAD * 40);
                }
                if (pad_left > 0) {
                    memset(pcm, 0, sizeof(pcm));
                    g726_encode_stream(&g_pb_ctx, pcm, CAP_FRAME, pkt);
                    if (write(g_pb_fd, pkt, 80) == 80) {
                        pad_left--;
                    } else {
                        close(g_pb_fd); g_pb_fd = -1; triggered = 0;
                        pad_left = pad_total = 0;
                    }
                } else {
                    close(g_pb_fd); g_pb_fd = -1;
                    triggered = 0; pad_left = pad_total = 0;
                    logf_("[pb] fifo closed (EOF -> rmm returns)");
                }
            }
        }

        usleep(10000);
    }

    if (g_pb_fd >= 0) close(g_pb_fd);
    logf_("[pb] fifo exit");
    return NULL;
}

/*
 * TTS 下行泵 v2（2026-10-31 段间空档修复）：编码与触发解耦。
 *
 * 结构：
 *   - 编码：只要 seg 没满且 ring 有数据就持续编 —— pending 期间也编，
 *     rmm 发上一段包时下一段已在内存里备好；
 *   - 触发：仅 !pending 时允许（fwrite + symlink + mq，约 20ms）。
 *
 * 正确性（symlink 安全窗口，两版实测教训）：
 *   symlink 只在 !pending 时改；pending 从触发持续到「本段全部包发完」
 *   （SendCnt >= expected）。handler 取下一条消息必然发生在上一条发完
 *   之后，而 symlink 的下一次变更又在该消息发完之后 —— 任何 fopen 时刻
 *   链接必然指向正确的段，mq 里至多排 1 条。
 *   - v0「开始发包即放行」：放行过早，handler 后取的旧消息 fopen 到新
 *     symlink = 跳段 + 重复播（实测复现）；
 *   - v1「发完才放行但 pending 期间不编码」：放行后现编现写现触发，
 *     段间空档 poll(20)+编码(15)+fwrite+symlink+mq+handler尾(20)+fopen
 *     ≈ 50~100ms，ring 生产(1x)追不上时放大到 200ms+ = 规律性卡顿（实测）。
 *   - v2：放行时下一段已备好，空档 ≈ handler 尾 20ms + 写盘触发 20ms +
 *     fopen，被 ADEC 队列稳态余量（~760ms：发送 1.74x 实时先灌满、
 *     SendStream 阻塞钳速维持）完全掩盖。
 *
 * ring 平衡：稳态 ADEC 消费=播放=1x 实时，p8 编码=ADEC 输入=1x，
 * TTS 生产=1x，三者长期平衡；瞬时抖动由 ring(4s) 吸收。
 */
static void *th_play(void *arg)
{
    /* FIFO 模式开关：见 th_play_fifo 的说明 */
    if (access("/tmp/hd1/hack/fifo_mode.on", F_OK) == 0)
        return th_play_fifo(arg);

    static unsigned char pkt[80];
    static unsigned char seg[PB_TAIL_PKTS * 80];   /* 按最大段（收尾 50 包）分配 */
    static short pcm[CAP_FRAME];
    int seg_pkts = 0, slot = 0, total_pkts = 0;
    int cur_session = 0;
    int pending = 0;        /* 已触发、尚未等到「本段全部包发完」 */
    int expected = 0;       /* 本段全部 SendStream 完成时的 SendCnt */

    (void)arg;
    set_realtime(3);    /* 低于 rmm adec_sendao(FIFO 10)：见 set_realtime 处说明 */
    mkdir(PB_DIR, 0777);
    /* 第一跳只建一次：/home/forbidden.g726 -> /tmp/pushseg/mid.g726。
     *
     * rmm 读的是硬编码的 /home 路径，这个符号链接本身在 jffs2 上躲不掉；
     * 但只要它一直指向同一个 tmpfs 目标，之后每段就只替换 tmpfs 上的第二跳，
     * 播放期间不再写 flash（原来是每段一次 unlink + symlink）。
     * 已经指向正确目标时连这一次也不写。 */
    {
        char got[160];
        int n = readlink(PB_LINK, got, sizeof got - 1);
        if (n <= 0 || n >= (int)sizeof got - 1) got[0] = '\0';
        else got[n] = '\0';
        if (strcmp(got, PB_MID) != 0) {
            /* 接管之前先留一份原厂提示音。rmm 硬编码读这个路径，一旦改成
             * 符号链接，原文件就没了；备份后拔卡或卸载本程序仍能还原，
             * 也让分发出去的包不至于悄悄破坏别人的固件内容。 */
            struct stat st;
            if (lstat(PB_LINK, &st) == 0 && S_ISREG(st.st_mode) &&
                access(PB_LINK ".orig", F_OK) != 0) {
                FILE *s = fopen(PB_LINK, "rb");
                FILE *t = s ? fopen(PB_LINK ".orig", "wb") : NULL;
                if (s && t) {
                    char buf[512];
                    size_t k;
                    while ((k = fread(buf, 1, sizeof buf, s)) > 0)
                        fwrite(buf, 1, k, t);
                    logf_("[pb] 已备份原厂提示音 -> %s.orig", PB_LINK);
                } else {
                    logf_("[pb] 备份原厂提示音失败 errno=%d（继续）", errno);
                }
                if (s) fclose(s);
                if (t) fclose(t);
            }
            unlink(PB_LINK);
            if (symlink(PB_MID, PB_LINK) != 0)
                logf_("[pb] link %s -> %s failed errno=%d", PB_LINK, PB_MID, errno);
            else
                logf_("[pb] link %s -> %s（此后播放不再写 flash）", PB_LINK, PB_MID);
        }
        /* mid.g726 不存在时先建个空文件：否则 rmm 在第一次播放前
         * 若因别的事 fopen 这条路径会失败（空文件读到 EOF，无害） */
        if (access(PB_MID, F_OK) != 0) {
            FILE *mf = fopen(PB_MID, "wb");
            if (mf) fclose(mf);
        }
    }
    /* 下行 PCM 落盘（诊断用）：把送进 G.726 编码器之前的 8 kHz PCM 原样存下来，
     * 用来区分"音频本身有问题"还是"rmm 播放有问题"。
     * 开关有两种：环境变量 DOWN_RAW=<path>，或存在标记文件 down_raw.on
     * （后者不依赖启动脚本传环境变量，touch 一下即可）。
     * 写到 tmpfs，避免诊断本身增加 SD 卡写入、干扰实时性。 */
    {
        const char *dr = getenv("DOWN_RAW");
        if (!dr || !dr[0]) {
            if (access("/tmp/hd1/hack/down_raw.on", F_OK) == 0)
                dr = "/tmp/pushseg/down.pcm";
        }
        if (dr && dr[0]) {
            g_down_raw = fopen(dr, "wb");
            if (g_down_raw)
                logf_("[pb] downlink PCM capture -> %s", dr);
            else
                logf_("[pb] capture open failed %s errno=%d", dr, errno);
        }
    }
    logf_("[pb] started v2: PCM8k -> G726-16k(2bit MSB) -> %s -> mq 0x1614", PB_LINK);

    while (g_running) {
        /* 新 TTS 会话（打断）：复位编码器/槽位，丢弃半成品段。
         * pending/expected 刻意不复位：上一段的消息可能还在队列里，
         * 必须等它全部发完才能改 symlink。 */
        if (g_tts_session != cur_session) {
            cur_session = g_tts_session;
            seg_pkts = 0;
            slot = 0;
            total_pkts = 0;
            g726_init(&g_pb_ctx, 2, G726_PACK_MSB);
            logf_("[pb] session %d: reset, adec SendCnt=%d", cur_session, adec_sendcnt());
        }

        /* 放行：本段全部包发完（或 10s 兜底防 rmm 异常卡死） */
        if (pending) {
            int now_cnt = adec_sendcnt();
            if (now_cnt >= expected) {
                pending = 0;
            } else if (now_ms() - g_pb_last_trig_ms > 10000) {
                logf_("[pb] pending timeout 10s, force release (cnt=%d expect=%d)",
                      now_cnt, expected);
                pending = 0;
            }
        }

        /* 编码优先：没到本段目标且 ring 有数据 —— pending 期间照编下一段。
         * 目标：正常段 16 包；TTS 结束后 50 包（见 PB_TAIL_PKTS 的说明）。 */
        if (seg_pkts < (g_tts_ended ? PB_TAIL_PKTS : PB_SEG_PKTS) &&
            ring_avail(&g_down_ring) >= CAP_FRAME) {
            ring_read(&g_down_ring, pcm, CAP_FRAME);
            /* 诊断落盘：这是送进 G.726 编码器之前的 8 kHz PCM，
             * 波形连续性直接反映"我给的音频"是否完整。 */
            if (g_down_raw) fwrite(pcm, sizeof(short), CAP_FRAME, g_down_raw);
            g726_encode_stream(&g_pb_ctx, pcm, CAP_FRAME, pkt);
            memcpy(seg + seg_pkts * 80, pkt, 80);
            seg_pkts++;
            total_pkts++;
            continue;
        }

        /* 收尾补静音到 PB_TAIL_PKTS 包 —— 解决"最后几个字没了"。
         *
         * rmm 每播完一个文件都会把 DAC 写回待机态（0x20050074 <- 0x06802424，
         * 见 docs/03），此刻 AO 的 30 帧（1.2s）缓冲里尚未播出的部分被静默消耗。
         * 中间段因为很快有下一次触发、DAC 被重新拉起，缓冲能续上；只有最后一段
         * 之后再无触发，缓冲尾巴连同里面的真实语音一起丢了。
         *
         * 关键是让 rmm 在 send_ao_beep 里停留 > 1.2s：补到 50 包时 ADEC 队列
         * (20 包) 灌满后 SendStream 阻塞，停留约 1.66s，缓冲自然排空。 */
        if (g_tts_ended && seg_pkts > 0 && seg_pkts < PB_TAIL_PKTS) {
            memset(pcm, 0, sizeof(pcm));
            g726_encode_stream(&g_pb_ctx, pcm, CAP_FRAME, pkt);
            memcpy(seg + seg_pkts * 80, pkt, 80);
            seg_pkts++;
            continue;
        }

        /* 触发：仅 !pending，且达到本段目标（收尾段由上面的补静音填到 50 包） */
        if (!pending && seg_pkts > 0 &&
            seg_pkts >= (g_tts_ended ? PB_TAIL_PKTS : PB_SEG_PKTS)) {
            char path[64];
            char lnk[64];
            FILE *f;

            /* 起播水位：本会话还没触发过任何段且 TTS 未结束时，ring 至少
             * 攒 PB_STARTUP_RING 再起播（供给 1x 无余量，见宏注释）。
             * 收尾段豁免：TTS 已结束，有多少播多少。 */
            if (total_pkts == seg_pkts && !g_tts_ended &&
                ring_avail(&g_down_ring) < PB_STARTUP_RING) {
                usleep(20000);
                continue;
            }

            snprintf(path, sizeof(path), PB_DIR "/xz_%03d.g726", slot);
            f = fopen(path, "wb");
            if (!f) {
                logf_("[pb] fopen %s failed errno=%d", path, errno);
                seg_pkts = 0;
                usleep(200000);
                continue;
            }
            fwrite(seg, 1, (size_t)seg_pkts * 80, f);
            fclose(f);

            /* 第二跳在 tmpfs 上原子替换：symlink 到临时名再 rename 过去。
             * /home 的第一跳（-> mid.g726）已在启动时建好，播放期间不碰 jffs2。
             * rename 是原子的，rmm 若正读旧槽位不受影响。 */
            snprintf(lnk, sizeof(lnk), PB_DIR "/.lnk.tmp");
            unlink(lnk);
            if (symlink(path, lnk) != 0) {
                logf_("[pb] symlink %s failed errno=%d", lnk, errno);
            } else if (rename(lnk, PB_MID) != 0) {
                logf_("[pb] rename %s failed errno=%d", PB_MID, errno);
            } else {
                /* 基线在 mq_send 之前取：!pending 已保证旧段包全部发完，
                 * 此刻 SendCnt 稳定；若在 mq_send 之后取，rmm handler 可能
                 * 抢先发出本段首包使基线虚高，pending 只能靠 10s 兜底释放 */
                int sc = adec_sendcnt();
                if (sc < 0) sc = 0;
                if (pb_trigger() == 0) {
                    static long long last_trig_ms = 0;
                    long long nw = now_ms();
                    expected = sc + seg_pkts;   /* 本段全部发完时的 SendCnt */
                    pending = 1;
                    g_pb_last_trig_ms = nw;
                    /* gap：相邻段触发间隔，正常=段长(480ms)；>段长=供给停顿
                     * ring：下行 ring 水位（样本），TTS 供给余量 */
                    logf_("[pb] seg slot=%d pkts=%d (%dms) total=%d expect=%d "
                          "ring=%d gap=%d",
                          slot, seg_pkts, seg_pkts * 40, total_pkts, expected,
                          ring_avail(&g_down_ring),
                          last_trig_ms ? (int)(nw - last_trig_ms) : 0);
                    last_trig_ms = nw;
                }
            }
            if (g_down_raw) fflush(g_down_raw);   /* 每段落一次，进程被杀也不丢整段 */
            seg_pkts = 0;
            slot = (slot + 1) % PB_SLOTS;
            continue;
        }

        g_pb_busy = pending || (seg_pkts > 0);
        /* 10ms：20ms 的轮询粒度会让每段多攒 20~40ms，累积成 ADEC 欠载。
         * 单核 240MHz 下每秒多醒 50 次的开销可忽略。 */
        usleep(10000);
    }
    logf_("[pb] exit");
    return NULL;
}

/* ===================== TLS / WebSocket ===================== */

static const char CA_PEM[] =
"-----BEGIN CERTIFICATE-----\n"
"MIIDjjCCAnagAwIBAgIQAzrx5qcRqaC7KGSxHQn65TANBgkqhkiG9w0BAQsFADBh\n"
"MQswCQYDVQQGEwJVUzEVMBMGA1UEChMMRGlnaUNlcnQgSW5jMRkwFwYDVQQLExB3\n"
"d3cuZGlnaWNlcnQuY29tMSAwHgYDVQQDExdEaWdpQ2VydCBHbG9iYWwgUm9vdCBH\n"
"MjAeFw0xMzA4MDExMjAwMDBaFw0zODAxMTUxMjAwMDBaMGExCzAJBgNVBAYTAlVT\n"
"MRUwEwYDVQQKEwxEaWdpQ2VydCBJbmMxGTAXBgNVBAsTEHd3dy5kaWdpY2VydC5j\n"
"b20xIDAeBgNVBAMTF0RpZ2lDZXJ0IEdsb2JhbCBSb290IEcyMIIBIjANBgkqhkiG\n"
"9w0BAQEFAAOCAQ8AMIIBCgKCAQEAuzfNNNx7a8myaJCtSnX/RrohCgiN9RlUyfuI\n"
"2/Ou8jqJkTx65qsGGmvPrC3oXgkkRLpimn7Wo6h+4FR1IAWsULecYxpsMNzaHxmx\n"
"1x7e/dfgy5SDN67sH0NO3Xss0r0upS/kqbitOtSZpLYl6ZtrAGCSYP9PIUkY92eQ\n"
"q2EGnI/yuum06ZIya7XzV+hdG82MHauVBJVJ8zUtluNJbd134/tJS7SsVQepj5Wz\n"
"tCO7TG1F8PapspUwtP1MVYwnSlcUfIKdzXOS0xZKBgyMUNGPHgm+F6HmIcr9g+UQ\n"
"vIOlCsRnKPZzFBQ9RnbDhxSJITRNrw9FDKZJobq7nMWxM4MphQIDAQABo0IwQDAP\n"
"BgNVHRMBAf8EBTADAQH/MA4GA1UdDwEB/wQEAwIBhjAdBgNVHQ4EFgQUTiJUIBiV\n"
"5uNu5g/6+rkS7QYXjzkwDQYJKoZIhvcNAQELBQADggEBAGBnKJRvDkhj6zHd6mcY\n"
"1Yl9PMWLSn/pvtsrF9+wX3N3KjITOYFnQoQj8kVnNeyIv/iPsGEMNKSuIEyExtv4\n"
"NeF22d+mQrvHRAiGfzZ0JFrabA0UWTW98kndth/Jsw1HKj2ZL7tcu7XUIOGZX1NG\n"
"Fdtom/DzMNU+MeKNhJ7jitralj41E6Vf8PlwUHBHQRFXGU7Aj64GxJUTFy8bJZ91\n"
"8rGOmaFvE7FBcf6IKshPECBV1/MUReXgRPTqh5Uykw7+U0b6LJ3/iyK5S9kJRaTe\n"
"pLiaWN0bfVKfjllDiIGknibVb63dDcY3fe0Dkhvld1927jyNxF1WW6LZZm6zNTfl\n"
"MrY=\n"
"-----END CERTIFICATE-----\n";

typedef struct {
    mbedtls_net_context      net;
    mbedtls_ssl_context      ssl;
    mbedtls_ssl_config       conf;
    mbedtls_x509_crt         ca;
    mbedtls_entropy_context  ent;
    mbedtls_ctr_drbg_context drbg;
    int ready;

    /* 流式缓冲：WebSocket 是基于流的，必须自己攒数据，
       否则半帧超时会把解析状态搞乱 */
    unsigned char rbuf[4096];
    int           rlen;
} ws_t;

static int tls_open(ws_t *w)
{
    int ret;
    const char *pers = "xzc_client";

    memset(w, 0, sizeof(*w));
    mbedtls_net_init(&w->net);
    mbedtls_ssl_init(&w->ssl);
    mbedtls_ssl_config_init(&w->conf);
    mbedtls_x509_crt_init(&w->ca);
    mbedtls_entropy_init(&w->ent);
    mbedtls_ctr_drbg_init(&w->drbg);

    if ((ret = mbedtls_ctr_drbg_seed(&w->drbg, mbedtls_entropy_func, &w->ent,
                                     (const unsigned char *)pers, strlen(pers))) != 0) return -1;
    if ((ret = mbedtls_x509_crt_parse(&w->ca, (const unsigned char *)CA_PEM, sizeof(CA_PEM))) != 0) return -2;
    if ((ret = mbedtls_net_connect(&w->net, HOST, PORT, MBEDTLS_NET_PROTO_TCP)) != 0) return -3;

    {
        struct timeval tv;
        tv.tv_sec = 25; tv.tv_usec = 0;
        setsockopt(w->net.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(w->net.fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        { int one = 1; setsockopt(w->net.fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)); }
    }

    if ((ret = mbedtls_ssl_config_defaults(&w->conf, MBEDTLS_SSL_IS_CLIENT,
                                           MBEDTLS_SSL_TRANSPORT_STREAM,
                                           MBEDTLS_SSL_PRESET_DEFAULT)) != 0) return -4;
    mbedtls_ssl_conf_authmode(&w->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&w->conf, &w->ca, NULL);
    mbedtls_ssl_conf_rng(&w->conf, mbedtls_ctr_drbg_random, &w->drbg);
    mbedtls_ssl_conf_handshake_timeout(&w->conf, 10000, 60000);

    if ((ret = mbedtls_ssl_setup(&w->ssl, &w->conf)) != 0) return -5;
    if ((ret = mbedtls_ssl_set_hostname(&w->ssl, HOST)) != 0) return -6;
    mbedtls_ssl_set_bio(&w->ssl, &w->net, mbedtls_net_send, mbedtls_net_recv, NULL);

    while ((ret = mbedtls_ssl_handshake(&w->ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) return -7;
    }
    if (mbedtls_ssl_get_verify_result(&w->ssl) != 0) return -8;

    w->ready = 1;
    return 0;
}

static void tls_close(ws_t *w)
{
    if (w->ready) mbedtls_ssl_close_notify(&w->ssl);
    mbedtls_net_free(&w->net);
    mbedtls_ssl_free(&w->ssl);
    mbedtls_ssl_config_free(&w->conf);
    mbedtls_x509_crt_free(&w->ca);
    mbedtls_ctr_drbg_free(&w->drbg);
    mbedtls_entropy_free(&w->ent);
    w->ready = 0;
}

static int ws_write_all(ws_t *w, const unsigned char *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        int r = mbedtls_ssl_write(&w->ssl, buf + off, len - off);
        if (r > 0) { off += (size_t)r; continue; }
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        return -1;
    }
    return 0;
}

/* 从 TLS 读满 n 字节（先吃本地缓冲，不够再读 socket） */
static int ws_read_exact(ws_t *w, unsigned char *dst, int need)
{
    while (w->rlen < need) {
        int r = mbedtls_ssl_read(&w->ssl, w->rbuf + w->rlen, (int)sizeof(w->rbuf) - w->rlen);
        if (r > 0) { w->rlen += r; continue; }
        if (r == 0) return -1;
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return -1;
        if (r == MBEDTLS_ERR_NET_RECV_FAILED) return -2;   /* 超时 */
        return -3;
    }
    memcpy(dst, w->rbuf, (size_t)need);
    memmove(w->rbuf, w->rbuf + need, (size_t)(w->rlen - need));
    w->rlen -= need;
    return 0;
}

static int ws_handshake(ws_t *w)
{
    unsigned char nonce[16], key[40];
    size_t keylen = 0;
    char req[768], resp[512];
    int n, ret;
    size_t rlen = 0;

    if ((ret = mbedtls_ctr_drbg_random(&w->drbg, nonce, sizeof(nonce))) != 0) return -1;
    if ((ret = mbedtls_base64_encode(key, sizeof(key) - 1, &keylen, nonce, sizeof(nonce))) != 0) return -2;
    key[keylen] = '\0';

    n = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Authorization: Bearer %s\r\n"
        "Device-Id: %s\r\n"
        "Client-Id: %s\r\n"
        "Protocol-Version: 1\r\n"
        "User-Agent: xiaozhi-cam/1.0\r\n"
        "\r\n",
        WS_PATH, HOST, (char *)key, TOKEN, g_device_id, g_client_id);

    if (ws_write_all(w, (const unsigned char *)req, (size_t)n) != 0) return -3;

    {
        unsigned char ch;
        while (rlen < sizeof(resp) - 1) {
            if (ws_read_exact(w, &ch, 1) != 0) return -4;
            resp[rlen++] = (char)ch;
            if (rlen >= 4 && !memcmp(resp + rlen - 4, "\r\n\r\n", 4)) break;
        }
    }
    resp[rlen] = '\0';
    if (!strstr(resp, "101")) {
        logf_("[WS] handshake failed:\n%.300s", resp);
        return -5;
    }
    return 0;
}

enum { WS_TEXT = 0x1, WS_BIN = 0x2, WS_CLOSE = 0x8, WS_PING = 0x9, WS_PONG = 0xA };

static int ws_send(ws_t *w, int opcode, const unsigned char *payload, size_t len)
{
    unsigned char hdr[14], mask[4];
    size_t hlen = 0, i;
    unsigned char *body;

    hdr[hlen++] = (unsigned char)(0x80 | (opcode & 0x0F));
    if (len < 126) {
        hdr[hlen++] = (unsigned char)(0x80 | (unsigned char)len);
    } else if (len < 65536) {
        hdr[hlen++] = 0x80 | 126;
        hdr[hlen++] = (unsigned char)((len >> 8) & 0xFF);
        hdr[hlen++] = (unsigned char)(len & 0xFF);
    } else {
        int k;
        hdr[hlen++] = 0x80 | 127;
        for (k = 7; k >= 0; k--) hdr[hlen++] = (unsigned char)((len >> (k * 8)) & 0xFF);
    }

    if (mbedtls_ctr_drbg_random(&w->drbg, mask, 4) != 0) return -1;
    memcpy(hdr + hlen, mask, 4);
    hlen += 4;

    body = (unsigned char *)malloc(len ? len : 1);
    if (!body) return -2;
    for (i = 0; i < len; i++) body[i] = payload[i] ^ mask[i & 3];

    if (ws_write_all(w, hdr, hlen) != 0) { free(body); return -3; }
    if (len && ws_write_all(w, body, len) != 0) { free(body); return -4; }
    free(body);
    return 0;
}

/* 返回 1=有帧 0=超时 -1=关闭 -3=错误 */
static int ws_recv(ws_t *w, int *opcode, unsigned char *buf, int cap, int *out_len, int first_timeout_ms)
{
    unsigned char h[2], m[4];
    int len, masked, r;

    if (w->rlen < 2) {
        /* 没有待解析数据时，先等第一块可读 */
        fd_set rfds;
        struct timeval tv;
        FD_ZERO(&rfds);
        FD_SET(w->net.fd, &rfds);
        tv.tv_sec = first_timeout_ms / 1000;
        tv.tv_usec = (first_timeout_ms % 1000) * 1000;
        if (select(w->net.fd + 1, &rfds, NULL, NULL, &tv) <= 0) return 0;
    }

    if ((r = ws_read_exact(w, h, 2)) != 0) return (r == -2) ? 0 : -1;

    *opcode = h[0] & 0x0F;
    masked  = (h[1] & 0x80) != 0;
    len     = h[1] & 0x7F;

    if (len == 126) {
        unsigned char e[2];
        if (ws_read_exact(w, e, 2) != 0) return -1;
        len = (e[0] << 8) | e[1];
    } else if (len == 127) {
        unsigned char e[8];
        if (ws_read_exact(w, e, 8) != 0) return -1;
        len = (e[4] << 24) | (e[5] << 16) | (e[6] << 8) | e[7];
    }

    if (masked && ws_read_exact(w, m, 4) != 0) return -1;

    if (len > cap) {
        unsigned char tmp[128];
        int left = len;
        while (left > 0) {
            int chunk = left > (int)sizeof(tmp) ? (int)sizeof(tmp) : left;
            if (ws_read_exact(w, tmp, chunk) != 0) return -1;
            left -= chunk;
        }
        *out_len = 0;
        return 2;
    }
    if (len > 0 && ws_read_exact(w, buf, len) != 0) return -1;
    if (masked) for (int i = 0; i < len; i++) buf[i] ^= m[i & 3];

    *out_len = len;
    return 1;
}

/* ===================== 极简 JSON 取值 ===================== */

/* 在 json 里找 "key":"value"，返回 value 起点，值写入 out（截断） */
static int json_get_str(const char *json, const char *key, char *out, int cap)
{
    char pat[64];
    const char *p, *q;
    int n = 0;

    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(json, pat);
    if (!p) return -1;
    p += strlen(pat);
    while (*p == ' ' || *p == ':') p++;
    if (*p != '"') return -2;
    p++;
    q = p;
    while (*q && *q != '"' && n < cap - 1) out[n++] = *q++;
    out[n] = '\0';
    return 0;
}

static int json_get_int(const char *json, const char *key, int defval)
{
    char pat[64];
    const char *p;
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(json, pat);
    if (!p) return defval;
    p += strlen(pat);
    while (*p == ' ' || *p == ':') p++;
    return atoi(p);
}

/* ===================== 编解码 ===================== */

static OpusEncoder *g_enc;
static OpusDecoder *g_dec;
static int g_dec_rate = 24000;

/* ---- 抗混叠降采样（2026-10-31 音质修复） ----
 * 旧三点平均不是抗混叠滤波器：2kHz -6.5dB（压闷）、4kHz -3.5dB（折进直流）、
 * 5kHz -5.9dB（折进 3kHz 人耳最敏感区）、6kHz -9.5dB（折进 2kHz）——
 * 4~8kHz 语音谐波几乎无衰减混叠进基带 = 金属感 + 沙沙声。上一版同病。
 * 现用 Kaiser β=7 线性相位 FIR（dsfir.h，scripts/gen_dsfir.py 生成）：
 *   24k: N=193 通带 0~3.4k ±0.1dB，4k -70dB；16k: N=97，4k 零点。
 * 实现：跨块滑动历史（N-1 样本）+ 全局相位计数，Opus 帧长任意整除皆准。
 * CPU：193MAC/输出 × 8k/s ≈ 1.5 MMAC/s（ARM926 @240MHz <1%）。
 * 状态仅 th_net 单线程使用，无锁。 */
static short ds24_hist[DS24K_TAPS - 1];
static int   ds24_hist_n;
static int   ds24_mg;               /* 下一个待产出的全局输出序号 */
static long long ds24_in;           /* 累计输入样本数 */
static short ds16_hist[DS16K_TAPS - 1];
static int   ds16_hist_n;
static int   ds16_mg;
static long long ds16_in;

void ds24k_reset(void) { ds24_hist_n = 0; ds24_mg = 0; ds24_in = 0; }
void ds16k_reset(void) { ds16_hist_n = 0; ds16_mg = 0; ds16_in = 0; }

static int ds_run(const short *src, int n, short *dst, int dst_cap,
                  short *hist, int *hist_n, int *mg, long long *in,
                  const short *h, int taps, int decim)
{
    const int H = taps - 1;
    static short cat[DS24K_TAPS - 1 + 2880];
    int c = *hist_n + n;
    long long cat_start = *in - *hist_n;    /* hist[0] 的全局样本序号 */
    int m_out = 0, i, m;

    if (n <= 0) return 0;
    if (c > (int)(sizeof(cat) / sizeof(cat[0]))) n -= c - (int)(sizeof(cat) / sizeof(cat[0]));
    memcpy(cat, hist, (size_t)*hist_n * sizeof(short));
    memcpy(cat + *hist_n, src, (size_t)n * sizeof(short));
    c = *hist_n + n;

    for (m = *mg; ; m++) {
        long long right = (long long)m * decim + H;   /* 窗口最右输入样本（全局序号） */
        long long left  = right - H;
        int idx_r, acc = 0;
        if (left < cat_start || right > cat_start + c - 1) break;
        if (m_out >= dst_cap) break;    /* 输出缓冲防御：丢弃超出部分（理论不可达） */
        idx_r = (int)(right - cat_start);
        for (i = 0; i <= H; i++)
            acc += (int)h[i] * cat[idx_r - i];
        acc >>= 14;
        dst[m_out++] = acc > 32767 ? (short)32767 : (acc < -32768 ? (short)-32768 : (short)acc);
    }
    *mg = m;

    *in += n;
    {
        int keep = c < H ? c : H;
        memcpy(hist, cat + c - keep, (size_t)keep * sizeof(short));
        *hist_n = keep;
    }
    return m_out;
}

/* 24k -> 8k（3:1 抽取，Kaiser β=7 N=193, 4kHz 抑制 -70dB） */
static int ds24k_run(const short *src, int n, short *dst)
{
    return ds_run(src, n, dst, 1440, ds24_hist, &ds24_hist_n, &ds24_mg, &ds24_in,
                  ds24k_h, DS24K_TAPS, 3);
}

/* 16k -> 8k（2:1 抽取，Kaiser β=7 N=97, 4kHz 零点） */
static int ds16k_run(const short *src, int n, short *dst)
{
    return ds_run(src, n, dst, 1440, ds16_hist, &ds16_hist_n, &ds16_mg, &ds16_in,
                  ds16k_h, DS16K_TAPS, 2);
}

/* ===================== 协议 ===================== */

static void send_json(ws_t *w, const char *json)
{
    ws_send(w, WS_TEXT, (const unsigned char *)json, strlen(json));
}

static int send_hello(ws_t *w)
{
    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"type\":\"hello\",\"version\":1,"
        "\"features\":{\"mcp\":true,\"aec\":true},"
        "\"transport\":\"websocket\","
        "\"audio_params\":{\"format\":\"opus\",\"sample_rate\":%d,"
        "\"channels\":1,\"frame_duration\":%d}}", NET_RATE, NET_FRAME_MS);
    return ws_send(w, WS_TEXT, (const unsigned char *)buf, strlen(buf));
}

static void send_listen(ws_t *w, const char *state)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"type\":\"listen\",\"state\":\"%s\",\"mode\":\"manual\"}", state);
    logf_("[tx] listen %s", state);
    send_json(w, buf);
}

/* 断了就重连——绝不退出进程。
 *
 * 背景（docs/12）：服务端 60 s 收不到音频帧就断开会话（PC 侧实测 60.0 s，
 * 发 listen start 也不续命）。旧代码把断连当致命错误 break 出去，靠 wrapper
 * sleep 5 重启进程，代价是 acodec / kws / LED 全部重新初始化、g_boot_ms 归零；
 * 而唤醒门控要求"本进程启动满 180 s"，进程又活不过 62 s，于是门永远不开。
 *
 * 进程内重连把这三个代价都消掉：KWS 不重启、宽限只走一次、静默窗口从 5 s+
 * 降到 1~2 s。
 *
 * 返回 0 = 已连上（hello 还没来，session 由调用方等）；
 * 返回 -1 = g_running 被清 0，该退出了。
 */
static int net_reconnect(ws_t *w)
{
    int attempt = 0;

    while (g_running) {
        int ret;

        attempt++;
        ret = tls_open(w);
        if (ret == 0) {
            ret = ws_handshake(w);
            if (ret == 0) {
                send_hello(w);
                logf_("[net] reconnected (attempt %d)", attempt);
                return 0;
            }
            logf_("[net] reconnect ws_handshake %d", ret);
        } else {
            logf_("[net] reconnect tls_open %d", ret);
        }
        tls_close(w);

        /* 第一次就失败才亮"连接中"：稳态的 60 s 重连是正常节奏，
         * 每分钟闪一次蓝灯会被当成故障。 */
        if (attempt > 1) led_tick(LEDP_CONNECT, now_ms());

        /* 1/2/4/8 s 退避，单核上不能打满 */
        {
            int ms = 1000 << (attempt > 3 ? 3 : (attempt - 1));
            int slept = 0;
            while (g_running && slept < ms) {
                usleep(100 * 1000);
                slept += 100;
            }
        }
    }
    return -1;
}

/* ===================== MCP / 识图 =====================
 *
 * 上游（xiaozhi-esp32）的视觉链路，已在真机逐段验证（docs/11）：
 *
 *   hello(features.mcp=true)
 *     → 服务端主动发 {"type":"mcp","payload":{jsonrpc,method:initialize}}
 *       params.capabilities.vision = {url, token}   ← 每会话一个 token
 *     → 设备回 initialize result（protocolVersion + capabilities.tools）
 *     → 服务端 notifications/initialized，然后 tools/list
 *     → 设备报工具 self.camera.take_photo
 *     → LLM 决定看图时发 tools/call {arguments:{question}}
 *     → 设备 mq 0x1610 抓拍 → multipart POST 到 vision/explain → 回 content[0].text
 *
 * 两个容易搞反的点：
 *   1. **initialize 是服务端发起的**，设备是 MCP server、云端是 client；
 *   2. MCP 消息外面还套一层 {"type":"mcp","payload":{...}}，payload 必须是对象。
 *
 * 实时性：抓拍 130 ms + 上传识别 ~1.7 s，同步做会阻塞主循环约 2 s。
 * 因此只在非播放态允许（播放时抓拍会占 rmm 主线程），并且连接超时收到 5 s。
 */

#define VISION_URL_MAX   160
#define VISION_TOK_MAX   96
#define VISION_JPG       "/tmp/vision.jpg"
#define VISION_CONN_MS   5000      /* TCP 建连 */
#define VISION_RECV_MS   12000     /* 收响应 */
#define VISION_SNAP_MS   5000      /* 等 rmm 出图 */

static char g_vision_url[VISION_URL_MAX];
static char g_vision_token[VISION_TOK_MAX];

/* ---- JSON 小工具（比 json_get_str 严格：认转义） ---- */

static void utf8_put(unsigned int cp, char *out, int cap, int *n)
{
    if (cp < 0x80) {
        if (*n < cap - 1) out[(*n)++] = (char)cp;
    } else if (cp < 0x800) {
        if (*n < cap - 2) {
            out[(*n)++] = (char)(0xC0 | (cp >> 6));
            out[(*n)++] = (char)(0x80 | (cp & 0x3F));
        }
    } else if (cp < 0x10000) {
        if (*n < cap - 3) {
            out[(*n)++] = (char)(0xE0 | (cp >> 12));
            out[(*n)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[(*n)++] = (char)(0x80 | (cp & 0x3F));
        }
    } else {
        if (*n < cap - 4) {
            out[(*n)++] = (char)(0xF0 | (cp >> 18));
            out[(*n)++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            out[(*n)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[(*n)++] = (char)(0x80 | (cp & 0x3F));
        }
    }
}

static int hex4(const char *p)
{
    int v = 0, i;
    for (i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= c - '0';
        else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
        else return -1;
    }
    return v;
}

/* 取 "key":"..."，按 JSON 规则还原转义（含 \uXXXX → UTF-8）。
 * json_get_str 遇到 \" 会提前截断，LLM 给的问句里带引号就废了，所以单独实现。 */
static int json_gets_unesc(const char *json, const char *key, char *out, int cap)
{
    char pat[64];
    const char *p;
    int n = 0;

    if (cap <= 0) return -1;
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(json, pat);
    if (!p) return -1;
    p += strlen(pat);
    while (*p == ' ' || *p == ':') p++;
    if (*p != '"') return -2;
    p++;

    while (*p && *p != '"' && n < cap - 1) {
        if (*p != '\\') { out[n++] = *p++; continue; }
        p++;
        switch (*p) {
        case 'n': out[n++] = '\n'; p++; break;
        case 't': out[n++] = '\t'; p++; break;
        case 'r': out[n++] = '\r'; p++; break;
        case 'b': out[n++] = '\b'; p++; break;
        case 'f': out[n++] = '\f'; p++; break;
        case 'u': {
            int cp = hex4(p + 1);
            if (cp < 0) { p++; break; }
            p += 5;
            /* 代理对：D800-DBFF + 后面紧跟 \uDC00-\uDFFF */
            if (cp >= 0xD800 && cp <= 0xDBFF && p[0] == '\\' && p[1] == 'u') {
                int lo = hex4(p + 2);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    p += 6;
                }
            }
            utf8_put((unsigned int)cp, out, cap, &n);
            break;
        }
        default: if (*p) out[n++] = *p; p++; break;
        }
    }
    out[n] = '\0';
    return 0;
}

/* 把任意 UTF-8 文本转成可塞进 JSON 字符串的形式 */
static int json_escape(const char *src, char *dst, int cap)
{
    int n = 0;
    while (*src && n < cap - 7) {
        unsigned char c = (unsigned char)*src++;
        if (c == '"' || c == '\\') { dst[n++] = '\\'; dst[n++] = (char)c; }
        else if (c == '\n') { dst[n++] = '\\'; dst[n++] = 'n'; }
        else if (c == '\r') { dst[n++] = '\\'; dst[n++] = 'r'; }
        else if (c == '\t') { dst[n++] = '\\'; dst[n++] = 't'; }
        else if (c < 0x20) { n += snprintf(dst + n, (size_t)(cap - n), "\\u%04x", c); }
        else dst[n++] = (char)c;
    }
    dst[n] = '\0';
    return n;
}

/* ---- 抓拍：rmm mq 0x1610（docs/11 §三，真机 126~134 ms） ---- */

static int snap_take(const char *path)
{
    unsigned char msg[512];
    mqd_t mq;
    int len = (int)strlen(path);
    long long t0;
    struct stat st;

    if (len < 1 || len > 480) return -1;
    unlink(path);

    memset(msg, 0, sizeof(msg));
    msg[8]  = 0x10;                       /* u16 LE 0x1610 */
    msg[9]  = 0x16;
    msg[0xc] = (unsigned char)(len & 0xff);
    msg[0xd] = (unsigned char)((len >> 8) & 0xff);
    msg[0xe] = (unsigned char)((len >> 16) & 0xff);
    msg[0xf] = (unsigned char)((len >> 24) & 0xff);
    memcpy(msg + 0x10, path, (size_t)len);

    mq = mq_open("/ipc_rmm", O_WRONLY | O_NONBLOCK);
    if (mq == (mqd_t)-1) { logf_("[vision] mq_open: %s", strerror(errno)); return -1; }
    if (mq_send(mq, (char *)msg, (size_t)(0x10 + len), 0) != 0) {
        logf_("[vision] mq_send: %s", strerror(errno));
        mq_close(mq);
        return -1;
    }
    mq_close(mq);

    t0 = now_ms();
    while (now_ms() - t0 < VISION_SNAP_MS) {
        if (stat(path, &st) == 0 && st.st_size > 0) {
            char tmp[64];
            struct stat stt;
            snprintf(tmp, sizeof(tmp), "%s.tmp", path);
            if (stat(tmp, &stt) != 0) return 0;   /* .tmp 消失 = 写完改名了 */
        }
        usleep(50 * 1000);
    }
    return -1;
}

/* ---- multipart POST 到 vision/explain（明文 HTTP，设备实测 1.7 s） ---- */

static int send_all_fd(int fd, const char *buf, int len)
{
    int off = 0;
    while (off < len) {
        int n = (int)send(fd, buf + off, (size_t)(len - off), 0);
        if (n <= 0) return -1;
        off += n;
    }
    return 0;
}

static int vision_post(const char *jpg, const char *question, char *out, int outcap)
{
    static char req[1024];
    static char part[1024];
    static char resp[4096];
    static const char *BND = "----XiaoZhiCamBoundary";

    struct stat st;
    char host[96], path[192];
    char *p, *q;
    int fd = -1, n, total_hdr, rlen = 0;
    long long content_len;
    struct addrinfo hints, *res = NULL, *ai;
    struct timeval tv;
    FILE *fp;

    out[0] = '\0';
    if (stat(jpg, &st) != 0 || st.st_size <= 0) return -1;
    if (strncmp(g_vision_url, "http://", 7) != 0) {
        logf_("[vision] only http:// supported: %.40s", g_vision_url);
        return -2;
    }

    p = g_vision_url + 7;
    q = strchr(p, '/');
    if (!q || (size_t)(q - p) >= sizeof(host)) return -2;
    memcpy(host, p, (size_t)(q - p));
    host[q - p] = '\0';
    snprintf(path, sizeof(path), "%s", q);
    if ((p = strchr(host, ':')) != NULL) *p = '\0';

    n = snprintf(part, sizeof(part),
        "--%s\r\nContent-Disposition: form-data; name=\"question\"\r\n\r\n%s\r\n"
        "--%s\r\nContent-Disposition: form-data; name=\"file\"; filename=\"camera.jpg\"\r\n"
        "Content-Type: image/jpeg\r\n\r\n",
        BND, question, BND);
    if (n <= 0 || n >= (int)sizeof(part)) return -2;
    content_len = (long long)n + (long long)st.st_size + (long long)strlen(BND) + 8;

    n = snprintf(req, sizeof(req),
        "POST %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Device-Id: %s\r\n"
        "Client-Id: %s\r\n"
        "Authorization: Bearer %s\r\n"
        "Content-Type: multipart/form-data; boundary=%s\r\n"
        "Content-Length: %lld\r\n"
        "Connection: close\r\n\r\n",
        path, host, g_device_id, g_client_id, g_vision_token, BND, content_len);
    if (n <= 0 || n >= (int)sizeof(req)) return -2;
    total_hdr = n;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, "80", &hints, &res) != 0) {
        logf_("[vision] dns fail %s", host);
        return -3;
    }
    for (ai = res; ai; ai = ai->ai_next) {
        int fl, rc, err = 0;
        socklen_t el = sizeof(err);
        fd_set wf;

        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;

        /* 非阻塞 connect + 5 s 超时：阻塞 connect 到黑洞主机要 20 s+，
         * 那段时间主循环完全停摆，状态机会被拖坏。 */
        fl = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc < 0 && errno == EINPROGRESS) {
            FD_ZERO(&wf);
            FD_SET(fd, &wf);
            tv.tv_sec = VISION_CONN_MS / 1000;
            tv.tv_usec = (VISION_CONN_MS % 1000) * 1000;
            rc = select(fd + 1, NULL, &wf, NULL, &tv);
            if (rc > 0) {
                /* select 成功返回的是"就绪个数"（这里是 1），不是 0。
                 * 必须显式归零，否则下面 rc == 0 的判断会把成功当失败
                 * —— 2026-11-01 23:01 就栽在这里：连得上却报 connect fail。 */
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
                rc = err ? -1 : 0;
                if (err) errno = err;
            } else {
                rc = -1;
            }
        }
        fcntl(fd, F_SETFL, fl);
        if (rc == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) { logf_("[vision] connect fail errno=%d", errno); return -3; }

    tv.tv_sec = VISION_RECV_MS / 1000;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (send_all_fd(fd, req, total_hdr) != 0 ||
        send_all_fd(fd, part, (int)strlen(part)) != 0) {
        logf_("[vision] send head fail");
        close(fd);
        return -4;
    }
    fp = fopen(jpg, "rb");
    if (!fp) { close(fd); return -4; }
    for (;;) {
        char buf[2048];
        size_t got = fread(buf, 1, sizeof(buf), fp);
        if (got == 0) break;
        if (send_all_fd(fd, buf, (int)got) != 0) {
            logf_("[vision] send body fail");
            fclose(fp);
            close(fd);
            return -4;
        }
    }
    fclose(fp);
    {
        char tail[96];
        int tl = snprintf(tail, sizeof(tail), "\r\n--%s--\r\n", BND);
        if (send_all_fd(fd, tail, tl) != 0) { close(fd); return -4; }
    }

    while (rlen < (int)sizeof(resp) - 1) {
        int r = (int)recv(fd, resp + rlen, (size_t)(sizeof(resp) - 1 - rlen), 0);
        if (r <= 0) break;
        rlen += r;
    }
    resp[rlen] = '\0';
    close(fd);

    if (strncmp(resp, "HTTP/1.", 7) != 0) { logf_("[vision] no http: %.40s", resp); return -5; }
    if (!strstr(resp, " 200 ")) { logf_("[vision] status: %.40s", resp); return -5; }

    {
        char *body = strstr(resp, "\r\n\r\n");
        if (!body) return -5;
        body += 4;
        if (json_gets_unesc(body, "text", out, outcap) != 0) {
            snprintf(out, (size_t)outcap, "%s", body);   /* 没 text 就整段回给模型 */
        }
    }
    return 0;
}

/* ---- MCP 回包 ---- */

static void mcp_send_text(ws_t *w, int id, const char *text, int is_error)
{
    static char esc[2400];
    static char buf[2800];

    json_escape(text, esc, sizeof(esc));
    snprintf(buf, sizeof(buf),
        "{\"type\":\"mcp\",\"payload\":{\"jsonrpc\":\"2.0\",\"id\":%d,"
        "\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"%s\"}],\"isError\":%s}}}",
        id, esc, is_error ? "true" : "false");
    ws_send(w, WS_TEXT, (const unsigned char *)buf, strlen(buf));
}

static void mcp_reply_initialize(ws_t *w, int id)
{
    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"type\":\"mcp\",\"payload\":{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":{"
        "\"protocolVersion\":\"2024-11-05\",\"capabilities\":{\"tools\":{}},"
        "\"serverInfo\":{\"name\":\"xiaozhi-cam\",\"version\":\"0.3.0\"}}}}", id);
    ws_send(w, WS_TEXT, (const unsigned char *)buf, strlen(buf));
}

static void mcp_reply_tools(ws_t *w, int id)
{
    static const char *tools =
        "{\"type\":\"mcp\",\"payload\":{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":{\"tools\":[{"
        "\"name\":\"self.camera.take_photo\","
        "\"description\":\"你有一个摄像头。用户让你看东西、问画面里有什么时，用这个工具拍一张照片。"
        "图像可能相对房间转了 90 度，请按房间的自然朝向理解。\","
        "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
        "\"question\":{\"type\":\"string\",\"description\":\"你想问照片的问题\"}},"
        "\"required\":[\"question\"]}}]}}}";
    char buf[800];
    snprintf(buf, sizeof(buf), tools, id);
    ws_send(w, WS_TEXT, (const unsigned char *)buf, strlen(buf));
}

static void mcp_tool_call(ws_t *w, const char *msg, int id)
{
    char name[64] = {0};
    char q[200] = {0};
    static char answer[1200];
    long long t0;
    int r;

    json_get_str(msg, "name", name, sizeof(name));
    json_gets_unesc(msg, "question", q, sizeof(q));

    if (strcmp(name, "self.camera.take_photo") != 0) {
        mcp_send_text(w, id, "unknown tool", 1);
        return;
    }
    if (!g_vision_url[0]) {
        mcp_send_text(w, id, "server did not provide vision url", 1);
        return;
    }

    /* 注意：服务端是「先 tts start，再发 tools/call」——实测 22:50 那次，
       TTS 已经在播"嗯"，工具调用才到。所以这里**不能**按 ST_SPEAK 拦截，
       否则永远拒拍（第一版就是这么错的）。
       代价：抓拍会占住 rmm 主线程约 130 ms、上传约 1.7 s，这段时间主循环
       不取帧；TTS 供给跟不上时播放线程会补静音（架构上就是这么设计的，
       不会爆音），最多在"嗯"这种语气词上多一小段静默。 */
    t0 = now_ms();
    if (snap_take(VISION_JPG) != 0) {
        logf_("[vision] snap failed after %lld ms", now_ms() - t0);
        mcp_send_text(w, id, "camera capture failed", 1);
        return;
    }
    logf_("[vision] snap ok %lld ms, q=%.60s", now_ms() - t0, q[0] ? q : "(none)");

    r = vision_post(VISION_JPG, q[0] ? q : "请描述这张图片", answer, sizeof(answer));
    if (r != 0) {
        logf_("[vision] post failed r=%d after %lld ms", r, now_ms() - t0);
        mcp_send_text(w, id, "image upload failed", 1);
        return;
    }
    logf_("[vision] done %lld ms: %.70s", now_ms() - t0, answer);
    mcp_send_text(w, id, answer, 0);
}

static void mcp_handle(ws_t *w, const char *msg)
{
    char method[48] = {0};
    int id;

    json_get_str(msg, "method", method, sizeof(method));
    if (!method[0]) return;                                  /* result / 空包，忽略 */
    if (!strncmp(method, "notifications", 13)) return;       /* 通知不回包 */
    id = json_get_int(msg, "id", -1);
    logf_("[mcp] <- %s id=%d", method, id);
    if (id < 0) return;

    if (!strcmp(method, "initialize")) {
        char url[VISION_URL_MAX] = {0}, tok[VISION_TOK_MAX] = {0};
        if (json_get_str(msg, "url", url, sizeof(url)) == 0 && url[0] == 'h') {
            snprintf(g_vision_url, sizeof(g_vision_url), "%s", url);
            if (json_get_str(msg, "token", tok, sizeof(tok)) == 0)
                snprintf(g_vision_token, sizeof(g_vision_token), "%s", tok);
            logf_("[mcp] vision %s (token %.8s...)", g_vision_url, g_vision_token);
        }
        mcp_reply_initialize(w, id);
    } else if (!strcmp(method, "tools/list")) {
        mcp_reply_tools(w, id);
    } else if (!strcmp(method, "tools/call")) {
        mcp_tool_call(w, msg, id);
    } else {
        mcp_send_text(w, id, "unsupported method", 1);
    }
}

/* ===================== 主流程 ===================== */

/* ---- 激活状态查询 ----
 *
 * 小智的流程是"先走 OTA 接口拿 WebSocket 地址与激活码，再建连"（docs/01 §179）。
 * 我们的客户端本来是直接硬编码地址连 WebSocket —— 已绑定的设备能连上，
 * 但别人拿到包、设备未绑定时会被服务端拒，而且没有任何地方能看到验证码。
 *
 * 这里在建立主连接前单独跑一次 OTA 请求，只做一件事：把 activation.code
 * 取出来落到 SD 卡，让用户能看见。返回：
 *   0 = 已激活  1 = 未激活（code 已填入 out）  -1 = 查询失败
 */
static ws_t g_ota_ws;

static int ota_check(char *out, int cap)
{
    static const char body[] =
        "{\"application\":{\"version\":\"1.0.0\"},\"board\":{\"type\":\"xiaozhi-cam\"}}";
    char req[512];
    static char resp[2048];
    int n, rlen = 0, ret = -1;

    if (out && cap > 0) out[0] = '\0';

    if (tls_open(&g_ota_ws) != 0) return -1;

    n = snprintf(req, sizeof(req),
        "POST /xiaozhi/ota/ HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Device-Id: %s\r\n"
        "Client-Id: %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n%s",
        HOST, g_device_id, g_client_id, (int)(sizeof(body) - 1), body);

    if (ws_write_all(&g_ota_ws, (const unsigned char *)req, (size_t)n) == 0) {
        while (rlen < (int)sizeof(resp) - 1) {
            unsigned char ch;
            if (ws_read_exact(&g_ota_ws, &ch, 1) != 0) break;
            resp[rlen++] = (char)ch;
        }
        resp[rlen] = '\0';

        /* 未绑定时响应里有 "activation":{"code":"123456"}；
         * 已绑定时整个 activation 字段不存在。 */
        if (strstr(resp, "activation") != NULL &&
            json_get_str(resp, "code", out, cap) == 0 && out[0]) {
            ret = 1;
        } else {
            ret = 0;
        }
    }

    tls_close(&g_ota_ws);
    return ret;
}

/* 把验证码写到用户能看到的地方：SD 卡根目录 + 日志。
 * 设备没屏幕、没按键，这是唯一不依赖额外工具的途径（拔卡或用 telnet 看日志）。 */
static void ota_publish_code(const char *code)
{
    FILE *f = fopen("/tmp/hd1/ACTIVATION.txt", "w");
    if (f) {
        fprintf(f, "此设备尚未绑定小智账号\n");
        fprintf(f, "============================\n\n");
        fprintf(f, "    验证码:  %s\n\n", code);
        fprintf(f, "============================\n");
        fprintf(f, "步骤：\n");
        fprintf(f, "  1. 浏览器打开 https://xiaozhi.me\n");
        fprintf(f, "  2. 登录后进入「控制台 → 设备」\n");
        fprintf(f, "  3. 添加设备，输入上面的验证码\n");
        fprintf(f, "  4. 绑定成功后本文件会自动消失\n");
        fclose(f);
        sync();
    }
    logf_("[ota] 设备未绑定，验证码 %s（已写入 SD 卡 ACTIVATION.txt）", code);
}

/* ===================== 本地语音播报（激活码）=====================
 *
 * 设备没有屏幕，也没有 TTS 引擎；而激活发生在「未绑定」状态，此时拿不到
 * 服务端 TTS —— 所以验证码只能本地发声。验证码是纯数字，用 SD 卡上的
 * 预生成数字音拼接即可（素材由 build/gen_voice.py 用 edge-tts 生成，
 * 8 kHz / 16-bit / mono）：
 *
 *     <sd>/hack/voice/pre.pcm       "激活码是"
 *     <sd>/hack/voice/d0..d9.pcm    "零".."九"
 *
 * 播放走与 TTS 完全相同的通路（段文件 + 双层 symlink + mq 0x1614），
 * 差别只是同步执行、不经过 th_play 和下行 ring。这样无需任何新机制，
 * 也自动继承已验证的「收尾段补静音」策略（否则 AO 的 1.2 s 缓冲会在
 * rmm 关 DAC 时把最后几个字吞掉，见 docs/13 §十）。
 */
#define ANN_DIR       "/tmp/hd1/hack/voice"
#define ANN_MAX_PKTS  640               /* 上限 ≈ 25 s，够播两遍 */
#define ANN_REPEAT    2                 /* 念两遍：开机时用户可能不在旁边 */

static char g_act_code[16];             /* 未绑定的验证码，等音频就绪后再念 */

static unsigned char *g_ann_pkts;                    /* 播报期间分配，播完释放 */
static unsigned char g_ann_seg[PB_TAIL_PKTS * 80];   /* 单段（含尾部静音） */
static unsigned char g_ann_sil[80];                  /* 一个静音包 */
static G726Ctx g_ann_ctx;

/* 编码一段真实静音，得到一个"静音包"。不能简单填 0：G.726 是 ADPCM，
 * 码字 0 只表示最小步进，连编几包让预测器收敛后再取稳态包。 */
static void ann_make_silence(void)
{
    G726Ctx c;
    short pcm[320];
    int i;

    memset(pcm, 0, sizeof(pcm));
    g726_init(&c, 2, G726_PACK_MSB);
    for (i = 0; i < 4; i++)
        g726_encode_stream(&c, pcm, 320, g_ann_sil);
}

static int ann_add_silence(unsigned char *out, int npkts, int max, int ms)
{
    short pcm[320];

    memset(pcm, 0, sizeof(pcm));
    while (ms > 0 && npkts < max) {
        g726_encode_stream(&g_ann_ctx, pcm, 320, out + npkts * 80);
        npkts++;
        ms -= 40;
    }
    return npkts;
}

static int ann_add_file(const char *name, unsigned char *out, int npkts, int max)
{
    char p[128];
    short pcm[320];
    size_t n;
    FILE *f;

    snprintf(p, sizeof(p), ANN_DIR "/%s.pcm", name);
    f = fopen(p, "rb");
    if (!f) {
        logf_("[ann] 缺少语音素材 %s", p);
        return -1;
    }
    while (npkts < max && (n = fread(pcm, sizeof(short), 320, f)) == 320) {
        g726_encode_stream(&g_ann_ctx, pcm, 320, out + npkts * 80);
        npkts++;
    }
    fclose(f);
    return npkts;
}

/* 拼出完整播报的包序列，返回包数（<0 表示素材缺失） */
static int ann_build(const char *code)
{
    char name[8];
    int n = 0, i, rep;

    g726_init(&g_ann_ctx, 2, G726_PACK_MSB);
    for (rep = 0; rep < ANN_REPEAT; rep++) {
        n = ann_add_file("pre", g_ann_pkts, n, ANN_MAX_PKTS);
        if (n < 0) return -1;
        n = ann_add_silence(g_ann_pkts, n, ANN_MAX_PKTS, 300);
        for (i = 0; code[i] && n < ANN_MAX_PKTS - 2; i++) {
            if (code[i] < '0' || code[i] > '9') continue;
            snprintf(name, sizeof(name), "d%c", code[i]);
            n = ann_add_file(name, g_ann_pkts, n, ANN_MAX_PKTS);
            if (n < 0) return -1;
            n = ann_add_silence(g_ann_pkts, n, ANN_MAX_PKTS, 350);
        }
        n = ann_add_silence(g_ann_pkts, n, ANN_MAX_PKTS, 900);
    }
    return n;
}

/* 触发一段并等它全部送进 ADEC（语义与 th_play 的 pending 一致） */
static int ann_fire(const unsigned char *data, int npkts, int slot)
{
    char p[80], lnk[80];
    FILE *f;
    int sc, expect, spin = 0;

    snprintf(p, sizeof(p), PB_DIR "/ann_%03d.g726", slot);
    f = fopen(p, "wb");
    if (!f) {
        logf_("[ann] 写段失败 errno=%d", errno);
        return -1;
    }
    fwrite(data, 1, (size_t)npkts * 80, f);
    fclose(f);

    snprintf(lnk, sizeof(lnk), PB_DIR "/.lnk.tmp");
    unlink(lnk);
    if (symlink(p, lnk) != 0 || rename(lnk, PB_MID) != 0) {
        logf_("[ann] 链接失败 errno=%d", errno);
        return -1;
    }

    sc = adec_sendcnt();
    if (sc < 0) sc = 0;
    if (pb_trigger() != 0) return -1;
    expect = sc + npkts;

    while (spin++ < 4000) {           /* 兜底 40 s */
        int v = adec_sendcnt();
        if (v >= expect) return 0;
        usleep(10000);
    }
    logf_("[ann] 段超时 %d/%d", adec_sendcnt(), expect);
    return 0;                          /* 不中断整段播报 */
}

/* 同步播报验证码。必须在 th_play 启动之前调用 —— 两者会争用同一个
 * symlink 和 mq 触发通路。 */
static void pb_announce_code(const char *code)
{
    char mark[64];
    char last[32];
    FILE *mf;
    int total, off = 0, slot = 0;
    int skip_mark = (getenv("ANN_ONLY") != NULL);   /* 手动测试不占标记 */

    /* 同一个验证码只念一次。
     * 未绑定状态下主连接会被服务端拒绝（实测 [FATAL] tls_open -7），
     * wrapper 5 秒后重拉进程，于是每次重启都会从 OTA 重新拿到同一个
     * 验证码并再念 14 秒 —— 用户会以为设备坏了。用 SD 卡上的标记文件
     * 记住已播过的验证码；服务端换了码才会重播。 */
    snprintf(mark, sizeof(mark), "/tmp/hd1/hack/.announced");
    last[0] = '\0';
    if (!skip_mark) {
        mf = fopen(mark, "r");
        if (mf) {
            if (fgets(last, sizeof(last), mf)) last[strcspn(last, "\r\n")] = '\0';
            fclose(mf);
        }
        if (strcmp(last, code) == 0) {
            logf_("[ann] 验证码 %s 已播报过，跳过（删 %s 可强制重播）", code, mark);
            return;
        }
    }

    logf_("[ann] 播报验证码 %s（%d 遍）", code, ANN_REPEAT);

    /* 运行期只剩 1~2 MB 可用内存，包序列按需分配、播完立刻释放，
     * 不长期占着 .bss（640 包 = 50 KB）。 */
    g_ann_pkts = (unsigned char *)malloc((size_t)ANN_MAX_PKTS * 80);
    if (!g_ann_pkts) {
        logf_("[ann] 内存不足，跳过播报（验证码见 ACTIVATION.txt）");
        return;
    }
    total = ann_build(code);
    if (total <= 0) {
        logf_("[ann] 跳过播报，验证码见 ACTIVATION.txt");
        free(g_ann_pkts);
        g_ann_pkts = NULL;
        return;
    }
    logf_("[ann] %d 包 / %d 秒", total, total * 40 / 1000);

    ann_make_silence();
    mkdir(PB_DIR, 0777);

    while (off < total) {
        int remain = total - off;
        int seg, pad = 0, i;

        if (remain > PB_SEG_PKTS) {
            seg = PB_SEG_PKTS;
        } else {
            /* 收尾段补静音到 PB_TAIL_PKTS，理由见 docs/13 §十：
             * rmm 播完关闭 DAC 时，AO 那 1.2 s 缓冲里未播出的部分会被丢弃 */
            seg = remain;
            pad = PB_TAIL_PKTS - seg;
            if (pad < 0) pad = 0;
        }
        memcpy(g_ann_seg, g_ann_pkts + (size_t)off * 80, (size_t)seg * 80);
        for (i = 0; i < pad; i++)
            memcpy(g_ann_seg + (size_t)(seg + i) * 80, g_ann_sil, 80);

        if (ann_fire(g_ann_seg, seg + pad, slot) != 0) break;
        slot = (slot + 1) % PB_SLOTS;
        off += seg;
    }
    free(g_ann_pkts);
    g_ann_pkts = NULL;

    /* 记下已播报，避免 wrapper 重启后重复念 */
    if (!skip_mark) {
        mf = fopen(mark, "w");
        if (mf) {
            fprintf(mf, "%s\n", code);
            fclose(mf);
        }
    }
    logf_("[ann] 播报结束");
}

int main(void)
{
    ws_t ws;
    pthread_t t_cap, t_play;
    int ret, state = ST_CONNECTING;
    int listen_restarts = 0;      /* 唤醒后无人说话的重开轮数：满 2 轮回门控 */
    long long t_state = 0;
    unsigned char *frame_buf;
    short pcm960[NET_FRAME];
    unsigned char opus_out[1024];
    short dec_pcm[2880];
    int session_ok = 0;
    int t_cap_ok = 0, t_play_ok = 0;

    g_boot_ms = now_ms();
    g_log = fopen("/tmp/hd1/client.log", "w");
    if (!g_log) g_log = fopen("/tmp/client.log", "w");
    load_identity();
    /* 只解析并打印身份后退出：用于确认 Device-Id 是否取到了本机 MAC，
     * 不碰音频/网络，可以随时运行（ID_ONLY=1 ./p8s）。 */
    if (getenv("ID_ONLY")) return 0;

    /* 只播报验证码后退出：验证素材与播放通路时用，也方便用户手动重播。
     *     ANN_ONLY=1 ANN_CODE=123456 ./p8s
     * 注意播报会占用 mq 触发通路，测试前应先停掉正在运行的实例。 */
    if (getenv("ANN_ONLY")) {
        const char *c = getenv("ANN_CODE");
        snprintf(g_act_code, sizeof(g_act_code), "%s",
                 (c && c[0]) ? c : "123456");
        pb_announce_code(g_act_code);
        return 0;
    }

    /* 查询激活状态。未绑定时把验证码落到 SD 卡 —— 设备没有屏幕，
     * 这是用户唯一能看到验证码的地方（拔卡看文件，或 telnet 看日志）。
     * 同时记到 g_act_code，等音频初始化完成后用语音念出来。 */
    {
        char code[16];
        int r = ota_check(code, sizeof(code));
        if (r == 1) {
            ota_publish_code(code);
            snprintf(g_act_code, sizeof(g_act_code), "%s", code);
        } else if (r == 0) {
            logf_("[ota] 设备已绑定");
            unlink("/tmp/hd1/ACTIVATION.txt");   /* 绑定后清掉旧的 */
        } else {
            logf_("[ota] 查询失败，按已绑定继续");
        }
    }
    /* 录音落盘：必须同时给 CAP_RAW 路径与 CAP_SECS 才开。
     * 2026-11-01：此前的默认值 g_raw_max=100 让每次启动都落 64 KB，
     * 而 /tmp/c 不存在时还会回退写 /cap.raw —— **jffs2 根分区**，3.5 MB 已用 79%，
     * 按 24 次/小时算就是每小时 1.4 MB 闪存写。默认关，且不再回退到根分区。 */
    {
        const char *rawp = getenv("CAP_RAW");
        const char *capsec = getenv("CAP_SECS");
        if (rawp && rawp[0] && capsec && atoi(capsec) > 0) {
            /* 每帧 320 点 @8k = 40 ms → 每秒 25 帧 */
            g_raw_max = atoi(capsec) * 25;
            g_raw = fopen(rawp, "wb");
            if (g_raw) logf_("[cap] 录音开启 %s（%d 帧）", rawp, g_raw_max);
            else logf_("[cap] 打不开 %s: %s（不落盘，继续）", rawp, strerror(errno));
        }
    }

    logf_("=================================================");
    logf_(" XiaoZhiCam Client   mbedTLS %s  opus %s",
          MBEDTLS_VERSION_STRING, opus_get_version_string());
    logf_("=================================================");

    frame_buf = (unsigned char *)malloc(8193);
    if (!frame_buf) { logf_("[ERR] no mem"); return 1; }

    if (ring_init(&g_up_ring, 1 << 16) != 0) { logf_("[ERR] up ring"); return 1; }
    if (ring_init(&g_down_ring, 1 << 15) != 0) { logf_("[ERR] down ring"); return 1; }

    /* Opus 编码器：16k mono */
    {
        int err = 0;
        g_enc = opus_encoder_create(NET_RATE, 1, OPUS_APPLICATION_VOIP, &err);
        if (err != OPUS_OK || !g_enc) { logf_("[ERR] opus encoder %d", err); return 1; }
        opus_encoder_ctl(g_enc, OPUS_SET_BITRATE(OPUS_BITRATE));
        opus_encoder_ctl(g_enc, OPUS_SET_COMPLEXITY(OPUS_COMPLEXITY));
        opus_encoder_ctl(g_enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
        opus_encoder_ctl(g_enc, OPUS_SET_INBAND_FEC(1));
        logf_("[OK] opus encoder 16k mono, %d bps, complexity %d",
              OPUS_BITRATE, OPUS_COMPLEXITY);
    }

    if (audio_open() != 0) {
        logf_("[FATAL] audio open failed");
        audio_close();
        return 2;
    }

    /* 未绑定则用语音念出验证码。放在这里的原因：
     *  - 音频设备已初始化，rmm 的 ADEC/AO 通道可用；
     *  - 各线程还没起来，不会与 th_play 争用 symlink 和 mq。
     * 代价是未绑定时启动被推迟约 12 s，但此时联网本来也没用。 */
    if (g_act_code[0])
        pb_announce_code(g_act_code);

    /* 唤醒引擎：必须在采集线程起来之前初始化（mel 表要 malloc ~20 KB）。
     * cold_start=1 给 6 s 防抖——实测上电后前 1~2 s 有一次 255 的瞬态。 */
    if (kws_init(NULL)) {
        kws_set_params(KWS_DEF_THRESHOLD, KWS_DEF_GAIN_DB, KWS_DEF_GATE_DB10);
        kws_arm(1);
        logf_("[kws] 引擎就绪：阈值=%d 增益=%d dB 门控=%.1f dB（只读模式，不接会话）",
              KWS_DEF_THRESHOLD, KWS_DEF_GAIN_DB, KWS_DEF_GATE_DB10 / 10.0);
    } else {
        g_kws_on = 0;
        logf_("[ERR] kws_init 失败，唤醒检测关闭");
    }

    /* LED 指示：打不开只告警，不影响语音链路 */
    if (led_open() != 0) logf_("[warn] led_open failed errno=%d", errno);
    led_tick(LEDP_BOOT, now_ms());
    logf_("[led] state machine ready (%d patterns)", (int)LEDP_NUM);

    pthread_create(&t_cap, NULL, th_cap, NULL);
    pthread_create(&t_play, NULL, th_play, NULL);
    t_cap_ok = 1;
    t_play_ok = 1;

    if (getenv("CAP_ONLY")) {
        /* 只采集模式：用于录房间底噪做负样本。
           走的是和正常会话完全相同的取帧路径，避免"探针能跑但固件不行"的偏差。 */
        int secs = atoi(getenv("CAP_ONLY"));
        long long t0 = now_ms();

        logf_("[cap] 只采集模式 %d s，序列号 %d 帧", secs, g_raw_max);
        while (g_running && (now_ms() - t0) < (long long)secs * 1000) {
            usleep(200000);
            led_tick(LEDP_IDLE, now_ms());
        }
        logf_("[cap] 采集结束，实际 %d 帧 %.1f s", g_raw_frames,
              (double)g_raw_frames * 320.0 / 8000.0);
        goto shutdown;
    }

    ret = tls_open(&ws);
    if (ret != 0) { logf_("[FATAL] tls_open %d", ret); goto shutdown; }
    ret = ws_handshake(&ws);
    if (ret != 0) { logf_("[FATAL] ws_handshake %d", ret); goto shutdown; }
    logf_("[OK] websocket connected");
    send_hello(&ws);

    state = ST_IDLE;
    t_state = now_ms();

    while (g_running) {
        int op = 0, len = 0;
        int r = ws_recv(&ws, &op, frame_buf, 8192, &len, 20);
        int do_stop = 0;

        if (r == 1) {
            if (op == WS_TEXT) {
                char type[32] = {0};
                frame_buf[len] = '\0';
                json_get_str((const char *)frame_buf, "type", type, sizeof(type));

                if (!strcmp(type, "hello")) {
                    int sr = json_get_int((const char *)frame_buf, "sample_rate", 24000);
                    char sid[40] = {0};
                    json_get_str((const char *)frame_buf, "session_id", sid, sizeof(sid));
                    logf_("[rx] hello session=%s down_rate=%d", sid, sr);
                    {
                        int err = 0;
                        if (g_dec) opus_decoder_destroy(g_dec);
                        g_dec = opus_decoder_create(sr, 1, &err);
                        if (err == OPUS_OK) { g_dec_rate = sr; logf_("[OK] decoder %d Hz", sr); }
                        else logf_("[ERR] decoder create %d", err);
                    }
                    session_ok = 1;
                    if (state == ST_CONNECTING) {   /* 重连后的新会话：重新待命 */
                        state = ST_IDLE;
                        g_sm_state = ST_IDLE;
                        t_state = now_ms();
                        logf_("[sm] session ready -> idle (gated)");
                    }
                } else if (!strcmp(type, "stt")) {
                    char text[256] = {0};
                    json_get_str((const char *)frame_buf, "text", text, sizeof(text));
                    if (text[0]) logf_("[STT] %s", text);
                } else if (!strcmp(type, "tts")) {
                    char st[24] = {0};
                    char text[200] = {0};
                    json_get_str((const char *)frame_buf, "state", st, sizeof(st));
                    json_get_str((const char *)frame_buf, "text", text, sizeof(text));
                    if (!strcmp(st, "start")) {
                        logf_("[TTS] start");
                        g_playing = 1;
                        g_tts_ended = 0;
                        g_tts_session++;              /* 播放线程感知并复位 */
                        g_down_ring.rd = g_down_ring.wr;  /* 丢弃上一轮残留 */
                        ds24k_reset(); ds16k_reset();     /* 滤波器历史同步清零 */
                        state = ST_SPEAK;
                        t_state = now_ms();
                    } else if (!strcmp(st, "sentence_start") && text[0]) {
                        logf_("[TTS] %s", text);
                    } else if (!strcmp(st, "stop")) {
                        logf_("[TTS] stop");
                        g_tts_ended = 1;
                    } else if (!strcmp(st, "abort")) {
                        /* 服务端 VAD 检测到用户语音（打断）。
                         * ring 清空 + 会话复位丢半成品段；当前已在播放的段
                         * 会播完（rmm 无中止接口，最长 480ms），pending 自然
                         * 释放；g_pb_abort 让 ST_SPEAK 跳过 1s tail 立即回 listen */
                        logf_("[TTS] abort (barge-in)");
                        g_tts_session++;                  /* th_play 感知复位 */
                        g_down_ring.rd = g_down_ring.wr;  /* 丢未编码数据 */
                        g_tts_ended = 1;
                        g_pb_abort = 1;
                    }
                } else if (!strcmp(type, "llm")) {
                    char text[200] = {0};
                    json_get_str((const char *)frame_buf, "text", text, sizeof(text));
                    if (text[0]) logf_("[LLM] %s", text);
                } else if (!strcmp(type, "mcp")) {
                    /* MCP 走 {"type":"mcp","payload":{...}}，payload 是对象。
                     * 直接在整帧里找字段（method/id/url/token/name/question），
                     * 这些键在本协议里唯一，不需要完整 JSON 解析器。 */
                    mcp_handle(&ws, (const char *)frame_buf);
                } else {
                    logf_("[rx] %s : %.200s", type, (char *)frame_buf);
                }
            } else if (op == WS_BIN && g_dec) {
                int n = opus_decode(g_dec, frame_buf, len, dec_pcm, 2880, 0);
                if (n > 0) {
                    short pcm8k[1440];
                    int m = 0;
                    if (g_dec_rate == 24000) {
                        m = ds24k_run(dec_pcm, n, pcm8k);
                    } else if (g_dec_rate == 16000) {
                        m = ds16k_run(dec_pcm, n, pcm8k);
                    } else if (g_dec_rate == 8000) {
                        m = n > 1440 ? 1440 : n;
                        memcpy(pcm8k, dec_pcm, (size_t)m * sizeof(short));
                    }
                    if (m > 0) ring_write(&g_down_ring, pcm8k, (unsigned)m);
                }
            } else if (op == WS_PING) {
                ws_send(&ws, WS_PONG, frame_buf, (size_t)len);
            } else if (op == WS_CLOSE) {
                logf_("[net] close frame");
                r = -1;                     /* 交给下面的重连，别把进程带走 */
            }
        }

        /* ---- 会话断了：进程内重连，绝不退出（docs/12） ---- */
        if (r == -1 || r == -3) {
            logf_("[net] %s -> reconnect", r == -1 ? "closed by peer" : "error");
            g_tx_enable = 0;
            g_playing = 0;
            g_pb_abort = 0;
            g_tts_ended = 1;
            g_tts_session++;                    /* 播放线程丢掉上一轮的段 */
            g_down_ring.rd = g_down_ring.wr;    /* 丢残留下行 PCM */
            g_up_ring.rd = g_up_ring.wr;        /* 丢残留上行 PCM */
            session_ok = 0;
            state = ST_CONNECTING;
            g_sm_state = ST_CONNECTING;
            t_state = now_ms();
            tls_close(&ws);
            if (net_reconnect(&ws) != 0) break; /* 只有 g_running 清 0 才真退 */
            continue;
        }

        /* ---- 状态机 ---- */
        {
            long long now = now_ms();

            switch (state) {
            case ST_IDLE:
                /* P3 唤醒门控（2026-11-01）：不再 1.2s 自动开监听。
                 * 等本地引擎在 IDLE 态命中“你好小智”（th_cap 置 g_wake_hit）
                 * 才开会话——否则服务端会把一切语音当指令（“讲个故事”直接讲）。
                 * 服务端主动推 TTS 走 rx 的 tts start -> ST_SPEAK，不经此门。 */
                if (session_ok && g_wake_hit) {
                    g_wake_hit = 0;
                    g_up_ring.rd = g_up_ring.wr;
                    g_vad_speech_frms = 0;
                    g_vad_silence_frms = 0;
                    g_vad_has_spoken = 0;
                    g_tx_enable = 1;
                    g_tx_start_ms = now;
                    send_listen(&ws, "start");
                    state = ST_LISTEN;
                    t_state = now;
                    listen_restarts = 0;
                    logf_("[sm] wake accepted -> listen");
                }
                break;

            case ST_LISTEN:
                if (g_vad_has_spoken && g_vad_silence_frms >= VAD_SILENCE_FRMS) {
                    logf_("[sm] end of speech (silence %d frames, rms=%d)",
                          g_vad_silence_frms, g_vad_rms);
                    listen_restarts = 0;
                    do_stop = 1;
                } else if (now - g_tx_start_ms > LISTEN_MAX_MS) {
                    if (!g_vad_has_spoken) {
                        /* 整段没人说话：丢弃缓冲、静默重开一轮。
                           绝不要发 stop —— 服务端会把底噪当成语音，
                           识别出"然后始"这类垃圾文本，白白触发一轮 LLM/TTS。
                           唤醒门控：连续 2 轮（约 30s）无人说话就回门控待机，
                           否则喊一次唤醒词后麦克风会永远开着。 */
                        listen_restarts++;
                        if (listen_restarts >= 2) {
                            logf_("[sm] %d listen rounds no speech -> gated idle",
                                  listen_restarts);
                            listen_restarts = 0;
                            g_tx_enable = 0;
                            g_up_ring.rd = g_up_ring.wr;
                            state = ST_IDLE;
                            t_state = now;
                            break;
                        }
                        logf_("[sm] listen idle-timeout (%d ms), no speech -> restart",
                              LISTEN_MAX_MS);
                        g_up_ring.rd = g_up_ring.wr;
                        g_vad_speech_frms = 0;
                        g_vad_silence_frms = 0;
                        g_tx_start_ms = now;
                        send_listen(&ws, "start");   /* 重置服务端的那一轮 */
                    } else {
                        logf_("[sm] listen timeout %d ms, had speech -> stop", LISTEN_MAX_MS);
                        do_stop = 1;
                    }
                }
                break;

            case ST_WAIT_TTS:
                if (now - t_state > 12000) {
                    logf_("[sm] wait tts timeout -> idle");
                    state = ST_IDLE;
                    t_state = now;
                }
                break;

            case ST_SPEAK:
                /* 本地自动打断已按用户指令移除（2026-10-31）——服务端不 VAD、
                   本地 VAD 被回声误触发、VQE AEC 未消回声，四轮实验详见
                   逆向记录 7.9-7.12。rx 的 tts abort 被动处理保留。 */
                /* 播完条件：tts stop/abort + ring 抽干 + 播放线程无待播段，
                   再留 1s 排空（ADEC 20 包队列 + AO 缓冲还在放）；服务端
                   abort 时跳过 tail 并跳过 idle 1.2s 等待 */
                if (g_tts_ended && ring_avail(&g_down_ring) < CAP_FRAME &&
                    !g_pb_busy && (g_pb_abort || now - g_pb_last_trig_ms > PB_TAIL_MS)) {
                    int aborted = g_pb_abort;
                    logf_(aborted ? "[sm] barge-in finished -> idle" :
                                    "[sm] playback finished -> idle");
                    g_playing = 0;
                    g_pb_abort = 0;
                    state = ST_IDLE;
                    t_state = now - (aborted ? 1201 : 0);
                } else if (now - t_state > 120000) {
                    logf_("[sm] speak timeout -> idle");
                    g_playing = 0;
                    g_pb_abort = 0;
                    state = ST_IDLE;
                    t_state = now;
                }
                break;

            default:
                break;
            }

            if (do_stop) {
                g_tx_enable = 0;
                send_listen(&ws, "stop");
                state = ST_WAIT_TTS;
                t_state = now;
            }

            g_sm_state = state;   /* 给采集线程的命中日志看：命中时设备在干什么 */
        }

        /* ---- LED 状态指示：黄=录音侧，蓝=播放侧 ---- */
        {
            static int last_lp = -1;
            led_pattern_t lp;
            switch (state) {
            case ST_CONNECTING: lp = LEDP_CONNECT; break;   /* 蓝快闪：连接中 */
            case ST_IDLE:       lp = LEDP_IDLE;    break;   /* 黄常亮：待命 */
            case ST_LISTEN:     lp = LEDP_LISTEN;  break;   /* 黄 2Hz：录音 */
            case ST_WAIT_TTS:   lp = LEDP_THINK;   break;   /* 黄 1Hz：思考 */
            case ST_SPEAK:      lp = LEDP_SPEAK;   break;   /* 蓝 2Hz：播放 */
            default:            lp = LEDP_OFF;     break;
            }
            if ((int)lp != last_lp) {
                logf_("[led] -> %s", led_pattern_name(lp));
                last_lp = (int)lp;
            }
            led_tick(lp, now_ms());
        }

        /* 推流：凑够一帧就编码发送 */
        if (g_tx_enable && ring_avail(&g_up_ring) >= NET_FRAME) {
            int n;
            ring_read(&g_up_ring, pcm960, NET_FRAME);
            n = opus_encode(g_enc, pcm960, NET_FRAME, opus_out, sizeof(opus_out));
            if (n > 0) ws_send(&ws, WS_BIN, opus_out, (size_t)n);
        }
    }

shutdown:
    g_running = 0;
    g_tx_enable = 0;
    usleep(200000);
    if (t_cap_ok) pthread_join(t_cap, NULL);
    if (t_play_ok) pthread_join(t_play, NULL);
    audio_close();
    led_alloff();
    led_close();
    tls_close(&ws);
    if (g_enc) opus_encoder_destroy(g_enc);
    if (g_dec) opus_decoder_destroy(g_dec);
    free(frame_buf);
    if (g_raw) { fclose(g_raw); g_raw = NULL; }
    logf_("========== EXIT ==========");
    if (g_log) fclose(g_log);
    return 0;
}
