/*
 * p6_tls_probe.c  --  设备端 TLS + HTTPS 可行性验证
 *
 * 目的：在真实硬件上回答三个问题
 *   1. ARM926（无 FPU/无加密单元）上 mbedTLS 握手要多久？
 *   2. 握手期间内存峰值多少？6.4MB 可用堆够不够？
 *   3. 能否正确验证 api.tenclass.net 的证书链并完成 OTA 请求？
 *
 * 这三项决定后续网络层能否成立。任何一项失败，方案都要改。
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
#include "mbedtls/error.h"
#include "mbedtls/version.h"

#define HOST      "api.tenclass.net"
#define PORT      "443"
#define OTA_PATH  "/xiaozhi/ota/"
#define DEVICE_ID "00:11:22:33:44:55"
#define CLIENT_ID "00000000-0000-4000-8000-000000000000"

/* DigiCert Global Root G2 —— 服务端证书链的根 */
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

/* 读 /proc/self/status 里的内存水位 */
static int mem_stat(const char *key, long *val)
{
    FILE *f = fopen("/proc/self/status", "r");
    char line[128];
    size_t klen = strlen(key);
    if (!f) return -1;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, key, klen) == 0) {
            *val = strtol(line + klen, NULL, 10);
            fclose(f);
            return 0;
        }
    }
    fclose(f);
    return -1;
}

static void print_mem(const char *tag)
{
    long v = -1, h = -1;
    mem_stat("VmRSS:", &v);
    mem_stat("VmHWM:", &h);
    logf_("[MEM] %-22s RSS=%ld kB  HWM=%ld kB", tag, v, h);
}

static void print_mbedtls_err(const char *what, int ret)
{
    char buf[160];
    mbedtls_strerror(ret, buf, sizeof(buf));
    logf_("[ERR] %s: -0x%04X %s", what, (unsigned)-ret, buf);
}

