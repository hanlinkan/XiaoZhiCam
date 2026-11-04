# 唤醒词链路的主机端构建（PC 侧，不参与固件）
#
#   make            构建 kws_host（纯 C，与固件同一份源码）
#   make ref        构建 ref_sim（上一版 C++ 实现，作为对拍基准）
#   make verify     跑金标数据：ref_sim 产 .feat/.prob -> kws_host 复算 -> 逐位比较
#                   ⚠ 需要自备 golden/*.wav（本仓库不含任何录音素材，见下）
#   make cross      用海思工具链交叉编译（只验证能编过，不产出固件）
#
# 外部路径（都是 ?= ，可在命令行覆盖，也可 export）：
#   TOOLCHAIN_DIR  海思交叉工具链中**包含 bin/ 的那一层**（仅 make cross 需要）
#                  例：/opt/arm-hisiv100nptl-linux/arm-hisiv100-linux
#   MWW_DATA       训练产物目录，含 tflite 模型（仅 make verify-py 需要）
#   本仓库不包含上述两者，也不包含任何训练数据——请自备。
#
# 测试音频：仓库不含 wav（避免分发第三方/真人录音）。
#   make verify 前请自行准备：
#     golden/sample16k.wav   16 kHz 单声道 16 bit，任意语音或合成音
#     golden/human16k.wav    另一条独立录音，用于端到端比对的第二组
#   只有 golden/frames.bin（mel 帧金标准）是随仓库提供的，verify-py 用它跑网络层。
#
# 说明：mf 里的 kiss_fft.c / tools/kiss_fftr.c **不能单独编译**，
#       它们由 kiss_fft_int16.c include 进来，重复编译会符号重复。

ROOT  := ..
KWS   := $(ROOT)
MF    := $(KWS)/mf/tensorflow/lite/experimental/microfrontend/lib

CC      ?= cc
CXX     ?= c++
CFLAGS  ?= -O2 -g -Wall -Wextra -std=gnu99
CXXFLAGS?= -O2 -g -Wall -std=c++11

INCS := -I$(KWS) -I$(KWS)/mf -I$(MF) -I$(ROOT)/../tests

# 生产代码（固件用）
KWS_SRC := $(KWS)/kws.c $(KWS)/kws_resamp.c $(KWS)/kws_level.c $(KWS)/kws_mel.c $(KWS)/kws_mww.c

# TFLM 前端：除 kiss_fft.c / kiss_fftr.c 之外的全部
MF_SRC := $(MF)/frontend.c $(MF)/frontend_util.c $(MF)/fft.c $(MF)/fft_util.c \
          $(MF)/filterbank.c $(MF)/filterbank_util.c $(MF)/log_lut.c \
          $(MF)/log_scale.c $(MF)/log_scale_util.c $(MF)/noise_reduction.c \
          $(MF)/noise_reduction_util.c $(MF)/pcan_gain_control.c \
          $(MF)/pcan_gain_control_util.c $(MF)/window.c $(MF)/window_util.c \
          $(MF)/kiss_fft_int16.c

# 训练侧特征提取器（与固件同一条链路），给 microWakeWord 打补丁用
FEAT_SRC := $(KWS)/kws_level.c $(KWS)/kws_resamp.c $(KWS)/kws_mel.c

GOLD1 ?= golden/sample16k.wav
GOLD2 ?= golden/human16k.wav

.PHONY: all clean verify ref cross

all: kws_host

kws_host: kws_host.c $(KWS_SRC) $(MF_SRC)
	$(CC) $(CFLAGS) $(INCS) $^ -o $@ -lm

libkwsfeat.so: kwsfeat.c $(FEAT_SRC) $(MF_SRC)
	$(CC) -O2 -fPIC -shared -std=gnu99 $(INCS) $^ -o $@ -lm

# 上一版的 C++ 实现：直接把 .c 当 C++ 编（-x c++），避免维护两份前端源码
ref_sim: $(KWS)/ref/xz_mel.cc $(KWS)/ref/xz_mww.cc mww_sim.cc $(MF_SRC)
	$(CXX) $(CXXFLAGS) $(INCS) -I$(KWS)/ref \
	    $(KWS)/ref/xz_mel.cc $(KWS)/ref/xz_mww.cc mww_sim.cc \
	    -x c++ $(MF_SRC) -x none -o $@ -lm

