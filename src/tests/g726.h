#ifndef G726_H
#define G726_H
/*
 * Self-contained ITU-T G.726 ADPCM codec (encoder + decoder).
 * Faithful port of FFmpeg libavcodec/g726.c, itself a rendition of
 * ITU-T G.726 Section 4 "Computational Details".
 *
 * Target use on the YHS-113 camera: the original rmm beep path plays
 * raw 32 kbps G.726 (8 kHz, 4 bits/sample, 80-byte packets = 160
 * samples = 20 ms), fed to ADEC PlType "g726" in PACK mode.
 *
 * Bit packing order selectable (the HiSilicon ADEC expectation must be
 * confirmed by playing both variants through the device):
 *   G726_PACK_MSB : first code occupies the HIGH bits of byte 0
 *                   (FFmpeg codec name "g726", put_bits/get_bits)
 *   G726_PACK_LSB : first code occupies the LOW bits of byte 0
 *                   (FFmpeg codec name "g726le", put_bits_le/get_bits_le)
 */

#include <limits.h>
#include <stdint.h>
#include <string.h>

#define G726_PACK_MSB 0
#define G726_PACK_LSB 1

typedef struct { uint8_t sign, exp, mant; } g726_f11;

typedef struct {
    int code_size;               /* bits per sample: 2..5 (32 kbps = 4) */
    int pack;                    /* G726_PACK_MSB / G726_PACK_LSB */
    const int     *quant;        /* quantization table */
    const int16_t *iquant;       /* inverse quantization table */
    const int16_t *W;            /* adaptation table W */
    const uint8_t *F;            /* F table */

    g726_f11 sr[2];              /* prev. reconstructed samples */
    g726_f11 dq[6];              /* prev. difference */
    int a[2];                    /* second order predictor coeffs */
    int b[6];                    /* sixth order predictor coeffs */
    int pk[2];                   /* signs of prev. 2 sez + dq */

    int ap;                      /* scale factor control */
    int yu;                      /* fast scale factor */
    int yl;                      /* slow scale factor */
    int dms;                     /* short average magnitude */
    int dml;                     /* long average magnitude */
    int td;                      /* tone detect */

    int se;                      /* estimated signal, next iteration */
    int sez;                     /* estimated second order prediction */
    int y;                       /* quantizer scale, next iteration */
} G726Ctx;

/* ---------------- tables (verbatim from the reference) ---------------- */

static const int     g726_quant_tbl16[] =
        { 260, INT_MAX };
static const int16_t g726_iquant_tbl16[] =
        { 116, 365, 365, 116 };
static const int16_t g726_W_tbl16[] =
        { -22, 439, 439, -22 };
static const uint8_t g726_F_tbl16[] =
        { 0, 7, 7, 0 };

static const int     g726_quant_tbl24[] =
        { 7, 217, 330, INT_MAX };
static const int16_t g726_iquant_tbl24[] =
        { INT16_MIN, 135, 273, 373, 373, 273, 135, INT16_MIN };
static const int16_t g726_W_tbl24[] =
        { -4, 30, 137, 582, 582, 137, 30, -4 };
static const uint8_t g726_F_tbl24[] =
        { 0, 1, 2, 7, 7, 2, 1, 0 };

static const int     g726_quant_tbl32[] =
        { -125, 79, 177, 245, 299, 348, 399, INT_MAX };
static const int16_t g726_iquant_tbl32[] =
        { INT16_MIN,   4, 135, 213, 273, 323, 373, 425,
            425, 373, 323, 273, 213, 135,   4, INT16_MIN };
static const int16_t g726_W_tbl32[] =
        { -12, 18, 41, 64, 112, 198, 355, 1122,
        1122, 355, 198, 112, 64, 41, 18, -12 };
static const uint8_t g726_F_tbl32[] =
        { 0, 0, 0, 1, 1, 1, 3, 7, 7, 3, 1, 1, 1, 0, 0, 0 };

static const int     g726_quant_tbl40[] =
        { -122, -16, 67, 138, 197, 249, 297, 338,
          377, 412, 444, 474, 501, 527, 552, INT_MAX };
