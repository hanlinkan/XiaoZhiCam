#!/usr/bin/env python3
"""把「真机长时间底噪录音」转成训练用负样本特征。

为什么要单独写一个脚本，而不是复用 gen_features_deploy.py：
    microWakeWord 的 Clips 只吃 16 kHz WAV，而设备录出来的是 8 kHz 原始帧。
    套 Clips 要先 8k→16k 升采样、再由特征器降回 8k，白绕一圈还引入插值误差。

更重要的是**连续性**：
    设备上 KWS 是一条不停歇的流——afe 的去工频状态、电平归一化的
    RMS 估计都在跨帧累积。如果把录音切段后各自 reset 再提特征，
    每段开头都会有几百毫秒的增益爬坡，和真实运行对不上。
    所以这里按**整条流**连续提特征，最后才把特征帧切成 3.2 s 窗口。

用法：
    python gen_noise_features.py --raw <8k.raw> --out <features_dir>
    python gen_noise_features.py --raw <8k.raw> --out ... --durations  # 只打印统计
"""
import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mww_deploy_features as df

from mmap_ninja.ragged import RaggedMmap

FEAT_DIM = 40
WIN_FRAMES = 309          # 3.2 s @ 10 ms（mel 预热吃掉约 11 帧）
MIN_FRAMES = 150          # 训练管线要求的最短样本


def stream_features(pcm8k, chunk=320, progress=True):
    """连续提特征：**中途不 reset**，等价于设备上一直跑着的那条 KWS 流。"""
    lib = df._bind()
    lib.kwsfeat_reset()

    max_frames = len(pcm8k) // 80 + 8
    out = np.zeros((max_frames, FEAT_DIM), dtype=np.int8)
    total = 0
    n_chunk = len(pcm8k) // chunk

    for k, i in enumerate(range(0, len(pcm8k) - chunk + 1, chunk)):
        n = lib.kwsfeat_process(
            pcm8k[i:i + chunk].ctypes.data_as(df.ctypes.POINTER(df.ctypes.c_int16)),
            chunk,
            out[total:].ctypes.data_as(df.ctypes.POINTER(df.ctypes.c_int8)),
            max_frames - total,
        )
        if n < 0:
            raise RuntimeError("kwsfeat 输出缓冲不足")
        total += n
        if progress and n_chunk > 1000 and k % 2000 == 0:
            print(f"    {k * chunk / 8000:.0f} s / {len(pcm8k) / 8000:.0f} s",
                  flush=True)

    lib.kwsfeat_reset()
    return (out[:total].astype(np.float32) + 128.0) * df.SCALE


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--raw", required=True, help="8 kHz / int16 / 单声道 原始帧文件")
    ap.add_argument("--out", required=True)
    ap.add_argument("--hop", type=int, default=WIN_FRAMES // 2, help="窗口跳步（帧）")
    ap.add_argument("--overwrite", action="store_true")
    ap.add_argument("--durations", action="store_true", help="只打印统计，不生成")
    args = ap.parse_args()

    raw = os.path.expanduser(args.raw)
    pcm = np.fromfile(raw, dtype="<i2")
    print(f"读入 {raw}  {len(pcm)} 样本 = {len(pcm) / 8000:.1f} s @8k "
          f"({os.path.getsize(raw) / 1048576:.1f} MB)")

    feats = stream_features(pcm)
    print(f"特征 {feats.shape[0]} 帧 = {feats.shape[0] / 100:.1f} s")

    if args.durations:
        return

    starts = list(range(0, max(1, len(feats) - WIN_FRAMES + 1), args.hop))
    windows = [feats[s:s + WIN_FRAMES] for s in starts]
    windows = [w for w in windows if len(w) >= MIN_FRAMES]
    print(f"切出 {len(windows)} 个窗口（{WIN_FRAMES} 帧 / 3.2 s，跳步 {args.hop}）")

    out_root = os.path.expanduser(args.out)
    # 单一来源：训练集占 90%，验证/测试各留一点（验证集里要有负样本，
    # 否则训练日志的 precision/FAP 全是假的好看，见 docs/07）
    n = len(windows)
    n_dev = max(1, n // 20)
    splits = {
        "training": windows[: n - 2 * n_dev],
        "validation": windows[n - 2 * n_dev:n - n_dev],
        "testing": windows[n - n_dev:],
    }
    for split, ws in splits.items():
        out_dir = os.path.join(out_root, split)
        os.makedirs(out_dir, exist_ok=True)
        target = os.path.join(out_dir, "wakeword_mmap")
        if os.path.exists(target):
            if not args.overwrite:
                print(f"  {split}: 已存在，跳过")
                continue
            import shutil
            shutil.rmtree(target)
        RaggedMmap.from_generator(
            out_dir=target,
            sample_generator=(np.asarray(w, dtype=np.float32) for w in ws),
            batch_size=100,
            verbose=False,
        )
        print(f"  {split}: {len(ws)} 条 -> {target}")


if __name__ == "__main__":
    main()
