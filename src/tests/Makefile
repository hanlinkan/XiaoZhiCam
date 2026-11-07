# 构建配置
#
# 目标设备：小蚁 YHS-113（Hi3518A/C —— V100 系列）
#
# 版本必须严格匹配，否则 HI_MPI_SYS_Init / AI_Enable 会返回
# 0xA01580xx 系列错误，且会污染驱动状态。
#
#   设备 /proc/umap/ai 自报：Hi3518_MPP_V1.0.A.0  (Build Apr 1 2015)
#   本配置使用库内嵌 HI_VERSION=Hi3518_MPP_V1.0.A.0
#
#   曾试 V200(hi3518v200) → SYS_Init 直接失败 "System get Kernel Config failed"
#   曾试 V1.0.B.0          → SYS_Init 能过但 AI_SetPubAttr 失败、状态被污染
#
# 工具链：HiSilicon arm-hisiv100nptl-linux
#   GCC 4.4.1 / uClibc 0.9.32.1 / EABI / armv5te softfp，与设备内核同源
#
# TOOLCHAIN_DIR 指向**包含 bin/ 的那一层**，例如：
#   .../toolchains/arm-hisiv100nptl-linux/arm-hisiv100-linux
# 本仓库不包含工具链，需自行按其许可获取。

TOOLCHAIN_DIR ?=
CROSS    ?= $(TOOLCHAIN_DIR)/bin/arm-hisiv100-linux-uclibcgnueabi-
CC       := $(CROSS)gcc
STRIP    := $(CROSS)strip

# 只在实际构建时校验，clean/info 不受影响
ifneq ($(filter-out clean info strip,$(MAKECMDGOALS)),)
ifeq ($(strip $(TOOLCHAIN_DIR)),)
$(error 未设置 TOOLCHAIN_DIR。请指向含 bin/ 的海思工具链目录，例如：make $(or $(MAKECMDGOALS),all) TOOLCHAIN_DIR=/opt/arm-hisiv100nptl-linux/arm-hisiv100-linux MPP_SDK=/path/to/mpp)
endif
endif

# MPP_SDK 指向**包含 hi3518v100_mpp_1.0.A.0/ 的父目录**（不是 SDK 目录本身）。
# 版本串必须与设备自报的一致，否则 SYS_Init 失败并污染驱动状态（docs/02）。
MPP_VER  ?= 1.0.A.0
MPP_SDK  ?=
MPP_DIR  ?= $(MPP_SDK)/hi3518v100_mpp_$(MPP_VER)

CFLAGS   := -O2 -Wall -Wextra -std=gnu99 \
            -mcpu=arm926ej-s -mfloat-abi=soft \
            -I$(MPP_DIR)/include
LDFLAGS  := -L$(MPP_DIR)/lib

# A.0 的 libmpi 把 VQE 算法拆到 vqev2 / aec / anr / resampler，链接顺序不可颠倒
# 设备实况（2026-10-31 验证）：出厂固件里没有任何 MPP 用户态 .so —— rmm 自身
# 就是静态链接 MPI 的（/proc/$(pidof rmm)/maps 无 libmpi）。
# 因此一律静态链 .a，彻底消除运行时库路径要求。
MPP_STATIC_LIBS := $(MPP_DIR)/lib/libmpi.a $(MPP_DIR)/lib/libvqev2.a \
                   $(MPP_DIR)/lib/libaec.a $(MPP_DIR)/lib/libanr.a \
                   $(MPP_DIR)/lib/libresampler.a $(MPP_DIR)/lib/libVoiceEngine.a
LDLIBS   := $(MPP_STATIC_LIBS) -lpthread -lm -ldl

# 旧动态链接所需运行库（已废弃，仅 bundle 目标还引用）
RUNTIME_LIBS := libmpi.so libvqev2.so libaec.so libanr.so \
                libresampler.so libVoiceEngine.so

# 分两类：
#   MPI_TARGETS   需要 libmpi 及音频算法库（静态链 .a，无需运行时库路径）
#   PLAIN_TARGETS 只依赖 libc，独立可跑
MPI_TARGETS   := p0_audio_probe p1_watchdog_audio p2_capture p3_playback p4_loopback p21_noisecap
PLAIN_TARGETS := wd_takeover wd_ioctl_test wd_cycle wd_r6
OPUS_TARGETS  := p5_opus_bench
TLS_TARGETS   := p6_tls_probe p7_ws_probe
CLIENT_TARGETS := p8_client
TARGETS       := $(MPI_TARGETS) $(PLAIN_TARGETS) $(OPUS_TARGETS) $(TLS_TARGETS) $(CLIENT_TARGETS)

# 唤醒词引擎（纯 C，组件在 ../kws）。训练与固件共用同一份代码，
# 所以特征的逐位一致性可以直接由 make verify 保证（见 docs/05、07、08）。
KWS_DIR  ?= ../kws
KWS_SRC  := $(KWS_DIR)/kws.c $(KWS_DIR)/kws_resamp.c $(KWS_DIR)/kws_level.c \
            $(KWS_DIR)/kws_mel.c $(KWS_DIR)/kws_mww.c
