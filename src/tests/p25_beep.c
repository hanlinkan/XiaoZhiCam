/*
 * p25_beep.c - validate the full rmm beep playback chain with our own
 * user-space encoder (no MPP AENC, no touching rmm's audio channels):
 *
 *   PCM 8k mono -> G.726 16kbps (2 bits/sample, MSB-first, reverse-
 *   engineered from libVoiceEngine.so G726_decoder_unpack / iquant16)
 *   -> raw file of consecutive 80-byte packets (320 samples = 40 ms)
 *   -> symlink /home/forbidden.g726 -> <file>
 *   -> mq_send(/ipc_rmm, cmd=0x1614 at offset 8)
 *   -> rmm_msg_handle -> send_ao_beep(fopen the symlink, fread 80B,
 *      4-byte header {0,1,40,0}, HI_MPI_ADEC_SendStream, usleep 23ms)
 *
 * Modes:
 *   p25s gen [out]            build test signal (default /tmp/beep16.g726)
 *   p25s enc <in.pcm> <out>   encode raw 8kHz S16LE mono pcm -> g726 file
 *   p25s play <file>          symlink + mq 0x1614 trigger
 *   p25s wait                 poll /proc/umap/adec until counts go still
 */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <mqueue.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "g726.h"

#define PACKET_BYTES 80             /* 320 samples @ 2 bits = 40 ms */
#define LINK         "/home/forbidden.g726"
#define MQ_NAME      "/ipc_rmm"
#define MQ_CMD_SPEAK_FORBID 0x1614

static int gen_test(const char *out)
{
    /* three ascending beeps: 0.4 s tone + 0.2 s silence, x3 = 1.8 s */
    static int16_t pcm[14400];
    static uint8_t buf[14400 / 4];
    static const double freq[3] = { 1000.0, 1500.0, 2200.0 };
    G726Ctx c;
    int i, g, n = 0, nb;
    FILE *f;

    for (g = 0; g < 3; g++) {
        for (i = 0; i < 3200; i++)
            pcm[n++] = (int16_t)(12000.0 * sin(2.0 * M_PI * freq[g] * i / 8000.0));
        for (i = 0; i < 1600; i++)
            pcm[n++] = 0;
    }
    g726_init(&c, 2, G726_PACK_MSB);
    nb = g726_encode_stream(&c, pcm, n, buf);
    if (nb % PACKET_BYTES) {
        printf("gen: internal error, %d bytes not multiple of %d\n", nb, PACKET_BYTES);
        return 1;
    }
    f = fopen(out, "wb");
    if (!f) { perror(out); return 1; }
    fwrite(buf, 1, nb, f);
    fclose(f);
    printf("gen: %s = %d bytes = %d packets = %.2f s audio (2-bit MSB G726-16k)\n",
           out, nb, nb / PACKET_BYTES, n / 8000.0);
    return 0;
}

static int enc_pcm(const char *in, const char *out)
{
    static int16_t pcm[1 << 20];
    static uint8_t buf[(1 << 20) / 4];
    G726Ctx c;
    int n = 0, nb, nsamp;
    FILE *f;
    unsigned char b[2];

    f = fopen(in, "rb");
    if (!f) { perror(in); return 1; }
    while (n < (int)(sizeof(pcm) / sizeof(pcm[0])) &&
           fread(b, 1, 2, f) == 2)
        pcm[n++] = (int16_t)(b[0] | (b[1] << 8));
    fclose(f);

    /* pad to whole 320-sample packets with silence so the file is a
     * multiple of 80 bytes (send_ao_beep aborts on short reads) */
    nsamp = (n + 319) / 320 * 320;
    while (n < nsamp)
        pcm[n++] = 0;

    g726_init(&c, 2, G726_PACK_MSB);
    nb = g726_encode_stream(&c, pcm, n, buf);

    f = fopen(out, "wb");
    if (!f) { perror(out); return 1; }
    fwrite(buf, 1, nb, f);
    fclose(f);
    printf("enc: %s (%d samples = %.2f s) -> %s (%d bytes = %d packets)\n",
           in, n, n / 8000.0, out, nb, nb / PACKET_BYTES);
    return 0;
}

static int do_play(const char *file)
{
    unsigned char msg[16];
    mqd_t mq;
    struct mq_attr attr;

    unlink(LINK);
    if (symlink(file, LINK) != 0) { perror("symlink " LINK); return 1; }

    mq = mq_open(MQ_NAME, O_WRONLY | O_NONBLOCK);
    if (mq == (mqd_t)-1) { perror("mq_open " MQ_NAME); return 1; }
    if (mq_getattr(mq, &attr) == 0)
        printf("mq: maxmsg=%ld msgsize=%ld curmsgs=%ld\n",
               (long)attr.mq_maxmsg, (long)attr.mq_msgsize, (long)attr.mq_curmsgs);

    memset(msg, 0, sizeof(msg));
    msg[8] = (unsigned char)(MQ_CMD_SPEAK_FORBID & 0xff);        /* LE u16 */
    msg[9] = (unsigned char)(MQ_CMD_SPEAK_FORBID >> 8);
    if (mq_send(mq, (char *)msg, sizeof(msg), 0) != 0) {
        perror("mq_send");
        mq_close(mq);
        return 1;
    }
    mq_close(mq);
    printf("play: %s -> " LINK ", mq 0x1614 sent\n", file);
    return 0;
}

static int adec_stable(void)
{
    /* read /proc/umap/adec, hash its bytes; done when the hash stays
     * identical 4 times in a row (300 ms apart) */
    static char prev[64];
    char line[4096];
    int stable = 0, round = 0;
    unsigned long h, hp = 0;
    FILE *f;

    while (round < 200) {
        f = fopen("/proc/umap/adec", "r");
        if (!f) { perror("/proc/umap/adec"); return 1; }
        h = 5381;
        line[0] = '\0';
        while (fgets(line, sizeof(line), f)) {
            const char *p;
            for (p = line; *p; p++)
                h = h * 33 + (unsigned char)*p;
        }
        fclose(f);
        if (h == hp)
            stable++;
        else
            stable = 0;
        hp = h;
        round++;
        if (stable >= 4) {
            /* print final counter line */
            f = fopen("/proc/umap/adec", "r");
            if (f) {
                while (fgets(line, sizeof(line), f))
                    if (strstr(line, "g726") || strstr(line, "ChnId"))
                        printf("adec| %s", line);
                fclose(f);
            }
            printf("wait: adec stable after %d rounds (%.1f s)\n",
                   round, round * 0.3);
            return 0;
        }
        {
            struct timespec ts;
            ts.tv_sec = 0;
            ts.tv_nsec = 300 * 1000 * 1000;
            nanosleep(&ts, NULL);
        }
    }
    printf("wait: timed out after 60 s\n");
    return 2;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "gen"))
        return gen_test(argc >= 3 ? argv[2] : "/tmp/beep16.g726");
    if (argc >= 4 && !strcmp(argv[1], "enc"))
        return enc_pcm(argv[2], argv[3]);
    if (argc >= 3 && !strcmp(argv[1], "play"))
        return do_play(argv[2]);
    if (argc >= 2 && !strcmp(argv[1], "wait"))
        return adec_stable();
    printf("usage:\n"
           "  %s gen [out.g726]          build test beep file\n"
           "  %s enc <in.pcm> <out.g726> encode 8k s16le pcm\n"
           "  %s play <file.g726>        symlink + mq 0x1614\n"
           "  %s wait                    wait for adec counters still\n",
           argv[0], argv[0], argv[0], argv[0]);
    return 1;
}
