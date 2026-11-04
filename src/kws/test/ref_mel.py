#!/usr/bin/env python3
"""pymicro-features 独立参考（训练侧用的就是它）。

用来证明 C 版 mel 前端不是"两份实现互相点头"，而是与训练/验收时的
特征提取逐字节一致。

量化方式（与 kws_mel.c 完全同序）：
    uint16 log-mel --×0.0390625--> float --/0.101960786 - 128--> int8
    先减 zp 再向零截断，最后 clamp 到 [-128,127]。

用法：
    ~/miniconda3/envs/mww/bin/python ref_mel.py <in16k.wav> <out.feat>
"""
import sys
import wave

import numpy as np
from pymicro_features import MicroFrontend

FEATURE_SCALE = 0.0390625
MODEL_SCALE = 0.101960786

def main():
    wav_path, out_path = sys.argv[1], sys.argv[2]

    with wave.open(wav_path, "rb") as w:
        assert w.getframerate() == 16000, w.getframerate()
        assert w.getnchannels() == 1
        assert w.getsampwidth() == 2
        pcm = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2")

    feats = []
    mf = MicroFrontend()
    idx = 0
    nbytes = pcm.nbytes
    raw = pcm.tobytes()
    while idx + 320 <= nbytes:
        out = mf.process_samples(raw[idx:idx + 320])
        idx += out.samples_read * 2
        if out.features:
            feats.append(out.features)

    f = np.asarray(feats, dtype=np.float32)
    if f.size == 0:
        raise SystemExit("没有产出特征")

    # pymicro_features 返回的已经是 TFLM 的浮点值（uint16 log-mel × 1/25.6），
    # 量纲 0~27，**不要再乘 0.0390625**（乘两次会整体缩小 25.6 倍）。
    # 与固件同序：除以量化尺度、减 zp、向零截断、clamp。
    q = f / MODEL_SCALE - 128.0
    q = np.trunc(q)
    q = np.clip(q, -128, 127).astype(np.int8)
    q.tofile(out_path)
    print(f"{out_path}: {q.shape[0]} 帧  min={q.min()} max={q.max()}")


if __name__ == "__main__":
    main()
