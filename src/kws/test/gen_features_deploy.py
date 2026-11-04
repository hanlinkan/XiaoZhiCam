#!/usr/bin/env python3
"""用「部署链路」重新生成训练特征（v4 的第一步）。

与 out/kws/gen_features_v2.py 的唯一区别：
    import mww_deploy_features as df; df.patch()
—— 之后 microWakeWord 的每一个特征都用固件同一份 C 代码算，而不是 pymicro。

用法：
    ~/miniconda3/envs/mww/bin/python gen_features_deploy.py \
        --src ~/mww_data/positive --out ~/mww_data/features_deploy_tts --repeat 3
    ... --src ~/mww_data/positive_real --out ~/mww_data/features_deploy_real --repeat 8
加 --limit N 只取前 N 个源文件（冒烟用）。
"""
import argparse
import os
import sys
import time

os.environ.setdefault("TF_CPP_MIN_LOG_LEVEL", "2")

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mww_deploy_features as df

from microwakeword.audio.augmentation import Augmentation
from microwakeword.audio.clips import Clips
from microwakeword.audio.spectrograms import SpectrogramGeneration
from mmap_ninja.ragged import RaggedMmap

HOME = os.path.expanduser("~")
RIRS = [os.path.join(HOME, "data/mit_rirs")]
BG = [p for p in (os.path.join(HOME, "data/negative/extracted/dinner_party"),
                  os.path.join(HOME, "data/my_background"))
      if os.path.isdir(p)]


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
    args = ap.parse_args()

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

    rir_dirs = [r for r in RIRS
                if os.path.isdir(r) and any(f.endswith(".wav") for f in os.listdir(r))]
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
        background_paths=BG,
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

    for split, rep, slide in (("training", args.repeat, 10), ("validation", 1, 10), ("testing", 1, 1)):
        out_dir = os.path.join(out_root, split)
        os.makedirs(out_dir, exist_ok=True)
        target = os.path.join(out_dir, "wakeword_mmap")
        if os.path.exists(target):
            if not args.overwrite:
                print(f"  {split}: 已存在，跳过")
                continue
            import shutil
            shutil.rmtree(target)

        split_name = {"training": "train", "validation": "validation", "testing": "test"}[split]
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
