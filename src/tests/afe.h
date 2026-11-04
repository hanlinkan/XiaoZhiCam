/*
 * afe.h -- 音频前端（Acoustic Front End），全 int32 定点、无堆分配、无浮点
 *
 * 处理链：
 *      [同步平均去工频]  ->  6阶巴特沃斯高通 200Hz (3 段)
 *                        ->  6阶巴特沃斯低通 3000Hz (3 段)
 *                        ->  噪声门 -> AGC
 *
 * ===== 设计依据：全部来自本机实测 =====
 *
 * 1) acodec：麦克风接 LINEIN（不是 MICIN！MICIN 是空引脚，切过去等于接空气）。
 *    模拟 PGA 保持出厂 31 会饱和（实测直流 31816，正向余量只剩 951），
 *    降到 20 后直流 10600、余量充足。原始信号 RMS≈3965，max 6728。
 *    **绝不能用原始 RMS 判有没有声音。**
 *
 * 2) 干扰的性质（本机实测，连续 11.8 秒）：
 *      自相关 lag=160 样本 (50.0Hz)  r = +0.9912
 *      自相关 lag= 80 样本 (100Hz)   r = -0.9949   <-- 负相关，说明以奇次谐波为主
 *      自相关 lag=159 / 161          r ≈  0.00     <-- 周期精确是 160，不是 159 或 161
 *    频谱上最强的是 50Hz 的奇次谐波，一路延伸到 1.3kHz：
 *        150 / 250 / 350 / 450 / 550 / 650 / 750 / 850 / 950 / 1050 / 1150Hz
 *    其中 650.4Hz 峰值比谱中位数高 35.8dB，750.5Hz 高 33.8dB —— 正好压在语音 F1/F2 上。
 *    这不是正弦交流声，是**50Hz 周期的尖峰串**（开关电源类），方波成分很重。
 *
 * 3) 为什么不用陷波：
 *    干扰谐波密布在 150~1300Hz，每 50Hz 一根，正好覆盖语音最主要的频段。
 *    十几个陷波会在语音上挖十几个洞，伤敌一千自损八百。
 *    改用**同步平均减法**（synchronous averaging）：
 *        模板 tmpl[160] 记录干扰在一个工频周期内的波形，
 *        y[n] = x[n] - tmpl[n mod 160]，模板以极快速率跟随。
 *    干扰是严格周期的，会被模板完整学到并整体减掉；
 *    语音不是 50Hz 周期的，只会在模板里留下一层被平均掉的微小分量。
 *
 *    实测（K=2，5 秒数据 + 合成元音 725/1125Hz，2~4.5 秒）：
 *        干扰抑制           28.7 dB
 *        725Hz 语音保留    114.3%（+1.2dB，完全没有损失，反而因为干扰残差被去掉而略增）
 *        1125Hz 语音保留   114.3%
 *    K=3 时：抑制 25.6dB、保留 106.7%；K=4/5 抑制明显变差。这里取 K=2。
 *
 * 4) 高通 200Hz 去 DC 与低频漂移；低通 3000Hz 去近奈奎斯特噪声。
 *    实测：6 阶高通 200Hz 在 50Hz 处 -73dB；6 阶低通 3000Hz 在 3900Hz 处 -124dB。
 *    （去工频主要靠同步平均，高通只是兜底，所以只要 6 阶，不必 8 阶。）
 *
 * 5) AI 会周期性整帧输出数字零（断流），必须显式排除。
 *
 * ===== 定点与性能约定（重要）=====
 * 系数 Q13（8192 = 1.0）；状态变量 int32。
 * 最大系数 16229，× 32768 = 5.3e8，远小于 2^31，**纯 int32 乘法不会溢出**。
 * 绝不要在这里引入 long long：ARM926EJ-S 没有 64 位乘法指令，
 * GCC 会生成 __aeabi_lmul 软调用；10 段时把单核压满，实测触发过看门狗复位。
 * 实测本链（6 段 + 同步平均）在设备上约 1.6ms/帧，占单核 3% 左右。
 *
 * 能量统计：样本先 >>4 再平方，320 个累加最大 1.34e9 < 2^31；
 * 最终 rms 再 <<4 还原量纲。最小分辨率 16 LSB。
 */
#ifndef AFE_H
#define AFE_H

#include <string.h>

#define AFE_MAX_FRAME   512
#define AFE_NHP         3          /* 高通 6 阶 = 3 个二阶节 */
#define AFE_NLP         3          /* 低通 6 阶 */
#define AFE_NSEC        (AFE_NHP + AFE_NLP)   /* = 6 */

#define AFE_QSHIFT      13
#define AFE_QONE        (1 << AFE_QSHIFT)   /* 8192 */

/* 工频周期：8000 / 50 = 160 样本（实测自相关确认就是 160） */
#define AFE_PERIOD      160
#define AFE_DEHUM_SHIFT 2       /* 模板跟随速率 1/4；实测 K=2 抑制 28.7dB 且语音无损 */

typedef struct { int b0, b1, b2, a1, a2; } afe_bq_t;

