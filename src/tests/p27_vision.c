/*
 * p27_vision.c -- 设备侧识图链路验证器（独立进程，不改 p8_client）
 *
 * 两个动作：
 *   p27s snap <jpg路径>                     发 mq 0x1610 让 rmm 抓一张 JPEG
 *   p27s post <jpg路径> <url> <token> <问题> POST multipart 到 vision/explain，打印响应
 *
 * 为什么要单独做：把"设备能不能把图传上去"和"固件里 MCP 接得对不对"分开验，
 * 避免一次改两处、出错时分不清是协议还是网络。
 *
 * vision url/token 来自服务端 MCP initialize（每会话一个），临时验证时由
 * PC 侧 mcp_probe.py 拿一个活的传进来。
 *
 * 编译：
 *   TC=~/theplace/XiaoZhiCam/toolchains/arm-hisiv100nptl-linux/arm-hisiv100-linux/bin/arm-hisiv100-linux-uclibcgnueabi-gcc
 *   $TC -Os -mcpu=arm926ej-s -mfloat-abi=soft -o p27s p27_vision.c -lrt
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <netinet/in.h>

#define MQ_NAME    "/ipc_rmm"
#define CMD_SNAP   0x1610
#define BOUNDARY   "----XiaoZhiCamBoundary"

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---------------- 抓拍：复用 p26_snap.c 已验证的 mq 0x1610 ---------------- */

static int do_snap(const char *path)
{
    unsigned char msg[512];
    mqd_t mq;
    int len = (int)strlen(path);
    long long t0;
    struct stat st;

    if (len < 1 || len > 480) { fprintf(stderr, "path too long\n"); return 2; }
    unlink(path);

    memset(msg, 0, sizeof(msg));
    msg[8]  = (unsigned char)(CMD_SNAP & 0xff);
    msg[9]  = (unsigned char)((CMD_SNAP >> 8) & 0xff);
    msg[0xc] = (unsigned char)(len & 0xff);
    msg[0xd] = (unsigned char)((len >> 8) & 0xff);
    msg[0xe] = (unsigned char)((len >> 16) & 0xff);
    msg[0xf] = (unsigned char)((len >> 24) & 0xff);
    memcpy(msg + 0x10, path, (size_t)len);

    mq = mq_open(MQ_NAME, O_WRONLY | O_NONBLOCK);
    if (mq == (mqd_t)-1) { printf("mq_open failed: %s\n", strerror(errno)); return 1; }
    if (mq_send(mq, (char *)msg, (size_t)(0x10 + len), 0) != 0) {
        printf("mq_send failed: %s\n", strerror(errno));
        mq_close(mq);
        return 1;
    }
    mq_close(mq);

    t0 = now_ms();
    while (now_ms() - t0 < 5000) {
        if (stat(path, &st) == 0 && st.st_size > 0) {
            char tmp[520];
            struct stat stt;
            snprintf(tmp, sizeof(tmp), "%s.tmp", path);
            if (stat(tmp, &stt) != 0) {
                printf("snap OK %s size=%ld after %lld ms\n",
                       path, (long)st.st_size, now_ms() - t0);
                return 0;
            }
        }
        usleep(50 * 1000);
    }
    printf("snap TIMEOUT\n");
    return 1;
}

/* ---------------- HTTP POST multipart ---------------- */

static int send_all(int fd, const char *buf, int len)
{
    int off = 0;
    while (off < len) {
        int n = (int)send(fd, buf + off, (size_t)(len - off), 0);
        if (n <= 0) return -1;
        off += n;
    }
    return 0;
}

