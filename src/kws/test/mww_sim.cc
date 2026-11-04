/*
 * 主机端对拍工具：mel 前端 + mww 网络 vs Python/TFLite 参考。
 *
 * 用法：
 *   ./mww_sim feat  <in16k.wav> <out.feat>     # 只跑 mel 前端，落盘 T*40 个 int8
 *   ./mww_sim prob  <in.feat>   <out.prob>     # 只跑网络，落盘 T/3 个 uint8
 *   ./mww_sim all   <in16k.wav> <prefix>       # 两步都跑，prefix.feat / prefix.prob
 *   ./mww_sim cmp   <a.bin> <b.bin> [limit]    # 逐字节比较，打印最大偏差
 *
 * 为什么要落到文件再比：特征和概率都由 Python 侧独立产生（pymicro-features +
 * tflite），C++ 侧只读文件，避免"两边都错但错得一样"。
 *
 * 编译（开发机，注意要带上 third_party/microfrontend 的源码）：
 *   g++ -O2 -std=c++11 -I firmware/port/include -I firmware/port/third_party/microfrontend \
 *       -I <该目录>/lib -o build/mww_sim \
 *       firmware/port/test/mww_sim.cc firmware/port/src/xz_mel.cc firmware/port/src/xz_mww.cc \
 *       <该目录>/lib/fft.cc ... （整个 lib 目录，kiss_fft.cc 由 kiss_fft_int16.cc 直接 include）
 */
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <vector>

#include "xz_mel.h"
#include "xz_mww.h"

