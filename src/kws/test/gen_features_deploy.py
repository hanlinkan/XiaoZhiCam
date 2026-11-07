#!/usr/bin/env python3
"""用「部署链路」重新生成训练特征（v4 的第一步）。

与 out/kws/gen_features_v2.py 的唯一区别：
    import mww_deploy_features as df; df.patch()
—— 之后 microWakeWord 的每一个特征都用固件同一份 C 代码算，而不是 pymicro。

用法：
    ~/miniconda3/envs/mww/bin/python gen_features_deploy.py \
        --src <wav 目录> --out <features 目录> --repeat 3
    # 留出音色的正样本：所有文件都进 testing，供 prepare_positive_features.py 组装考卷
    ... --src <wav 目录> --out <features 目录> --all-testing
加 --limit N 只取前 N 个源文件（冒烟用）。

底噪 / 混响 / 背景音路径默认取开发机 mww_data（历史上写死 ~/mww_data，
那个软链已不存在，会让 df.patch() 在取预热底噪时直接报错）；
可用 --noise / --rirs / --background 显式覆盖。
"""
import argparse
import os
import sys
import time

os.environ.setdefault("TF_CPP_MIN_LOG_LEVEL", "2")

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mww_deploy_features as df

# conda 环境里的 editable 安装指向旧路径 ~/mww_setup/...（仓库已搬到
# ~/theplace/XiaoZhiCam/mww_setup/...），直接 import 会 ModuleNotFoundError。
# 优先环境变量，再自动探测常见位置；不改动环境本身。
try:
    import microwakeword  # noqa: F401
except ImportError:
    HOME_TMP = os.path.expanduser("~")
    for _root in (os.environ.get("MICROWAKEWORD_ROOT", ""),
                  os.path.join(HOME_TMP, "theplace/XiaoZhiCam/mww_setup/microWakeWord"),
                  os.path.join(HOME_TMP, "mww_setup/microWakeWord")):
        _root = os.path.expanduser(_root)
        if _root and os.path.isdir(os.path.join(_root, "microwakeword")):
            sys.path.insert(0, _root)
            break
    else:
        raise SystemExit("找不到 microwakeword 源码，请设置 MICROWAKEWORD_ROOT")

from microwakeword.audio.augmentation import Augmentation
from microwakeword.audio.clips import Clips
from microwakeword.audio.spectrograms import SpectrogramGeneration
from mmap_ninja.ragged import RaggedMmap

HOME = os.path.expanduser("~")
MWW = os.path.join(HOME, "theplace/XiaoZhiCam/mww_data")
DEFAULT_NOISE = os.path.join(MWW, "noise30m.raw")
DEFAULT_RIRS = os.path.join(MWW, "mit_rirs")
DEFAULT_BG = os.path.join(MWW, "my_background")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--repeat", type=int, default=3)
    ap.add_argument("--limit", type=int, default=0, help="只取前 N 个源文件（冒烟）")
    ap.add_argument("--overwrite", action="store_true")
    ap.add_argument("--no-augment", action="store_true",
                    help="负样本用：不做增强，原始底噪直接过部署链路")
    ap.add_argument("--split-sec", type=float, default=0.0,
                    help="把长录音切成 N 秒片段（负样本用）")
    ap.add_argument("--noise", default=os.environ.get("KWS_NOISE_RAW", DEFAULT_NOISE),
                    help="8 kHz int16 真机底噪（特征预热用）")
    ap.add_argument("--rirs", default=DEFAULT_RIRS, help="房间脉冲响应目录")
    ap.add_argument("--background", default=DEFAULT_BG, help="增强用背景音目录")
    ap.add_argument("--all-testing", action="store_true",
                    help="所有文件都生成到 testing（留出音色考卷用），不切 train/val")
    args = ap.parse_args()

    import corpus_common as cc
    cc.setup_deploy(args.noise)   # 显式底噪 + libkwsfeat.so，绕开失效的 ~/mww_data
    df.patch()   # ← 关键：把特征提取换成固件链路

    src = os.path.expanduser(args.src)
    out_root = os.path.expanduser(args.out)
    files = sorted(f for f in os.listdir(src) if f.endswith(".wav"))
    if args.limit:
        files = files[:args.limit]
        tmp = os.path.join(out_root, "_src")
        os.makedirs(tmp, exist_ok=True)
        for f in files:
            dst = os.path.join(tmp, f)
            if not os.path.exists(dst):
                os.symlink(os.path.join(src, f), dst)
        src = tmp
    print(f"源: {src}  {len(files)} 条  repeat={args.repeat}")

    rir_dirs = [p for p in (os.path.expanduser(x) for x in args.rirs.split(",") if x.strip())
                if os.path.isdir(p) and any(f.endswith(".wav") for f in os.listdir(p))]
    bg_dirs = [p for p in (os.path.expanduser(x) for x in args.background.split(",") if x.strip())
               if os.path.isdir(p)]
    augmenter = Augmentation(
        augmentation_duration_s=3.2,
        augmentation_probabilities={
            "SevenBandParametricEQ": 0.1,
            "TanhDistortion": 0.1,
            "PitchShift": 0.1,
            "BandStopFilter": 0.1,
            "AddColorNoise": 0.15,
            "AddBackgroundNoise": 0.75,
            "Gain": 1.0,
            "RIR": 0.5 if rir_dirs else 0.0,
        },
        impulse_paths=rir_dirs,
        background_paths=bg_dirs,
        background_min_snr_db=-5,
        background_max_snr_db=10,
        min_jitter_s=0.195,
        max_jitter_s=0.205,
    )

    augmenter_use = None if args.no_augment else augmenter

    clips = Clips(
        input_directory=src,
        file_pattern="*.wav",
        max_clip_duration_s=None,
        remove_silence=False,
        random_split_seed=10,
        split_count=0.1,
    )

    if args.all_testing:
        jobs = (("testing", args.repeat, 1, "train"),)
    else:
        jobs = (("training", args.repeat, 10, "train"),
                ("validation", 1, 10, "validation"),
                ("testing", 1, 1, "test"))

    for split, rep, slide, split_name in jobs:
        out_dir = os.path.join(out_root, split)
        os.makedirs(out_dir, exist_ok=True)
        target = os.path.join(out_dir, "wakeword_mmap")
        if os.path.exists(target):
            if not args.overwrite:
                print(f"  {split}: 已存在，跳过")
                continue
            import shutil
            shutil.rmtree(target)

        t0 = time.time()
        print(f"  生成 {split}（repeat={rep} slide={slide}）...", flush=True)
        spec = SpectrogramGeneration(clips=clips, augmenter=augmenter_use,
                                     slide_frames=slide, step_ms=10,
                                     split_spectrogram_duration_s=(args.split_sec or None))
        RaggedMmap.from_generator(
            out_dir=target,
            sample_generator=spec.spectrogram_generator(split=split_name, repeat=rep),
            batch_size=100,
            verbose=False,
        )
        print(f"    ok -> {target}  ({time.time() - t0:.0f}s)", flush=True)


if __name__ == "__main__":
    main()