MF_DIR   := $(KWS_DIR)/mf/tensorflow/lite/experimental/microfrontend/lib
# 必须显式列举，不能 wildcard：目录里同时有浮点版 kiss_fft.c / tools/kiss_fftr.c
# 和定点版 kiss_fft_int16.c，两者导出同名符号（kiss_fft / kiss_fftr / ...），
# 一起链会 multiple definition。mel 用的是定点版。
MF_SRC   := $(MF_DIR)/frontend.c $(MF_DIR)/frontend_util.c $(MF_DIR)/fft.c \
            $(MF_DIR)/fft_util.c $(MF_DIR)/filterbank.c $(MF_DIR)/filterbank_util.c \
            $(MF_DIR)/log_lut.c $(MF_DIR)/log_scale.c $(MF_DIR)/log_scale_util.c \
            $(MF_DIR)/noise_reduction.c $(MF_DIR)/noise_reduction_util.c \
            $(MF_DIR)/pcan_gain_control.c $(MF_DIR)/pcan_gain_control_util.c \
            $(MF_DIR)/window.c $(MF_DIR)/window_util.c $(MF_DIR)/kiss_fft_int16.c
KWS_INC  := -I$(KWS_DIR) -I$(KWS_DIR)/mf -I$(MF_DIR)

# libopus 定点版（--enable-fixed-point，无 FPU 环境专用）
OPUS_DIR    ?= $(THIRD_PARTY)/opus-1.3.1
# mbedTLS 2.28.8（静态链接，只验证 TLS 1.2 + ECDHE-RSA-AES-GCM）
MBEDTLS_DIR ?= $(THIRD_PARTY)/mbedtls-2.28.8

.PHONY: all clean bundle info strip

all: $(TARGETS)

$(MPI_TARGETS): %: %.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS)
	@echo "--- built $@ (with MPI) ---"
	@ls -l $@

$(PLAIN_TARGETS): %: %.c
	$(CC) $(CFLAGS) -o $@ $<
	@echo "--- built $@ (libc only) ---"
	@ls -l $@

# 音频播放 / CPLD 专项探针：按需单独 make，不进 all —— 它们只在摸播放通路时用，
# 没必要让每次全量构建都带上。p36_cpld 是「不经过 rmm 自己驱动喇叭」的最小示例，
# 完整配方与踩过的坑见 docs/audio-playback.md。
AUDIO_PROBE_TARGETS := p29_ao_send p30_adec_send p31_ao_beep p32_ao_state \
                       p33_ao_dac p35_adec_rmm p36_cpld
PLAIN_PROBE_TARGETS := cpld_sw mqbeep

$(AUDIO_PROBE_TARGETS): %: %.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS)

# cpld_sw 只开关 CPLD 那对 GPIO；mqbeep 发 0x1614 触发 rmm 播放。
# 旧 uClibc 把 mq_* 放在 librt，统一带 -lrt（对纯 libc 目标无害）。
$(PLAIN_PROBE_TARGETS): %: %.c
	$(CC) $(CFLAGS) -o $@ $< -lrt

$(OPUS_TARGETS): %: %.c
	$(CC) $(CFLAGS) -I$(OPUS_DIR)/include -o $@ $< $(OPUS_DIR)/.libs/libopus.a -lm
	@echo "--- built $@ (with libopus fixed-point) ---"
	@ls -l $@

$(TLS_TARGETS): %: %.c
	$(CC) $(CFLAGS) -I$(MBEDTLS_DIR)/include -o $@ $< \
	  $(MBEDTLS_DIR)/library/libmbedtls.a \
	  $(MBEDTLS_DIR)/library/libmbedx509.a \
	  $(MBEDTLS_DIR)/library/libmbedcrypto.a -lm
	@echo "--- built $@ (with mbedTLS static) ---"
	@ls -l $@

# 完整客户端：MPI（音频）+ libopus（定点）+ mbedTLS（TLS）+ mqueue（rmm 触发）+ 唤醒引擎
# 注意：唤醒引擎的 mel 前端用的是 TFLM microfrontend，它的 .c 文件要一起编；
# 这部分算术必须与训练侧逐位一致，改动后务必跑 src/kws/test 的 make verify。
# 头文件也要进依赖：只写 .c 的话，改 kws.h（比如默认阈值）不会触发重编，
# 会拿到一个 md5 没变、行为没变的旧二进制，白跑一轮。
KWS_HDR := $(wildcard $(KWS_DIR)/*.h) $(wildcard $(MF_DIR)/*.h)
$(CLIENT_TARGETS): %: %.c $(KWS_SRC) $(KWS_HDR) $(MF_SRC)
	$(CC) $(CFLAGS) $(KWS_INC) -pthread -I$(OPUS_DIR)/include -I$(MBEDTLS_DIR)/include -o $@ $< \
	  $(KWS_SRC) $(MF_SRC) \
	  $(OPUS_DIR)/.libs/libopus.a \
	  $(MBEDTLS_DIR)/library/libmbedtls.a \
	  $(MBEDTLS_DIR)/library/libmbedx509.a \
	  $(MBEDTLS_DIR)/library/libmbedcrypto.a \
	  $(LDFLAGS) $(LDLIBS) -lrt
	@echo "--- built $@ (client: mpi + opus + mbedtls + kws + rt) ---"
	@ls -l $@

strip: $(TARGETS)
	$(STRIP) $(TARGETS)

bundle: $(TARGETS)
	rm -rf bundle && mkdir -p bundle
	cp $(TARGETS) bundle/
	for l in $(RUNTIME_LIBS); do cp $(MPP_DIR)/lib/$$l bundle/ || exit 1; done
	tar czf p0_bundle.tar.gz -C bundle .
	@ls -lh p0_bundle.tar.gz

clean:
	rm -rf $(TARGETS) bundle p0_bundle.tar.gz

info:
	@echo "CC      = $(CC)"
	@echo "MPP_DIR = $(MPP_DIR)"
	@strings $(MPP_DIR)/lib/libmpi.so | grep HI_VERSION | head -1
	@$(CC) --version | head -1