namespace {

bool ReadFile(const char* path, std::vector<unsigned char>* out) {
    FILE* f = fopen(path, "rb");
    if (f == nullptr) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out->resize((size_t)n);
    size_t got = fread(out->data(), 1, (size_t)n, f);
    fclose(f);
    return got == (size_t)n;
}

bool WriteFile(const char* path, const void* data, size_t n) {
    FILE* f = fopen(path, "wb");
    if (f == nullptr) return false;
    size_t w = fwrite(data, 1, n, f);
    fclose(f);
    return w == n;
}

/* 极简 WAV：只接受 16 kHz / 16 bit / 单声道（参考数据就是这种） */
bool ReadWav16k(const char* path, std::vector<int16_t>* pcm) {
    std::vector<unsigned char> b;
    if (!ReadFile(path, &b)) return false;
    if (b.size() < 44 || memcmp(b.data(), "RIFF", 4) != 0) return false;

    size_t pos = 12;
    while (pos + 8 <= b.size()) {
        const char* id = (const char*)b.data() + pos;
        uint32_t sz = 0;
        memcpy(&sz, b.data() + pos + 4, 4);
        if (memcmp(id, "data", 4) == 0) {
            size_t n = sz / 2;
            pcm->resize(n);
            memcpy(pcm->data(), b.data() + pos + 8, n * 2);
            return true;
        }
        pos += 8 + sz + (sz & 1);
    }
    return false;
}

int CmdFeat(const char* wav, const char* out) {
    std::vector<int16_t> pcm;
    if (!ReadWav16k(wav, &pcm)) {
        fprintf(stderr, "读 WAV 失败: %s\n", wav);
        return 2;
    }
    if (!xz_mel::Init()) {
        fprintf(stderr, "mel 初始化失败\n");
        return 3;
    }
    std::vector<int8_t> feats;
    int8_t f40[40];
    for (size_t i = 0; i + 160 <= pcm.size(); i += 160) {
        if (xz_mel::Process10ms(&pcm[i], f40)) {
            feats.insert(feats.end(), f40, f40 + 40);
        }
    }
    if (!WriteFile(out, feats.data(), feats.size())) return 4;
    printf("feat: %zu 帧 -> %s\n", feats.size() / 40, out);
    return 0;
}

int CmdStream(const char* wav, const char* out, int reset_frames) {
    std::vector<int16_t> pcm;
    if (!ReadWav16k(wav, &pcm)) {
        fprintf(stderr, "读 WAV 失败: %s\n", wav);
        return 2;
    }
    if (!xz_mel::Init()) {
        fprintf(stderr, "mel 初始化失败\n");
        return 3;
    }
    xz_mww::Reset();
    std::vector<unsigned char> probs;
    int8_t f40[40];
    int frame = 0;
    for (size_t i = 0; i + 160 <= pcm.size(); i += 160, frame++) {
        /* 每 reset_frames 个 10ms 帧重置一次前端（含 PCAN），网络历史保留。
         * reset_frames=0 表示不重置（设备当前的连续运行方式）。 */
        if (reset_frames > 0 && frame > 0 && (frame % reset_frames) == 0) {
            xz_mel::Reset();
        }
        if (!xz_mel::Process10ms(&pcm[i], f40)) continue;
        const int p = xz_mww::Feed(f40);
        if (p >= 0) probs.push_back((unsigned char)p);
    }
    if (!WriteFile(out, probs.data(), probs.size())) return 4;
    printf("stream: %zu 帧概率 -> %s（输入 %d 帧）\n", probs.size(), out, frame);
    return 0;
}

/* 安静时重置 PCAN：模拟固件里"g_vad_last_rel < 3 dB 且距上次重置 > 1.5 s"的策略。
 * 阈值用输入 RMS（-46 dBFS 以下算安静），只是近似，真正判决还是固件的 VAD。 */
int CmdStreamQuiet(const char* wav, const char* out, int quiet_frames) {
    std::vector<int16_t> pcm;
    if (!ReadWav16k(wav, &pcm)) {
        fprintf(stderr, "读 WAV 失败: %s\n", wav);
        return 2;
    }
    if (!xz_mel::Init()) {
        fprintf(stderr, "mel 初始化失败\n");
        return 3;
    }
    xz_mww::Reset();
    std::vector<unsigned char> probs;
    int8_t f40[40];
    int frame = 0;
    int since_reset = 0;
    int resets = 0;
    for (size_t i = 0; i + 160 <= pcm.size(); i += 160, frame++) {
        double acc = 0;
        for (int k = 0; k < 160; k++) acc += (double)pcm[i + k] * pcm[i + k];
        const double rms = std::sqrt(acc / 160.0);
        if (since_reset >= quiet_frames && rms < 400.0) {   /* 约 -38 dBFS */
            xz_mel::Reset();
            since_reset = 0;
            resets++;
        }
        since_reset++;
        if (!xz_mel::Process10ms(&pcm[i], f40)) continue;
        const int p = xz_mww::Feed(f40);
        if (p >= 0) probs.push_back((unsigned char)p);
    }
    if (!WriteFile(out, probs.data(), probs.size())) return 4;
    printf("streamq: %zu 帧概率, 安静重置 %d 次 -> %s\n", probs.size(), resets, out);
    return 0;
}

int CmdProb(const char* feat, const char* out, int reset_frames) {
    std::vector<unsigned char> b;
    if (!ReadFile(feat, &b)) {
        fprintf(stderr, "读特征失败: %s\n", feat);
        return 2;
    }
    if (b.size() % 40 != 0) {
        fprintf(stderr, "特征长度不是 40 的整数倍: %zu\n", b.size());
        return 3;
    }
    xz_mww::Reset();
    std::vector<unsigned char> probs;
    const int frames = (int)(b.size() / 40);
    for (int i = 0; i < frames; i++) {
        /* reset_frames > 0：模拟"每 N 帧重置一次前端/网络"，用于验证
         * 长时连续流上 PCAN 漂移的影响（见 mww-引擎设计.md §7）。 */
        if (reset_frames > 0 && i > 0 && (i % reset_frames) == 0) {
            xz_mel::Reset();   /* 只重置前端（PCAN），保留网络历史 */
        }
        int p = xz_mww::Feed((const int8_t*)b.data() + i * 40);
        if (p >= 0) probs.push_back((unsigned char)p);
    }
    if (!WriteFile(out, probs.data(), probs.size())) return 4;
    printf("prob: %zu 个 -> %s（特征 %d 帧）\n", probs.size(), out, frames);
    return 0;
}

int CmdCmp(const char* a, const char* b, int limit) {
    std::vector<unsigned char> x, y;
    if (!ReadFile(a, &x) || !ReadFile(b, &y)) {
        fprintf(stderr, "读文件失败\n");
        return 2;
    }
    if (x.size() != y.size()) {
        printf("长度不同: %zu vs %zu\n", x.size(), y.size());
        return 3;
    }
    int maxd = 0, at = -1, ndiff = 0;
    for (size_t i = 0; i < x.size(); i++) {
        int d = (int)x[i] - (int)y[i];
        if (d < 0) d = -d;
        if (d > 0) ndiff++;
        if (d > maxd) {
            maxd = d;
            at = (int)i;
        }
    }
    printf("比较 %zu 项: 不同 %d 项, 最大偏差 %d (第 %d 项), 判据 <= %d -> %s\n",
           x.size(), ndiff, maxd, at, limit, maxd <= limit ? "PASS" : "FAIL");
    return maxd <= limit ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "用法: %s feat|prob|all|cmp ...\n", argv[0]);
        return 1;
    }
    const char* cmd = argv[1];
    if (strcmp(cmd, "feat") == 0 && argc >= 4) return CmdFeat(argv[2], argv[3]);
    if (strcmp(cmd, "prob") == 0 && argc >= 4) {
        return CmdProb(argv[2], argv[3], argc >= 5 ? atoi(argv[4]) : 0);
    }
    if (strcmp(cmd, "streamq") == 0 && argc >= 4) {
        return CmdStreamQuiet(argv[2], argv[3], argc >= 5 ? atoi(argv[4]) : 150);
    }
    if (strcmp(cmd, "stream") == 0 && argc >= 4) {
        return CmdStream(argv[2], argv[3], argc >= 5 ? atoi(argv[4]) : 0);
    }
    if (strcmp(cmd, "cmp") == 0 && argc >= 4) {
        return CmdCmp(argv[2], argv[3], argc >= 5 ? atoi(argv[4]) : 0);
    }
    if (strcmp(cmd, "all") == 0 && argc >= 4) {
        char f[512], p[512];
        snprintf(f, sizeof(f), "%s.feat", argv[3]);
        snprintf(p, sizeof(p), "%s.prob", argv[3]);
        int rc = CmdFeat(argv[2], f);
        if (rc != 0) return rc;
        return CmdProb(f, p, argc >= 5 ? atoi(argv[4]) : 0);
    }
    fprintf(stderr, "参数不对\n");
    return 1;
}
