/*
 * p7_ws_probe.c  --  WebSocket + 小智协议握手验证
 *
 * 在 p6 证明 TLS 可行（432ms / 700KB）之后，验证下一层：
 *   1. WebSocket 握手（含 Authorization / Device-Id / Protocol-Version）
 *   2. 发送 hello，接收 server hello
 *   3. 保持连接并观察服务端主动下发的消息（心跳、session 状态）
 *
 * WebSocket 帧编解码按 RFC6455 手写：客户端发出的帧必须 mask，
 * 服务端发来的帧不 mask。不做 permessage-deflate（服务器未要求）。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/base64.h"
#include "mbedtls/error.h"
#include "mbedtls/version.h"

#define HOST       "api.tenclass.net"
#define PORT       "443"
#define WS_PATH    "/xiaozhi/v1/"
#define DEVICE_ID  "00:11:22:33:44:55"
#define CLIENT_ID  "00000000-0000-4000-8000-000000000000"
#define TOKEN      "test-token"

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

static FILE *g_log;

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void logf_(const char *fmt, ...)
{
    va_list ap;
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
}

static void dump_mem(const char *tag)
{
    FILE *f = fopen("/proc/self/status", "r");
    char line[128];
    long rss = -1, hwm = -1;
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "VmRSS:", 6))  rss = strtol(line + 6, NULL, 10);
        if (!strncmp(line, "VmHWM:", 6))  hwm = strtol(line + 6, NULL, 10);
    }
    fclose(f);
    logf_("[MEM] %-24s RSS=%ld kB  HWM=%ld kB", tag, rss, hwm);
}

/* ============ mbedTLS 连接封装 ============ */

typedef struct {
    mbedtls_net_context      net;
    mbedtls_ssl_context      ssl;
    mbedtls_ssl_config       conf;
    mbedtls_x509_crt         ca;
    mbedtls_entropy_context  ent;
    mbedtls_ctr_drbg_context drbg;
    int ready;
} tls_conn;

/* ssl_read 可能一次拿不到想要的字节数，这里保证读满 */
static int tls_read_exact(tls_conn *c, unsigned char *buf, size_t need)
{
    size_t got = 0;
    while (got < need) {
        int r = mbedtls_ssl_read(&c->ssl, buf + got, need - got);
        if (r > 0) { got += (size_t)r; continue; }
        if (r == 0) return -1;                       /* 对端关闭 */
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return -1;
        if (r == MBEDTLS_ERR_NET_RECV_FAILED) return -2;
        return -3;
    }
    return 0;
}

static int tls_write_all(tls_conn *c, const unsigned char *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        int r = mbedtls_ssl_write(&c->ssl, buf + off, len - off);
        if (r > 0) { off += (size_t)r; continue; }
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        return -1;
    }
    return 0;
}