static const int16_t g726_iquant_tbl40[] =
        { INT16_MIN, -66, 28, 104, 169, 224, 274, 318,
          358, 395, 429, 459, 488, 514, 539, 566,
          566, 539, 514, 488, 459, 429, 395, 358,
          318, 274, 224, 169, 104, 28, -66, INT16_MIN };
static const int16_t g726_W_tbl40[] =
        { 14, 14, 24, 39, 40, 41, 58, 100,
        141, 179, 219, 280, 358, 440, 529, 696,
        696, 529, 440, 358, 280, 219, 179, 141,
        100, 58, 41, 40, 39, 24, 14, 14 };
static const uint8_t g726_F_tbl40[] =
        { 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 2, 3, 4, 5, 6, 6,
          6, 6, 5, 4, 3, 2, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0 };

/* ---------------- helpers (FFmpeg semantics, bit-exact) ---------------- */

static int g726_clip(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* av_clip_intp2: clip to [-(1<<p), (1<<p)-1] */
static int g726_clip_intp2(int a, int p)
{
    if ((a + ((1 << p) - 1)) & ~((2 << p) - 1))
        return (a >> 31) ^ ((1 << p) - 1);
    return a;
}

/* av_log2_16bit: floor(log2(v)), 0 for v == 0 (v assumed <= 0xffff) */
static int g726_log2_16(unsigned v)
{
    int n = 0;
    while (v >>= 1)
        n++;
    return n;
}

/* G.726 11-bit float conversion */
static g726_f11 *g726_i2f(int i, g726_f11 *f)
{
    f->sign = (i < 0);
    if (f->sign)
        i = -i;
    f->exp = (uint8_t)(g726_log2_16((unsigned)i) + !!i);
    f->mant = (uint8_t)(i ? ((i << 6) >> f->exp) : (1 << 5));
    return f;
}

static int16_t g726_mult(g726_f11 *f1, g726_f11 *f2)
{
    int exp = f1->exp + f2->exp;
    int res = (((f1->mant * f2->mant) + 0x30) >> 4);
    res = exp > 19 ? res << (exp - 19) : res >> (19 - exp);
    return (int16_t)((f1->sign ^ f2->sign) ? -res : res);
}

static int g726_sgn(int value)
{
    return (value < 0) ? -1 : 1;
}

/* -------------- paragraph 4.2.2: adaptive quantizer -------------- */
static uint8_t g726_quant(G726Ctx *c, int d)
{
    int sign = 0, exp, i = 0, dln;

    if (d < 0) {
        sign = 1;
        d = -d;
    }
    exp = g726_log2_16((unsigned)d);
    dln = ((exp << 7) + (((d << 7) >> exp) & 0x7f)) - (c->y >> 2);

    while (c->quant[i] < INT_MAX && c->quant[i] < dln)
        ++i;

    if (sign)
        i = ~i;
    if (c->code_size != 2 && i == 0)
        i = 0xff;

    return (uint8_t)i;
}

/* -------------- paragraph 4.2.3: inverse adaptive quantizer -------------- */
static int16_t g726_invq(G726Ctx *c, int i)
{
    int dql, dex, dqt;

    dql = c->iquant[i] + (c->y >> 2);
    dex = (dql >> 7) & 0xf;         /* 4-bit exponent */
    dqt = (1 << 7) + (dql & 0x7f);  /* log2 -> linear */
    return (int16_t)((dql < 0) ? 0 : ((dqt << dex) >> 7));
}

/* core state update; shared by encoder and decoder.
 * I: the code; returns the reconstructed output sample. */
static int16_t g726_step(G726Ctx *c, int I)
{
    int dq, re_signal, pk0, fa1, i, tr, ylint, ylfrac, thr2, al, dq0;
    g726_f11 f;
    int I_sig = I >> (c->code_size - 1);

    dq = g726_invq(c, I);

    /* transition detect */
    ylint  = c->yl >> 15;
    ylfrac = (c->yl >> 10) & 0x1f;
    thr2   = (ylint > 9) ? (0x1f << 10) : ((0x20 + ylfrac) << ylint);
    tr = (c->td == 1 && dq > ((3 * thr2) >> 2));

    if (I_sig)  /* get the sign */
        dq = -dq;
    re_signal = (int16_t)(c->se + dq);

    /* update second order predictor coefficients A2 and A1 */
    pk0 = (c->sez + dq) ? g726_sgn(c->sez + dq) : 0;
    dq0 = dq ? g726_sgn(dq) : 0;
    if (tr) {
        c->a[0] = 0;
        c->a[1] = 0;
        for (i = 0; i < 6; i++)
            c->b[i] = 0;
    } else {
        /* this is a bit crazy, but it really is +255 not +256 */
        fa1 = g726_clip_intp2((-c->a[0] * c->pk[0] * pk0) >> 5, 8);

        c->a[1] += 128 * pk0 * c->pk[1] + fa1 - (c->a[1] >> 7);
        c->a[1] = g726_clip(c->a[1], -12288, 12288);
        c->a[0] += 64 * 3 * pk0 * c->pk[0] - (c->a[0] >> 8);
        c->a[0] = g726_clip(c->a[0], -(15360 - c->a[1]), 15360 - c->a[1]);

        for (i = 0; i < 6; i++)
            c->b[i] += 128 * dq0 * g726_sgn(-c->dq[i].sign) - (c->b[i] >> 8);
    }

    /* update Dq and Sr and Pk */
    c->pk[1] = c->pk[0];
    c->pk[0] = pk0 ? pk0 : 1;
    c->sr[1] = c->sr[0];
    g726_i2f(re_signal, &c->sr[0]);
    for (i = 5; i > 0; i--)
        c->dq[i] = c->dq[i - 1];
    g726_i2f(dq, &c->dq[0]);
    c->dq[0].sign = (uint8_t)I_sig;  /* isn't it crazy ?!?! */

    c->td = c->a[1] < -11776;

    /* update Ap */
    c->dms += (c->F[I] << 4) + ((-c->dms) >> 5);
    c->dml += (c->F[I] << 4) + ((-c->dml) >> 7);
    if (tr) {
        c->ap = 256;
    } else {
        c->ap += (-c->ap) >> 4;
        if (c->y <= 1535 || c->td ||
            abs((c->dms << 2) - c->dml) >= (c->dml >> 3))
            c->ap += 0x20;
    }

    /* update Yu and Yl */
    c->yu = g726_clip(c->y + c->W[I] + ((-c->y) >> 5), 544, 5120);
    c->yl += c->yu + ((-c->yl) >> 6);

    /* next iteration for Y */
    al = (c->ap >= 256) ? (1 << 6) : (c->ap >> 2);
    c->y = (c->yl + (c->yu - (c->yl >> 6)) * al) >> 6;

    /* next iteration for SE and SEZ */
    c->se = 0;
    for (i = 0; i < 6; i++)
        c->se += g726_mult(g726_i2f(c->b[i] >> 2, &f), &c->dq[i]);
    c->sez = c->se >> 1;
    for (i = 0; i < 2; i++)
        c->se += g726_mult(g726_i2f(c->a[i] >> 2, &f), &c->sr[i]);
    c->se >>= 1;

    return (int16_t)g726_clip(re_signal * 4, -0xffff, 0xffff);
}

static void g726_reset(G726Ctx *c)
{
    int i;

    /* NOTE: must not wipe c->quant/iquant/W/F (assigned by g726_init) */
    for (i = 0; i < 2; i++) {
        c->sr[i].sign = 0;
        c->sr[i].exp  = 0;
        c->sr[i].mant = 1 << 5;
        c->pk[i] = 1;
    }
    for (i = 0; i < 6; i++) {
        c->dq[i].sign = 0;
        c->dq[i].exp  = 0;
        c->dq[i].mant = 1 << 5;
    }
    memset(c->a, 0, sizeof(c->a));
    memset(c->b, 0, sizeof(c->b));
    c->ap  = 0;
    c->dms = 0;
    c->dml = 0;
    c->td  = 0;
    c->se  = 0;
    c->sez = 0;
    c->yu = 544;
    c->yl = 34816;
    c->y  = 544;
}

static void g726_init(G726Ctx *c, int code_size, int pack)
{
    static const struct {
        const int     *quant;
        const int16_t *iquant;
        const int16_t *W;
        const uint8_t *F;
    } pool[4] = {
        { g726_quant_tbl16, g726_iquant_tbl16, g726_W_tbl16, g726_F_tbl16 },
        { g726_quant_tbl24, g726_iquant_tbl24, g726_W_tbl24, g726_F_tbl24 },
        { g726_quant_tbl32, g726_iquant_tbl32, g726_W_tbl32, g726_F_tbl32 },
        { g726_quant_tbl40, g726_iquant_tbl40, g726_W_tbl40, g726_F_tbl40 },
    };

    if (code_size < 2) code_size = 2;
    if (code_size > 5) code_size = 5;
    memset(c, 0, sizeof(*c));
    c->code_size = code_size;
    c->pack      = pack;
    c->quant     = pool[code_size - 2].quant;
    c->iquant    = pool[code_size - 2].iquant;
    c->W         = pool[code_size - 2].W;
    c->F         = pool[code_size - 2].F;
    g726_reset(c);
}

/* encoder: one sample in, one code out (state advances like the decoder) */
static uint8_t g726_encode_sample(G726Ctx *c, int16_t sig)
{
    int i = g726_quant(c, sig / 4 - c->se) & ((1 << c->code_size) - 1);
    g726_step(c, i);
    return (uint8_t)i;
}

/* decoder: one code in, one sample out */
static int16_t g726_decode_sample(G726Ctx *c, int I)
{
    return g726_step(c, I & ((1 << c->code_size) - 1));
}

/* ---------------- bitstream packing ---------------- */

typedef struct {
    uint8_t *buf;
    int len;         /* capacity in bytes */
    int bitpos;      /* total bits written/read */
    int pack;
} G726BS;

static void g726_bs_init(G726BS *bs, uint8_t *buf, int cap, int pack)
{
    bs->buf = buf;
    bs->len = cap;
    bs->bitpos = 0;
    bs->pack = pack;
}

static void g726_bs_put(G726BS *bs, int code, int nbits)
{
    int i;

    for (i = nbits - 1; i >= 0; i--) {
        int bit    = (code >> i) & 1;
        int byte_i = bs->bitpos >> 3;
        int bit_in = bs->bitpos & 7;
        int shift  = (bs->pack == G726_PACK_MSB) ? (7 - bit_in) : bit_in;

        if (byte_i < bs->len)
            bs->buf[byte_i] |= (uint8_t)(bit << shift);
        bs->bitpos++;
    }
}

static int g726_bs_get(G726BS *bs, int nbits)
{
    int v = 0, i;

    for (i = 0; i < nbits; i++) {
        int byte_i = bs->bitpos >> 3;
        int bit_in = bs->bitpos & 7;
        int shift  = (bs->pack == G726_PACK_MSB) ? (7 - bit_in) : bit_in;

        if (byte_i < bs->len)
            v = (v << 1) | ((bs->buf[byte_i] >> shift) & 1);
        else
            v <<= 1;
        bs->bitpos++;
    }
    return v;
}

/* ---------------- bulk conveniences ---------------- */

/* Encode PCM16 mono @8kHz to a raw G.726 bitstream (pack from ctx).
 * Returns bytes written; out needs (n*code_size+7)/8 bytes, zeroed here. */
static int g726_encode_stream(G726Ctx *c, const int16_t *pcm, int n, uint8_t *out)
{
    G726BS bs;
    int i, nbytes = (n * c->code_size + 7) / 8;

    memset(out, 0, nbytes);
    g726_bs_init(&bs, out, nbytes, c->pack);
    for (i = 0; i < n; i++)
        g726_bs_put(&bs, g726_encode_sample(c, pcm[i]), c->code_size);
    return nbytes;
}

/* Decode a raw G.726 bitstream (pack from ctx) to PCM16 mono @8kHz.
 * Returns samples written. */
static int g726_decode_stream(G726Ctx *c, const uint8_t *in, int nbytes, int16_t *pcm)
{
    G726BS bs;
    int i, n = nbytes * 8 / c->code_size;

    g726_bs_init(&bs, (uint8_t *)in, nbytes, c->pack);
    for (i = 0; i < n; i++)
        pcm[i] = g726_decode_sample(c, g726_bs_get(&bs, c->code_size));
    return n;
}

#endif /* G726_H */