/* 6 阶巴特沃斯高通 @200Hz（fs=8000） */
static const afe_bq_t AFE_HPF[AFE_NHP] = {
    {  7073, -14146,   7073, -14058,   6041 },   /* Q=0.51764 */
    {  7331, -14661,   7331, -14571,   6560 },   /* Q=0.70711 */
    {  7825, -15650,   7825, -15553,   7554 },   /* Q=1.93185 */
};
/* 6 阶巴特沃斯低通 @3000Hz */
static const afe_bq_t AFE_LPF[AFE_NLP] = {
    {  4155,   8309,   4155,   6884,   1543 },   /* Q=0.51764 */
    {  4662,   9323,   4662,   7723,   2731 },   /* Q=0.70711 */
    {  5911,  11821,   5911,   9793,   5657 },   /* Q=1.93185 */
};

/* AGC / 噪声门 / VAD */
#define AFE_AGC_TARGET_PEAK  9000
#define AFE_AGC_MIN_Q10      256     /* 0.25x */
#define AFE_AGC_MAX_Q10      24576   /* 24x */
#define AFE_GATE_OPEN_Q10    1024
#define AFE_GATE_CLOSE_Q10   128     /* -18dB */
#define AFE_GATE_STEP_SHIFT  2
#define AFE_NF_INIT          200
#define AFE_NF_MIN           20
#define AFE_NF_MAX           20000
#define AFE_NF_DOWN_SHIFT    3      /* 变安静时快速下调（/8） */
#define AFE_NF_UP_SHIFT      7      /* 变吵时缓慢上调（/128，约 3~10s 收敛） */
#define AFE_VAD_MULT_NUM     200    /* rms/noise > 2.00 即 +6dB */
#define AFE_VAD_MULT_DEN     100
#define AFE_SPEECH_FRMS      3
#define AFE_SILENCE_FRMS     12
#define AFE_WARMUP_FRMS      6

typedef struct {
    int z1[AFE_NSEC], z2[AFE_NSEC];
    int tmpl[AFE_PERIOD];          /* 工频周期波形模板 */
    int phase;
    int noise_rms, agc_q10, gate_q10;
    int speech_frms, silence_frms, in_speech;
    int warmup;
    int last_raw_rms, last_out_rms, last_peak, zero_frame;
    int dehum_enable;              /* 1=开同步平均去工频（默认）；0=旁路，调试用 */
    short tmp[AFE_MAX_FRAME];
} afe_t;

static void afe_reset(afe_t *a)
{
    memset(a, 0, sizeof(*a));
    a->noise_rms = AFE_NF_INIT;
    a->agc_q10   = 2048;
    a->gate_q10  = AFE_GATE_OPEN_Q10;
    a->warmup    = AFE_WARMUP_FRMS;
    a->dehum_enable = 1;
}

static inline int afe_clamp16(int v)
{
    if (v >  32767) return  32767;
    if (v < -32768) return -32768;
    return v;
}

/* 同步平均去工频：模板记录一个 50Hz 周期内的波形并整体减掉。
   干扰是严格周期的（自相关 0.9912），语音不是，所以对语音几乎无损。 */
static inline int afe_dehum(afe_t *a, int x)
{
    int p = a->phase;
    int d = x - a->tmpl[p];
    a->tmpl[p] += d >> AFE_DEHUM_SHIFT;
    if (++a->phase >= AFE_PERIOD) a->phase = 0;
    return d;
}

/* 直接 II 型转置二阶节，系数 Q13，纯 int32 */
static inline int afe_bq(const afe_bq_t *c, int *z1, int *z2, int x)
{
    int y = ((c->b0 * x) >> AFE_QSHIFT) + *z1;
    *z1 = ((c->b1 * x) >> AFE_QSHIFT) - ((c->a1 * y) >> AFE_QSHIFT) + *z2;
    *z2 = ((c->b2 * x) >> AFE_QSHIFT) - ((c->a2 * y) >> AFE_QSHIFT);
    return y;
}

static inline int afe_filter(afe_t *a, int x)
{
    int s = 0;
    x = afe_bq(&AFE_HPF[0], &a->z1[s], &a->z2[s], x); s++;
    x = afe_bq(&AFE_HPF[1], &a->z1[s], &a->z2[s], x); s++;
    x = afe_bq(&AFE_HPF[2], &a->z1[s], &a->z2[s], x); s++;
    x = afe_bq(&AFE_LPF[0], &a->z1[s], &a->z2[s], x); s++;
    x = afe_bq(&AFE_LPF[1], &a->z1[s], &a->z2[s], x); s++;
    x = afe_bq(&AFE_LPF[2], &a->z1[s], &a->z2[s], x);
    return afe_clamp16(x);
}

/* int32 整数开方（逐位法，无除法、无 64 位） */
static inline int afe_isqrt(unsigned int x)
{
    unsigned int r = 0, b = 1u << 30;
    while (b > x) b >>= 2;
    while (b) {
        if (x >= r + b) { x -= r + b; r = (r >> 1) + b; }
        else            { r >>= 1; }
        b >>= 2;
    }
    return (int)r;
}