static int tls_open(tls_conn *c, const char *host, const char *port, int timeout_sec)
{
    int ret;
    const char *pers = "xzc_ws";

    memset(c, 0, sizeof(*c));
    mbedtls_net_init(&c->net);
    mbedtls_ssl_init(&c->ssl);
    mbedtls_ssl_config_init(&c->conf);
    mbedtls_x509_crt_init(&c->ca);
    mbedtls_entropy_init(&c->ent);
    mbedtls_ctr_drbg_init(&c->drbg);

    if ((ret = mbedtls_ctr_drbg_seed(&c->drbg, mbedtls_entropy_func, &c->ent,
                                     (const unsigned char *)pers, strlen(pers))) != 0)
        return -1;
    if ((ret = mbedtls_x509_crt_parse(&c->ca, (const unsigned char *)CA_PEM, sizeof(CA_PEM))) != 0)
        return -2;
    if ((ret = mbedtls_net_connect(&c->net, host, port, MBEDTLS_NET_PROTO_TCP)) != 0)
        return -3;

    {
        struct timeval tv;
        tv.tv_sec = timeout_sec; tv.tv_usec = 0;
        setsockopt(c->net.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(c->net.fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        { int one = 1; setsockopt(c->net.fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)); }
    }

    if ((ret = mbedtls_ssl_config_defaults(&c->conf, MBEDTLS_SSL_IS_CLIENT,
                                           MBEDTLS_SSL_TRANSPORT_STREAM,
                                           MBEDTLS_SSL_PRESET_DEFAULT)) != 0)
        return -4;
    mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&c->conf, &c->ca, NULL);
    mbedtls_ssl_conf_rng(&c->conf, mbedtls_ctr_drbg_random, &c->drbg);
    mbedtls_ssl_conf_handshake_timeout(&c->conf, 10000, 60000);

    if ((ret = mbedtls_ssl_setup(&c->ssl, &c->conf)) != 0) return -5;
    if ((ret = mbedtls_ssl_set_hostname(&c->ssl, host)) != 0) return -6;
    mbedtls_ssl_set_bio(&c->ssl, &c->net, mbedtls_net_send, mbedtls_net_recv, NULL);

    while ((ret = mbedtls_ssl_handshake(&c->ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE)
            return -7;
    }
    if (mbedtls_ssl_get_verify_result(&c->ssl) != 0) return -8;

    c->ready = 1;
    return 0;
}

static void tls_close(tls_conn *c)
{
    if (c->ready) mbedtls_ssl_close_notify(&c->ssl);
    mbedtls_net_free(&c->net);
    mbedtls_ssl_free(&c->ssl);
    mbedtls_ssl_config_free(&c->conf);
    mbedtls_x509_crt_free(&c->ca);
    mbedtls_ctr_drbg_free(&c->drbg);
    mbedtls_entropy_free(&c->ent);
}

/* ============ WebSocket 帧层 ============ */

enum { WS_CONT = 0x0, WS_TEXT = 0x1, WS_BIN = 0x2, WS_CLOSE = 0x8, WS_PING = 0x9, WS_PONG = 0xA };

static int ws_handshake(tls_conn *c, const char *host, const char *path, const char *token)
{
    unsigned char nonce[16], key[32];
    size_t keylen = 0;
    char req[768];
    int n, ret;
    unsigned char ch;
    char resp[512];
    size_t rlen = 0;

    if ((ret = mbedtls_ctr_drbg_random(&c->drbg, nonce, sizeof(nonce))) != 0) return -1;
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
        "Device-Id: " DEVICE_ID "\r\n"
        "Client-Id: " CLIENT_ID "\r\n"
        "Protocol-Version: 1\r\n"
        "User-Agent: xiaozhi-cam/1.0\r\n"
        "\r\n",
        path, host, (char *)key, token);

    if (tls_write_all(c, (const unsigned char *)req, (size_t)n) != 0) return -3;

    /* 逐字节读到 \r\n\r\n */
    while (rlen < sizeof(resp) - 1) {
        if (tls_read_exact(c, &ch, 1) != 0) return -4;
        resp[rlen++] = (char)ch;
        if (rlen >= 4 && !memcmp(resp + rlen - 4, "\r\n\r\n", 4)) break;
    }
    resp[rlen] = '\0';

    if (!strstr(resp, "101")) {
        logf_("[WS] handshake response:\n%.400s", resp);
        return -5;
    }
    return 0;
}

static int ws_send_frame(tls_conn *c, int opcode, const unsigned char *payload, size_t len)
{
    unsigned char hdr[14];
    size_t hlen = 0;
    unsigned char mask[4];
    unsigned char *body;
    size_t i;

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

    if (mbedtls_ctr_drbg_random(&c->drbg, mask, 4) != 0) return -1;
    memcpy(hdr + hlen, mask, 4);
    hlen += 4;

    body = (unsigned char *)malloc(len ? len : 1);
    if (!body) return -2;
    for (i = 0; i < len; i++) body[i] = payload[i] ^ mask[i & 3];

    if (tls_write_all(c, hdr, hlen) != 0) { free(body); return -3; }
    if (len && tls_write_all(c, body, len) != 0) { free(body); return -4; }
    free(body);
    return 0;
}

/* 接收一帧。返回 1=有帧 0=超时 -1=连接关闭 -2=错误 */
static int ws_recv_frame(tls_conn *c, int *opcode, unsigned char *buf, size_t cap, size_t *out_len)
{
    unsigned char h[2], m[4];
    size_t len, i;
    int masked;

    if (tls_read_exact(c, h, 2) != 0) return -1;

    *opcode = h[0] & 0x0F;
    masked  = (h[1] & 0x80) != 0;
    len     = (size_t)(h[1] & 0x7F);

    if (len == 126) {
        unsigned char e[2];
        if (tls_read_exact(c, e, 2) != 0) return -1;
        len = ((size_t)e[0] << 8) | e[1];
    } else if (len == 127) {
        unsigned char e[8];
        if (tls_read_exact(c, e, 8) != 0) return -1;
        len = ((size_t)e[0] << 56) | ((size_t)e[1] << 48) | ((size_t)e[2] << 40) |
              ((size_t)e[3] << 32) | ((size_t)e[4] << 24) | ((size_t)e[5] << 16) |
              ((size_t)e[6] << 8)  |  (size_t)e[7];
    }

    if (masked) {
        if (tls_read_exact(c, m, 4) != 0) return -1;
    }
    if (len > cap) {
        /* 超长帧：读掉丢弃，避免流错位 */
        unsigned char tmp[64];
        size_t left = len;
        while (left) {
            size_t chunk = left > sizeof(tmp) ? sizeof(tmp) : left;
            if (tls_read_exact(c, tmp, chunk) != 0) return -1;
            left -= chunk;
        }
        *out_len = 0;
        return 2;
    }
    if (len && tls_read_exact(c, buf, len) != 0) return -1;
    if (masked) for (i = 0; i < len; i++) buf[i] ^= m[i & 3];

    *out_len = len;
    return 1;
}