# `make ref` 的别名：ref 是伪目标，必须显式挂到 ref_sim 上，
# 否则 make 只会报告 "Nothing to be done" 并静默成功（CI 会假绿）。
ref: ref_sim

# 判据：mel 特征与网络概率都必须逐字节相同（limit=0）。
# 任何一处不一致都说明 C 化动到了算术，必须修到 0 才能上机。
verify: kws_host ref_sim
	@mkdir -p out
	@for f in $(GOLD1) $(GOLD2); do \
	  if [ ! -f "$$f" ]; then \
	    echo "缺少测试音频: $$f"; \
	    echo "本仓库不包含任何录音素材。请自备 16 kHz / mono / 16-bit WAV，"; \
	    echo "或用 GOLD1=... GOLD2=... 指定你自己的文件。详见 Makefile 顶部说明。"; \
	    exit 1; \
	  fi; \
	done
	@set -e; \
	for f in $(GOLD1) $(GOLD2); do \
	  echo "=== $$f ==="; \
	  ./ref_sim all $$f out/ref >/dev/null; \
	  ./kws_host all $$f out/c   >/dev/null; \
	  echo "--- mel 特征 ---"; ./kws_host cmp out/ref.feat out/c.feat 0; \
	  echo "--- 网络概率 ---"; ./kws_host cmp out/ref.prob out/c.prob 0; \
	done

# 目标板编译验证（不出固件，只证明 C 版本能在 gcc 4.4 / uClibc 下编过）
CROSS ?= $(TOOLCHAIN_DIR)/bin/arm-hisiv100-linux-uclibcgnueabi-
cross:
	@if [ -z "$(TOOLCHAIN_DIR)" ]; then \
	  echo "请指定海思交叉工具链路径，例如："; \
	  echo "  make cross TOOLCHAIN_DIR=/opt/arm-hisiv100nptl-linux/arm-hisiv100-linux"; \
	  echo "工具链需自行按其许可获取，本仓库不包含。"; \
	  exit 1; \
	fi
	@mkdir -p out/cross
	@for f in $(KWS_SRC) $(MF_SRC); do \
	  echo "  CC $$f"; \
	  $(CROSS)gcc -O2 -std=gnu99 -Wall -Wextra \
	      -mcpu=arm926ej-s -mfloat-abi=soft $(INCS) \
	      -c $$f -o out/cross/$$(basename $$f .c).o || exit 1; \
	done
	@echo "cross: 目标编译器语法/ABI 检查通过"
	@$(CROSS)size out/cross/*.o | tail -1

# 三段对拍（C / C++ / 官方实现）：
#   mel  <- pymicro-features（训练侧同一份）
#   net  <- TFLite（ai-edge-litert）
# 需要 mww 环境（含 pymicro_features、ai_edge_litert）与模型文件，故单独一个目标。
PY    ?= python3
MODEL ?= $(MWW_DATA)/trained_models/wakeword_v2/tflite_stream_state_internal_quant/stream_state_internal_quant.tflite

verify-py: kws_host
	@mkdir -p out
	@echo "=== mel: pymicro-features vs C ==="
	$(PY) ref_mel.py $(GOLD1) out/py.feat
	./kws_host feat $(GOLD1) out/c.feat
	./kws_host cmp out/py.feat out/c.feat 0
	@echo "=== net: TFLite vs C ==="
	$(PY) ref_tflite.py $(MODEL) golden/frames.bin out/py.prob
	./kws_host prob golden/frames.bin out/c.prob
	./kws_host cmp out/py.prob out/c.prob 0

# 离线评估：48 段真人喊词 + 真实底噪/闲聊负样本（素材在仓库外，见 eval_engine.py）
eval: kws_host
	$(PY) eval_engine.py --afe 1 --gain 0 --thr 190 --gate 40

clean:
	rm -rf out kws_host ref_sim libkwsfeat.so