/* 处理一帧；返回 1 表示本帧判定为"有声" */
static int afe_process(afe_t *a, const short *in, short *out, int n)
{
    unsigned int sq = 0;
    int i, rms, pk = 0, is_speech = 0, nonzero = 0;

    if (n <= 0) return 0;
    if (n > AFE_MAX_FRAME) n = AFE_MAX_FRAME;

    for (i = 0; i < n; i++) {
        int v = a->dehum_enable ? afe_dehum(a, in[i]) : in[i];
        int t;
        v = afe_filter(a, v);
        a->tmp[i] = (short)v;
        t = v >> 4;                        /* 防溢出：320 × 2047² < 2^31 */
        sq += (unsigned int)(t * t);
        if (v) nonzero = 1;
        if (v < 0) { if (-v > pk) pk = -v; }
        else if (v > pk) pk = v;
    }
    rms = afe_isqrt(sq / (unsigned int)n) << 4;
    a->last_raw_rms = rms;
    a->last_peak    = pk;
    a->zero_frame   = nonzero ? 0 : 1;

    if (a->zero_frame) {                   /* AI 断流产生的全零帧 */
        memset(out, 0, (size_t)n * sizeof(short));
        a->last_out_rms = 0;
        return 0;
    }
    if (a->warmup > 0) {                   /* 模板/滤波器建立期不做判定 */
        a->warmup--;
        memset(out, 0, (size_t)n * sizeof(short));
        a->last_out_rms = 0;
        return 0;
    }

    /* 噪声底：快降慢升。
       必须给最小步长 1，否则 (rms-noise)>>8 在差值 <256 时恒为 0，
       噪声底永远爬不上去（实测卡在 123，导致 300 多的真实底噪被一直判成语音）。 */
    {
        int d = rms - a->noise_rms;
        if (d < 0) {
            int step = (-d) >> AFE_NF_DOWN_SHIFT;
            if (step < 1) step = 1;
            a->noise_rms -= step;
        } else if (d > 0) {
            int step = d >> AFE_NF_UP_SHIFT;
            if (step < 1) step = 1;
            a->noise_rms += step;
        }
    }
    if (a->noise_rms < AFE_NF_MIN) a->noise_rms = AFE_NF_MIN;
    if (a->noise_rms > AFE_NF_MAX) a->noise_rms = AFE_NF_MAX;

    /* VAD：高于噪声底 6dB */
    is_speech = (rms * AFE_VAD_MULT_DEN > a->noise_rms * AFE_VAD_MULT_NUM) ? 1 : 0;
    if (is_speech) {
        if (a->speech_frms < 10000) a->speech_frms++;
        a->silence_frms = 0;
        if (a->speech_frms >= AFE_SPEECH_FRMS) a->in_speech = 1;
    } else {
        if (a->silence_frms < 10000) a->silence_frms++;
        a->speech_frms = 0;
        if (a->silence_frms >= AFE_SILENCE_FRMS) a->in_speech = 0;
    }

    /* AGC：只在有声帧收敛，按峰值定目标 */
    if (is_speech && pk > 0) {
        int want = (AFE_AGC_TARGET_PEAK << 10) / pk;
        if (want < AFE_AGC_MIN_Q10) want = AFE_AGC_MIN_Q10;
        if (want > AFE_AGC_MAX_Q10) want = AFE_AGC_MAX_Q10;
        a->agc_q10 += (want - a->agc_q10) >> 2;
        if (a->agc_q10 < AFE_AGC_MIN_Q10) a->agc_q10 = AFE_AGC_MIN_Q10;
        if (a->agc_q10 > AFE_AGC_MAX_Q10) a->agc_q10 = AFE_AGC_MAX_Q10;
    } else {
        /* 静音段让 AGC 缓慢回退（约 2.5s 时间常数），
           否则一次大声后增益会一直粘在高位，把底噪持续放大。 */
        a->agc_q10 -= (a->agc_q10 - AFE_AGC_MIN_Q10) >> 6;
        if (a->agc_q10 < AFE_AGC_MIN_Q10) a->agc_q10 = AFE_AGC_MIN_Q10;
    }

    /* 噪声门：平滑到目标，避免咔哒声 */
    {
        int target = is_speech ? AFE_GATE_OPEN_Q10 : AFE_GATE_CLOSE_Q10;
        a->gate_q10 += (target - a->gate_q10) >> AFE_GATE_STEP_SHIFT;
        if (a->gate_q10 < 1) a->gate_q10 = 1;
    }

    sq = 0;
    for (i = 0; i < n; i++) {
        int s = (a->tmp[i] * a->agc_q10) >> 10;
        s = (s * a->gate_q10) >> 10;
        s = afe_clamp16(s);
        out[i] = (short)s;
        {
            int t = s >> 4;
            sq += (unsigned int)(t * t);
        }
    }
    a->last_out_rms = afe_isqrt(sq / (unsigned int)n) << 4;
    return is_speech;
}

#endif /* AFE_H */