/* ============ 主流程 ============ */

int main(void)
{
    tls_conn c;
    int ret, exit_code = 0;
    long long t0;

    g_log = fopen("/tmp/hd1/ws_probe.log", "w");
    if (!g_log) g_log = fopen("/tmp/ws_probe.log", "w");

    logf_("=================================================");
    logf_(" XiaoZhiCam WebSocket Probe   mbedTLS %s", MBEDTLS_VERSION_STRING);
    logf_("=================================================");
    dump_mem("startup");

    t0 = now_ms();
    ret = tls_open(&c, HOST, PORT, 25);
    if (ret != 0) {
        logf_("[ERR] tls_open failed at step %d", -ret);
        exit_code = 20;
        goto cleanup;
    }
    logf_("[1] TLS connect          OK   %lld ms", now_ms() - t0);

    t0 = now_ms();
    ret = ws_handshake(&c, HOST, WS_PATH, TOKEN);
    if (ret != 0) {
        logf_("[ERR] ws_handshake failed code=%d", ret);
        exit_code = 21;
        goto cleanup;
    }
    logf_("[2] WS handshake         OK   %lld ms", now_ms() - t0);
    dump_mem("after ws handshake");

    /* ---- 发送 hello ---- */
    {
        const char *hello =
            "{\"type\":\"hello\",\"version\":1,"
            "\"features\":{\"mcp\":false,\"aec\":true},"
            "\"transport\":\"websocket\","
            "\"audio_params\":{\"format\":\"opus\",\"sample_rate\":16000,"
            "\"channels\":1,\"frame_duration\":60}}";
        t0 = now_ms();
        ret = ws_send_frame(&c, WS_TEXT, (const unsigned char *)hello, strlen(hello));
        if (ret != 0) { logf_("[ERR] send hello failed %d", ret); exit_code = 22; goto cleanup; }
        logf_("[3] hello sent           OK   %u bytes  %lld ms", (unsigned)strlen(hello), now_ms() - t0);
    }

    /* ---- 收消息 ---- */
    {
        unsigned char *buf = (unsigned char *)malloc(16384);
        if (!buf) { exit_code = 23; goto cleanup; }

        for (int round = 0; round < 40; round++) {
            int op;
            size_t got = 0;
            t0 = now_ms();
            ret = ws_recv_frame(&c, &op, buf, 16383, &got);
            if (ret == -1) { logf_("[4] connection closed by peer (round %d)", round); break; }
            if (ret == -2 || ret == -3) { logf_("[4] recv error %d", ret); break; }

            if (op == WS_TEXT) {
                buf[got] = '\0';
                logf_("[RX %5lldms] TEXT  %u bytes: %.800s", now_ms() - t0, (unsigned)got, (char *)buf);
            } else if (op == WS_BIN) {
                logf_("[RX %5lldms] BIN   %u bytes  head=%02X%02X%02X%02X",
                      now_ms() - t0, (unsigned)got,
                      got > 0 ? buf[0] : 0, got > 1 ? buf[1] : 0,
                      got > 2 ? buf[2] : 0, got > 3 ? buf[3] : 0);
            } else if (op == WS_PING) {
                logf_("[RX] PING %u bytes -> PONG", (unsigned)got);
                ws_send_frame(&c, WS_PONG, buf, got);
            } else if (op == WS_CLOSE) {
                logf_("[RX] CLOSE frame, %u bytes", (unsigned)got);
                break;
            } else {
                logf_("[RX] opcode=0x%X len=%u", op, (unsigned)got);
            }
        }
        free(buf);
    }
    dump_mem("after session");

cleanup:
    tls_close(&c);
    logf_("========== EXIT = %d ==========", exit_code);
    if (g_log) fclose(g_log);
    return exit_code;
}