int main(int argc, char **argv)
{
    int ret = 0, exit_code = 0, ssl_ready = 0;
    long long t0, t1, t_dns = 0, t_tcp = 0, t_tls = 0, t_http = 0;

    mbedtls_net_context server_fd;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt cacert;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;

    char *req = NULL;
    char *resp = NULL;
    const char *pers = "xzc_tls";
    size_t req_len, total = 0;
    static char rbuf[4096];

    (void)argc; (void)argv;

    g_log = fopen("/tmp/hd1/tls_probe.log", "w");
    if (!g_log) g_log = fopen("/tmp/tls_probe.log", "w");

    logf_("=================================================");
    logf_(" XiaoZhiCam TLS Probe   mbedTLS %s", MBEDTLS_VERSION_STRING);
    logf_("=================================================");
    print_mem("startup");

    mbedtls_net_init(&server_fd);
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&conf);
    mbedtls_x509_crt_init(&cacert);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);

    /* ---- 1. 随机数发生器 ---- */
    t0 = now_ms();
    ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                                (const unsigned char *)pers, strlen(pers));
    if (ret != 0) { print_mbedtls_err("ctr_drbg_seed", ret); exit_code = 10; goto out; }
    logf_("[ 1] ctr_drbg_seed            OK   %lld ms", now_ms() - t0);

    /* ---- 2. 解析根证书 ---- */
    t0 = now_ms();
    ret = mbedtls_x509_crt_parse(&cacert, (const unsigned char *)CA_PEM, sizeof(CA_PEM));
    if (ret != 0) { print_mbedtls_err("crt_parse", ret); exit_code = 11; goto out; }
    logf_("[ 2] x509_crt_parse           OK   %lld ms  (1 root)", now_ms() - t0);

    /* ---- 3. TCP 连接（mbedtls_net 内含 DNS 解析）---- */
    t0 = now_ms();
    ret = mbedtls_net_connect(&server_fd, HOST, PORT, MBEDTLS_NET_PROTO_TCP);
    t_dns = now_ms() - t0;
    if (ret != 0) { print_mbedtls_err("net_connect(DNS+TCP)", ret); exit_code = 12; goto out; }
    logf_("[ 3] DNS + TCP connect        OK   %lld ms  fd=%d", t_dns, server_fd.fd);
    t_tcp = t_dns;

    {
        struct timeval tv;
        tv.tv_sec  = 20; tv.tv_usec = 0;
        setsockopt(server_fd.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(server_fd.fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        { int one = 1; setsockopt(server_fd.fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)); }
    }
    print_mem("after tcp");

    /* ---- 4. SSL 配置 ---- */
    ret = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT,
                                      MBEDTLS_SSL_TRANSPORT_STREAM,
                                      MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) { print_mbedtls_err("ssl_config_defaults", ret); exit_code = 13; goto out; }

    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&conf, &cacert, NULL);
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &ctr_drbg);
    /* 放宽握手超时，ARM926 上 ECDHE 较慢 */
    mbedtls_ssl_conf_handshake_timeout(&conf, 10000, 60000);

    ret = mbedtls_ssl_setup(&ssl, &conf);
    if (ret != 0) { print_mbedtls_err("ssl_setup", ret); exit_code = 14; goto out; }
    ssl_ready = 1;
    ret = mbedtls_ssl_set_hostname(&ssl, HOST);
    if (ret != 0) { print_mbedtls_err("ssl_set_hostname", ret); exit_code = 15; goto out; }
    mbedtls_ssl_set_bio(&ssl, &server_fd, mbedtls_net_send, mbedtls_net_recv, NULL);

    /* ---- 5. TLS 握手（关键指标）---- */
    t0 = now_ms();
    while ((ret = mbedtls_ssl_handshake(&ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            print_mbedtls_err("ssl_handshake", ret);
            exit_code = 16; goto out;
        }
    }
    t_tls = now_ms() - t0;
    logf_("[ 5] TLS handshake            OK   %lld ms", t_tls);
    logf_("      version=%s  cipher=%s",
          mbedtls_ssl_get_version(&ssl), mbedtls_ssl_get_ciphersuite(&ssl));

    {
        unsigned int vflags = 0;
        uint32_t vr = mbedtls_ssl_get_verify_result(&ssl);
        if (vr != 0) {
            char vbuf[256];
            mbedtls_x509_crt_verify_info(vbuf, sizeof(vbuf), "  ! ", vr);
            logf_("[ 5b] cert verify FAILED: %s", vbuf);
            vflags = 1;
        } else {
            logf_("[ 5b] cert verify            OK   (chain trusted)");
        }
        (void)vflags;
    }
    print_mem("after handshake");

    /* ---- 6. 构造 OTA 请求 ---- */
    {
        const char *body =
            "{\"application\":{\"name\":\"xiaozhi-cam\",\"version\":\"1.0.0\"},"
            "\"board\":{\"type\":\"xzc-hi3518\"},"
            "\"device\":{\"chip_model\":\"hi3518\"}}";
        size_t body_len = strlen(body);
        size_t cap = 1024 + body_len;

        req = (char *)malloc(cap);
        if (!req) { logf_("[ERR] malloc req failed"); exit_code = 17; goto out; }
        req_len = (size_t)snprintf(req, cap,
            "POST " OTA_PATH " HTTP/1.1\r\n"
            "Host: " HOST "\r\n"
            "Device-Id: " DEVICE_ID "\r\n"
            "Client-Id: " CLIENT_ID "\r\n"
            "Activation-Version: 1\r\n"
            "Content-Type: application/json\r\n"
            "User-Agent: xiaozhi-cam/1.0\r\n"
            "Accept: */*\r\n"
            "Content-Length: %u\r\n"
            "Connection: close\r\n"
            "\r\n"
            "%s",
            (unsigned)body_len, body);
    }

    /* ---- 7. 发送 ---- */
    t0 = now_ms();
    {
        size_t off = 0;
        while (off < req_len) {
            ret = mbedtls_ssl_write(&ssl, (const unsigned char *)req + off, req_len - off);
            if (ret > 0) { off += (size_t)ret; continue; }
            if (ret == MBEDTLS_ERR_SSL_WANT_WRITE || ret == MBEDTLS_ERR_SSL_WANT_READ) continue;
            print_mbedtls_err("ssl_write", ret);
            exit_code = 18; goto out;
        }
    }
    logf_("[ 7] HTTP request sent        OK   %u bytes  %lld ms", (unsigned)req_len, now_ms() - t0);

    /* ---- 8. 读取响应 ---- */
    t0 = now_ms();
    resp = (char *)calloc(1, 8192);
    if (!resp) { logf_("[ERR] malloc resp failed"); exit_code = 19; goto out; }
    while (total < 8191) {
        ret = mbedtls_ssl_read(&ssl, (unsigned char *)rbuf, sizeof(rbuf) - 1);
        if (ret > 0) {
            size_t n = (size_t)ret;
            if (total + n > 8191) n = 8191 - total;
            memcpy(resp + total, rbuf, n);
            total += n;
            continue;
        }
        if (ret == 0) break;                                   /* 对端关闭 */
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) break;
        if (ret == MBEDTLS_ERR_NET_RECV_FAILED) break;         /* 超时 */
        print_mbedtls_err("ssl_read", ret);
        break;
    }
    t_http = now_ms() - t0;
    resp[total] = '\0';
    logf_("[ 8] HTTP response           OK   %u bytes  %lld ms", (unsigned)total, t_http);
    print_mem("after http");

    /* ---- 9. 打印响应 ---- */
    logf_("---------------- RESPONSE ----------------");
    {
        /* 只打印头部和 body，控制刷屏 */
        char *p = strstr(resp, "\r\n\r\n");
        logf_("%.*s", (int)((p ? (size_t)(p - resp) : total) < 700 ? (p ? (size_t)(p - resp) : total) : 700), resp);
        if (p) logf_("----\n%.1200s", p + 4);
    }
    logf_("------------------------------------------");

    logf_("========== SUMMARY ==========");
    logf_(" DNS+TCP : %lld ms", t_tcp);
    logf_(" TLS     : %lld ms", t_tls);
    logf_(" HTTP    : %lld ms", t_http);
    logf_(" TOTAL   : %lld ms", t_tcp + t_tls + t_http);

out:
    if (ssl_ready) mbedtls_ssl_close_notify(&ssl);
    mbedtls_net_free(&server_fd);
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    mbedtls_x509_crt_free(&cacert);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
    free(req);
    free(resp);

    logf_("========== EXIT = %d ==========", exit_code);
    if (g_log) fclose(g_log);
    return exit_code;
}
