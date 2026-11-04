#!/usr/bin/env python3
"""生成唤醒词链路的金标音频（8k 实测录音 -> 16k WAV）

为什么需要这一步：设备上麦克风是 8 kHz，进 mel 之前要升到 16 kHz；
而训练侧用的是 scipy 的 resample_poly(x, 2, 1)。为了让主机端对拍和
训练域一致，这里用**同一个**重采样器把 8k 录音升到 16k，
后续 C/C++ 两边都吃这个 16k 文件，比对才有意义。

用法（开发机，用 mww 环境，带 scipy）：
    ~/miniconda3/envs/mww/bin/python gen_golden.py

输入（不在仓库里，来自上一版工程）：
    <上一版仓库>/human_kws.raw   8k/mono/s16le，180 s 真人喊「你好小智」
    ~/kws/sample1.wav                  8k/mono，17 s 现场录音（含环境声）
输出：
    golden/human16k.wav    真词，用来验证"能不能唤醒"
    golden/sample16k.wav   长一点的现场音，用来验证"长时间流不漂"
"""
import os
import sys
import wave

import numpy as np
from scipy.signal import resample_poly

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "golden")


def write_wav16k(path, x):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(16000)
        w.writeframes(np.asarray(x, dtype="<i2").tobytes())
    print(f"{path}: {len(x) / 16000:.2f} s")


def from_raw8k(src):
    x = np.fromfile(os.path.expanduser(src), dtype="<i2")
    return resample_poly(x.astype(np.float32), 2, 1)


def from_wav8k(src):
    with wave.open(os.path.expanduser(src), "rb") as w:
        assert w.getframerate() == 8000, w.getframerate()
        assert w.getnchannels() == 1
        x = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2")
    return resample_poly(x.astype(np.float32), 2, 1)


def trim(x, seconds):
    """只留前 N 秒：金标数据进仓库，180 s 全量太重（5.8 MB）。
    60 s 已经够覆盖"多遍真词 + 长时间连续流"，漂移测试在设备上用原始文件做。"""
    return x[: int(seconds * 16000)]


def main():
    os.makedirs(OUT, exist_ok=True)

    raw = "<上一版仓库>/human_kws.raw"
    if os.path.exists(os.path.expanduser(raw)):
        y = np.clip(np.round(trim(from_raw8k(raw), 60)), -32768, 32767).astype(np.int16)
        write_wav16k(os.path.join(OUT, "human16k.wav"), y)
    else:
        print(f"跳过 {raw}（不存在）", file=sys.stderr)

    src = "~/kws/sample1.wav"
    if os.path.exists(os.path.expanduser(src)):
        y = np.clip(np.round(from_wav8k(src)), -32768, 32767).astype(np.int16)
        write_wav16k(os.path.join(OUT, "sample16k.wav"), y)
    else:
        print(f"跳过 {src}（不存在）", file=sys.stderr)


if __name__ == "__main__":
    main()
