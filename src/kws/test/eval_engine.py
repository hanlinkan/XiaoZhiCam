#!/usr/bin/env python3
"""唤醒词引擎离线评估：召回率 + 误触发率。

素材（都不在仓库里，来自上一版的数据集）：
    正样本  ./data/positive_real/*.wav   48 段真人喊词（8 段原始 × 6 变速）
    负样本  ./data/my_background/*.wav   5 × 15 s 真实房间底噪
            ./data/hard_neg/*.wav       11 段真实闲聊/非唤醒词
    全部先降到 8 kHz（相机麦克风的带宽），再喂给引擎 —— 这样才算"设备视角"。

用法：
    python3 eval_engine.py [--afe 0|1] [--gain 0|16|28] [--thr 190] [--gate 40]
"""
import argparse
import glob
import os
import re
import subprocess
import sys
import wave

import numpy as np
from scipy.signal import butter, resample_poly, sosfilt

HERE = os.path.dirname(os.path.abspath(__file__))
CACHE = os.path.join(HERE, "out", "eval")
POS = os.path.expanduser("./data/positive_real")
BG = os.path.expanduser("./data/my_background")
NEG = os.path.expanduser("./data/hard_neg")


def to8k(src, dst):
    if os.path.exists(dst):
        return dst
    with wave.open(src, "rb") as w:
        rate = w.getframerate()
        x = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2")
    if rate == 16000:
        y = resample_poly(x.astype(np.float32), 1, 2)
    elif rate == 8000:
        y = x.astype(np.float32)
    else:
        raise SystemExit(f"不支持的采样率 {rate}: {src}")
    y = np.clip(np.round(y), -32768, 32767).astype("<i2")
    with wave.open(dst, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(8000)
        w.writeframes(y.tobytes())
    return dst


def prepend_noise(wav8k, seconds, dst):
    """在正样本前面接一段真实房间底噪。

    为什么必须这么做：设备是"空闲 -> 用户开口"，底噪估计在词之前就已经收敛；
    而数据集里的正样本是直接从词开始的 1.8 s 片段，底噪只能拿语音自己当基准，
    门控必然拒绝。少了这一段，评估出来的召回会被严重低估（实测 29% vs 90%+）。
    """
    if os.path.exists(dst):
        return dst
    with wave.open(wav8k, "rb") as w:
        x = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2")
    bg = sorted(glob.glob(os.path.join(CACHE, "n_*.wav")))
    if not bg:
        raise SystemExit("需要先把负样本转成 8k 才能合成前置底噪")
    with wave.open(bg[0], "rb") as w:
        n = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2")
    need = int(seconds * 8000)
    while len(n) < need:
        n = np.concatenate([n, n])
    y = np.concatenate([n[:need], x])
    with wave.open(dst, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(8000)
        w.writeframes(y.tobytes())
    return dst


def prefilter(src, dst, mode):
    """用 scipy 模拟不同的前端，用于"到底该不该低通"的受控对比。

    mode: none / hp（去工频下限 + 200 Hz 高通）/ hp_lp3000（再加 3 kHz 低通）
    """
    if os.path.exists(dst):
        return dst
    with wave.open(src, "rb") as w:
        x = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").astype(np.float64)
    if mode != "none":
        x = sosfilt(butter(6, 200 / 4000.0, "highpass", output="sos"), x)
    if mode == "hp_lp3000":
        x = sosfilt(butter(6, 3000 / 4000.0, "lowpass", output="sos"), x)
    y = np.clip(np.round(x), -32768, 32767).astype("<i2")
    with wave.open(dst, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(8000)
        w.writeframes(y.tobytes())
    return dst


def run_one(path, args):
    cmd = [os.path.join(HERE, "kws_host"), "kws", path,
           str(args.thr), str(args.gain), str(args.gate), "0", str(args.afe), "1"]
    out = subprocess.run(cmd, capture_output=True, text=True).stdout
    m = re.search(r"命中=(\d+)", out)
    d = re.search(r"时长=([\d.]+)s", out)
    return (int(m.group(1)) if m else 0, float(d.group(1)) if d else 0.0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--afe", type=int, default=1, help="1=过 afe 抽头（设备路径），0=原始 8k")
    ap.add_argument("--gain", type=int, default=0, help="0=自动电平归一化；其它=固定 dB 叠加")
    ap.add_argument("--thr", type=int, default=190)
    ap.add_argument("--gate", type=int, default=40, help="0.1 dB 为单位，40=4 dB")
    ap.add_argument("--prefilter", default="", choices=["", "none", "hp", "hp_lp3000"],
                    help="先用 scipy 模拟前端（配合 --afe 0），用于对比前端方案")
    ap.add_argument("--presilence", type=float, default=2.0,
                    help="正样本前面补多长的房间底噪（秒），0=不补")
    args = ap.parse_args()

    os.makedirs(CACHE, exist_ok=True)

    pos = sorted(glob.glob(os.path.join(POS, "*.wav")))
    negs = sorted(glob.glob(os.path.join(BG, "*.wav"))) + sorted(glob.glob(os.path.join(NEG, "*.wav")))
    if not pos:
        raise SystemExit(f"没有正样本：{POS}")

    # 负样本先转，正样本合成前置底噪时要用
    neg8 = []
    for p in negs:
        neg8.append((to8k(p, os.path.join(CACHE, "n_" + os.path.basename(p))), p))

    hits = 0
    for p in pos:
        w8 = to8k(p, os.path.join(CACHE, "p_" + os.path.basename(p)))
        if args.presilence > 0:
            w8 = prepend_noise(w8, args.presilence,
                               os.path.join(CACHE, "ps_" + os.path.basename(p)))
        if args.prefilter:
            w8 = prefilter(w8, os.path.join(CACHE, f"pf_{args.prefilter}_" + os.path.basename(p)),
                           args.prefilter)
        h, _ = run_one(w8, args)
        hits += 1 if h else 0

    fp = 0
    neg_sec = 0.0
    for w8, _p in neg8:
        # 负样本同样要预热：设备上底噪永远是"流的中段"，不是冷启动。
        # 不预热的话每个文件都会被冷启动瞬态打一次假命中（实测 19/19）。
        if args.presilence > 0:
            w8 = prepend_noise(w8, args.presilence,
                               os.path.join(CACHE, "psn_" + os.path.basename(w8)))
        h, dur = run_one(w8, args)
        fp += h
        neg_sec += dur

    recall = 100.0 * hits / len(pos)
    per_hour = fp / neg_sec * 3600.0 if neg_sec else 0.0
    print(f"afe={args.afe} gain={args.gain} thr={args.thr} gate={args.gate}: "
          f"召回 {hits}/{len(pos)} = {recall:.0f}%   "
          f"误触发 {fp} 次 / {neg_sec:.0f}s = {per_hour:.1f} 次/小时")


if __name__ == "__main__":
    main()
