/*
 * g726_check.c - host-side validation for g726.h (build with plain gcc).
 *
 * Usage:
 *   g726_check roundtrip              encode->decode SNR for both packings
 *   g726_check probe <file.g726> <out_msb.pcm> <out_lsb.pcm>
 *                                     decode file with both packings, print
 *                                     statistics per packing (the correct
 *                                     packing yields speech-like RMS/crest,
 *                                     the wrong one loud full-scale noise)
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "g726.h"

#define SR 8000

static int load_pcm16(const char *path, int16_t *buf, int maxn)
{
    FILE *f = fopen(path, "rb");
    int n = 0;
    if (!f) return -1;
    while (n < maxn) {
        unsigned char b[2];
        if (fread(b, 1, 2, f) != 2) break;
        buf[n++] = (int16_t)(b[0] | (b[1] << 8));
    }
    fclose(f);
    return n;
}

static void save_pcm16(const char *path, const int16_t *buf, int n)
{
    FILE *f = fopen(path, "wb");
    int i;
    for (i = 0; i < n; i++) {
        unsigned char b[2];
        b[0] = (unsigned char)(buf[i] & 0xff);
        b[1] = (unsigned char)((buf[i] >> 8) & 0xff);
        fwrite(b, 1, 2, f);
    }
    fclose(f);
}

static double snr_db(const int16_t *x, const int16_t *y, int n)
{
    double s = 0, e = 0;
    int i;
    for (i = 0; i < n; i++) {
        double d = (double)x[i] - y[i];
        s += (double)x[i] * x[i];
        e += d * d;
    }
    if (e <= 0) return 999.0;
    return 10.0 * log10(s / e);
}

static void stats(const char *tag, const int16_t *p, int n)
{
    double s = 0, peak = 0, zc = 0, dsum = 0;
    int i, clip = 0;
    for (i = 0; i < n; i++) {
        double a = p[i] < 0 ? -(double)p[i] : (double)p[i];
        s += (double)p[i] * p[i];
        if (a > peak) peak = a;
        if (a > 32000.0) clip++;
        if (i) {
            double d = p[i] - p[i-1];
            dsum += d < 0 ? -d : d;
            if ((p[i] >= 0) != (p[i-1] >= 0)) zc++;
        }
    }
    printf("  %-14s n=%d rms=%.0f peak=%.0f crest=%.1f clip=%d(%d%%) zc=%.1f%% meandelta=%.0f\n",
           tag, n, sqrt(s / (n ? n : 1)), peak,
           peak / (sqrt(s / (n ? n : 1)) + 1e-9),
           clip, n ? clip * 100 / n : 0,
           n ? zc * 100.0 / n : 0.0,
           n ? dsum / n : 0.0);
}

static void do_roundtrip(void)
{
    static int16_t pcm[8 * SR], dec[8 * SR];
    static uint8_t bits[8 * SR];
    int n = 0, i;
    double t;

    /* silence + tones + sweep + noise + silence */
    for (i = 0; i < SR / 2; i++) pcm[n++] = 0;
    for (i = 0; i < SR; i++)     pcm[n++] = (int16_t)(12000.0 * sin(2 * M_PI * 1000.0 * i / SR));
    for (i = 0; i < SR / 2; i++) pcm[n++] = 0;
    for (i = 0; i < SR; i++) {
        t = 300.0 + 2700.0 * i / SR;
        pcm[n++] = (int16_t)(9000.0 * sin(2 * M_PI * t * i / SR));
    }
    for (i = 0; i < SR / 2; i++)
        pcm[n++] = (int16_t)(8000.0 * (2.0 * ((i * 7919) % 101) / 101.0 - 1.0));
    for (i = 0; i < SR / 4; i++) pcm[n++] = 0;

    printf("roundtrip: %d samples (%.2f s @ 8 kHz)\n", n, n / 8000.0);
    for (i = 0; i < 4; i++) {
        G726Ctx e, d;
        int nb, nd;
        static const int rate[4] = { 2, 4, 4, 5 };
        static const int pack[4] = { 0, 0, 1, 0 };
        char tag[32];
        sprintf(tag, "%d-bit %s", rate[i], pack[i] ? "LSB" : "MSB");
        g726_init(&e, rate[i], pack[i]);
        g726_init(&d, rate[i], pack[i]);
        nb = g726_encode_stream(&e, pcm, n, bits);
        nd = g726_decode_stream(&d, bits, nb, dec);
        printf("%-12s bytes=%d samples=%d snr=%.1f dB\n", tag, nb, nd, snr_db(pcm, dec, n));
    }
}

static void do_probe(const char *file, const char *outm, const char *outl)
{
    static uint8_t bits[1 << 22];
    static int16_t dec[1 << 23];
    FILE *f = fopen(file, "rb");
    int nb, i;

    if (!f) { printf("open %s failed\n", file); return; }
    nb = (int)fread(bits, 1, sizeof(bits), f);
    fclose(f);
    printf("probe %s: %d bytes (%d x 80B packets, rem %d)\n",
           file, nb, nb / 80, nb % 80);
    printf("  (HiSilicon PT_G726 = 16 kbps: 2 bits/sample, MSB-first, 80B = 320 samples = 40 ms)\n");

    /* 0: 2-bit MSB (the HiSilicon format), 1: 2-bit LSB, 2: 4-bit MSB */
    for (i = 0; i < 3; i++) {
        static const int rate[3] = { 2, 2, 4 };
        static const int pack[3] = { 0, 1, 0 };
        G726Ctx d;
        int n;
        char tag[32], out[256];
        sprintf(tag, "%d-bit %s", rate[i], pack[i] ? "LSB" : "MSB");
        g726_init(&d, rate[i], pack[i]);
        n = g726_decode_stream(&d, bits, nb - nb % 80, dec);
        stats(tag, dec, n);
        if (i == 0)
            strcpy(out, outm);
        else if (i == 1)
            strcpy(out, outl);
        else
            continue;
        save_pcm16(out, dec, n);
        printf("    -> wrote %s\n", out);
    }
}

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "roundtrip"))
        do_roundtrip();
    else if (argc >= 5 && !strcmp(argv[1], "probe"))
        do_probe(argv[2], argv[3], argv[4]);
    else {
        printf("usage:\n  %s roundtrip\n  %s probe <file.g726> <out_msb.pcm> <out_lsb.pcm>\n",
               argv[0], argv[0]);
        return 1;
    }
    return 0;
}