static int do_post(const char *jpg, const char *url, const char *token, const char *question)
{
    struct stat st;
    char host[128], path[256], req[1024], part[1024];
    char *p, *q;
    int fd, n, total_hdr = 0;
    long long content_len;
    struct addrinfo hints, *res = NULL, *ai;
    char resp[4096];
    int rlen = 0;
    FILE *fp;
    struct timeval tv;
    long long t0;

    if (stat(jpg, &st) != 0 || st.st_size <= 0) {
        printf("cannot stat %s: %s\n", jpg, strerror(errno));
        return 2;
    }

    /* 拆 url: http://host[:port]/path */
    if (strncmp(url, "http://", 7) != 0) { printf("only http:// supported\n"); return 2; }
    p = (char *)url + 7;
    q = strchr(p, '/');
    if (!q) { printf("bad url\n"); return 2; }
    if ((size_t)(q - p) >= sizeof(host)) { printf("host too long\n"); return 2; }
    memcpy(host, p, (size_t)(q - p));
    host[q - p] = '\0';
    snprintf(path, sizeof(path), "%s", q);
    { /* 去掉 :port */
        char *c = strchr(host, ':');
        if (c) *c = '\0';
    }

    /* 两段头 + 文件 + 尾，先算 Content-Length */
    {
        int h1 = snprintf(NULL, 0,
            "--%s\r\nContent-Disposition: form-data; name=\"question\"\r\n\r\n%s\r\n"
            "--%s\r\nContent-Disposition: form-data; name=\"file\"; filename=\"camera.jpg\"\r\n"
            "Content-Type: image/jpeg\r\n\r\n",
            BOUNDARY, question, BOUNDARY);
        int tail = snprintf(NULL, 0, "\r\n--%s--\r\n", BOUNDARY);
        if (h1 < 0 || h1 >= (int)sizeof(part)) { printf("question too long\n"); return 2; }
        snprintf(part, sizeof(part),
            "--%s\r\nContent-Disposition: form-data; name=\"question\"\r\n\r\n%s\r\n"
            "--%s\r\nContent-Disposition: form-data; name=\"file\"; filename=\"camera.jpg\"\r\n"
            "Content-Type: image/jpeg\r\n\r\n",
            BOUNDARY, question, BOUNDARY);
        content_len = (long long)h1 + (long long)st.st_size + tail;
    }

    n = snprintf(req, sizeof(req),
        "POST %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Device-Id: 00:11:22:33:44:55\r\n"
        "Client-Id: 00000000-0000-4000-8000-000000000000\r\n"
        "Authorization: Bearer %s\r\n"
        "Content-Type: multipart/form-data; boundary=%s\r\n"
        "Content-Length: %lld\r\n"
        "Connection: close\r\n\r\n",
        path, host, token, BOUNDARY, content_len);
    if (n <= 0 || n >= (int)sizeof(req)) { printf("header overflow\n"); return 2; }
    total_hdr = n;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, "80", &hints, &res) != 0) {
        printf("dns fail %s\n", host);
        return 3;
    }

    fd = -1;
    for (ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        tv.tv_sec = 20; tv.tv_usec = 0;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        t0 = now_ms();
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) { printf("connect fail\n"); return 3; }
    printf("connected, jpeg=%ld B content_length=%lld\n", (long)st.st_size, content_len);

    if (send_all(fd, req, total_hdr) != 0) { printf("send hdr fail\n"); close(fd); return 4; }
    if (send_all(fd, part, (int)strlen(part)) != 0) { printf("send part fail\n"); close(fd); return 4; }

    fp = fopen(jpg, "rb");
    if (!fp) { printf("open jpg fail\n"); close(fd); return 4; }
    {
        char buf[4096];
        size_t got;
        while ((got = fread(buf, 1, sizeof(buf), fp)) > 0) {
            if (send_all(fd, buf, (int)got) != 0) {
                printf("send body fail\n"); fclose(fp); close(fd); return 4;
            }
        }
    }
    fclose(fp);
    {
        char tail[128];
        int tl = snprintf(tail, sizeof(tail), "\r\n--%s--\r\n", BOUNDARY);
        if (send_all(fd, tail, tl) != 0) { printf("send tail fail\n"); close(fd); return 4; }
    }

    while (rlen < (int)sizeof(resp) - 1) {
        int r = (int)recv(fd, resp + rlen, (size_t)(sizeof(resp) - 1 - rlen), 0);
        if (r <= 0) break;
        rlen += r;
    }
    resp[rlen] = '\0';
    close(fd);

    printf("--- 响应 %d B，耗时 %lld ms ---\n", rlen, now_ms() - t0);
    {
        char *body = strstr(resp, "\r\n\r\n");
        printf("status: %.40s\n", resp);
        if (body) {
            body += 4;
            /* 只打印 text 字段，太长就截断 */
            char *t = strstr(body, "\"text\"");
            if (t) {
                t = strchr(t, ':');
                if (t) {
                    t++;
                    while (*t == ' ') t++;
                    if (*t == '"') {
                        t++;
                        char *e = strchr(t, '"');
                        if (e) *e = '\0';
                    }
                }
                printf("text: %s\n", t ? t : "(none)");
            } else {
                printf("body: %.300s\n", body);
            }
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "snap")) return do_snap(argv[2]);

    if (argc >= 5 && !strcmp(argv[1], "post")) {
        /* 问题可能带空格，把剩余参数拼起来 */
        char question[256] = {0};
        int i;
        for (i = 5; i < argc; i++) {
            if (i > 5) strncat(question, " ", sizeof(question) - strlen(question) - 1);
            strncat(question, argv[i], sizeof(question) - strlen(question) - 1);
        }
        if (!question[0]) snprintf(question, sizeof(question), "你看到了什么");
        return do_post(argv[2], argv[3], argv[4], question);
    }

    printf("usage:\n"
           "  p27s snap <jpg>\n"
           "  p27s post <jpg> <url> <token> [question...]\n");
    return 1;
}
