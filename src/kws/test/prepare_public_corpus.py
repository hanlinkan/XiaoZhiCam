#!/usr/bin/env python3
"""把 AISHELL-1 之类的公开多说话人语料变成「固件同分布」的负样本特征。

为什么要有这个脚本（见本地训练笔记，仓库不分发）：
  真机误报是「听见真人说不相干的话就醒」，而训练负样本过去主要靠单一 TTS 音色。
  AISHELL-1 约 165 h / 400 说话人，正好补上说话人多样性；但它不能直接喂训练——
  公开录音是干净的 16 kHz，相机链路吃 8 kHz 且带 afe / 电平归一化 / 真机底噪。

流程（全部在开发机跑，不动固件）：
  外层 tgz 流式读取 → 16k WAV → 可选 RIR 混响 → 可选叠真机底噪
  → libkwsfeat.so 连续流特征（前置 2 s 真机底噪预热）
  → 切 3.2 s 窗口 → 按说话人分组写 training/validation/testing。

⚠️ 训练管线 gen_features_deploy.py 的 Augmentation 自带 RIR(0.5) 与背景噪声(0.75)。
   本脚本的 --rir-prob / --snr-* 默认关闭，避免双重增强；只有直接用本脚本产出的
   特征训练时才需要打开。

用法：
  ~/miniconda3/envs/mww/bin/python prepare_public_corpus.py \
      --aishell ~/theplace/XiaoZhiCam/mww_data/aishell1.tgz \
      --out     ~/theplace/XiaoZhiCam/mww_data/features_public_v1 \
      --speakers 20 --per-speaker 40 --seed 1234 --resume

  冒烟：--speakers 3 --per-speaker 5 --out /tmp/features_smoke
"""
import argparse
import io
import os
import pickle
import sys
import tarfile
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import corpus_common as cc  # noqa: E402

DEFAULT_MWW = "~/theplace/XiaoZhiCam/mww_data"


def iter_speaker_wavs(tgz, wanted, per_speaker, speaker_list=None):
    """一次顺序扫描外层 tgz，产出 (speaker, filename, wav_bytes)。

    gzip 流不支持随机 seek，所以只扫一遍；wanted 为 None 时按出现顺序取前
    --speakers 个，用 --speaker-list 可显式指定。
    """
    seen = []
    with tarfile.open(os.path.expanduser(tgz), "r:gz") as outer:
        for member in outer:
            base = os.path.basename(member.name)
            if not base.endswith(".tar.gz") or not member.isfile():
                continue
            spk = base[:-len(".tar.gz")]
            if speaker_list is not None:
                if spk not in speaker_list:
                    continue
            else:
                if spk in seen:
                    continue
                if len(seen) >= wanted:
                    break
                seen.append(spk)
            f = outer.extractfile(member)
            if f is None:
                continue
            try:
                payload = f.read()
                with tarfile.open(fileobj=io.BytesIO(payload), mode="r:gz") as inner:
                    n = 0
                    for im in inner:
                        if not im.name.endswith(".wav") or not im.isfile():
                            continue
                        ef = inner.extractfile(im)
                        if ef is None:
                            continue
                        yield spk, os.path.basename(im.name), ef.read()
                        n += 1
                        if n >= per_speaker:
                            break
            except (tarfile.TarError, OSError) as e:
                print("  [warn] 分卷 %s 解压失败: %s" % (base, e), file=sys.stderr)
                continue
            if speaker_list is not None and len(seen) >= len(speaker_list):
                break


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--aishell", required=True, help="AISHELL-1 外层 tgz")
    ap.add_argument("--out", required=True, help="输出特征根目录")
    ap.add_argument("--noise", default=DEFAULT_MWW + "/noise30m.raw",
                    help="真机 8 kHz int16 底噪（不依赖 ~/mww_data 软链）")
    ap.add_argument("--rirs", default=DEFAULT_MWW + "/mit_rirs", help="MIT RIRS 目录")
    ap.add_argument("--speakers", type=int, default=20, help="取多少个说话人")
    ap.add_argument("--speaker-list", default="", help="逗号分隔，显式指定说话人 ID")
    ap.add_argument("--per-speaker", type=int, default=40, help="每人最多取多少条 WAV")
    ap.add_argument("--seed", type=int, default=1234)
    ap.add_argument("--rir-prob", type=float, default=0.0, help="RIR 卷积概率，默认关")
    ap.add_argument("--snr-min", type=float, default=None, help="叠底噪 SNR 下限 dB")
    ap.add_argument("--snr-max", type=float, default=None, help="叠底噪 SNR 上限 dB")
    ap.add_argument("--hop", type=int, default=cc.HOP_FRAMES, help="窗口跳步（帧）")
    ap.add_argument("--val-ratio", type=float, default=0.15, help="按说话人划给 validation 的比例")
    ap.add_argument("--test-ratio", type=float, default=0.15, help="按说话人划给 testing 的比例")
    ap.add_argument("--resume", action="store_true", help="跳过已有 cache/<spk>.pkl")
    ap.add_argument("--overwrite", action="store_true", help="清掉已有 cache 重跑")
    ap.add_argument("--regroup-only", action="store_true",
                    help="只从 cache 重新划分并重写 split/manifest，不重新提特征（调比例用）")
    args = ap.parse_args()

    if not (0.0 <= args.rir_prob <= 1.0):
        ap.error("--rir-prob 必须在 0~1")
    if (args.snr_min is None) != (args.snr_max is None):
        ap.error("--snr-min / --snr-max 必须同时给出")
    if args.snr_min is not None and args.snr_min > args.snr_max:
        ap.error("--snr-min 不能大于 --snr-max")
    if not (0.0 <= args.val_ratio < 1.0 and 0.0 <= args.test_ratio < 1.0):
        ap.error("--val-ratio / --test-ratio 必须在 0~1")
    if args.val_ratio + args.test_ratio >= 0.9:
        ap.error("--val-ratio + --test-ratio 太大会让 training 为空")

    out_root = os.path.expanduser(args.out)
    cache_dir = os.path.join(out_root, "cache")
    os.makedirs(cache_dir, exist_ok=True)
    if args.overwrite:
        for f in os.listdir(cache_dir):
            if f.endswith(".pkl"):
                os.remove(os.path.join(cache_dir, f))

    cc.setup_deploy(args.noise)
    noise16k = None
    if args.snr_min is not None:
        noise16k = cc.load_noise16k(args.noise)
    rirs = cc.load_rirs(args.rirs) if args.rir_prob > 0 else []
    if args.rir_prob > 0 and not rirs:
        ap.error("开了 --rir-prob 但 %s 里没有 WAV" % args.rirs)

    rng = np.random.default_rng(args.seed)
    slist = None
    if args.speaker_list:
        slist = set(s.strip() for s in args.speaker_list.split(",") if s.strip())
    else:
        slist = None

    print("固件特征链路就绪；底噪=%s  RIR=%d 个  rir_prob=%.2f  snr=%s" %
          (args.noise, len(rirs), args.rir_prob,
           "off" if args.snr_min is None else "%g~%g dB" % (args.snr_min, args.snr_max)))

    by_spk_wins = {}
    by_spk_sec = {}
    t_all = time.time()

    def load_cache(spk, path):
        with open(path, "rb") as cf:
            rec = pickle.load(cf)
        # 兼容早期只存窗口列表的 cache
        wins, sec = (rec["windows"], rec.get("seconds", 0.0)) \
            if isinstance(rec, dict) else (rec, 0.0)
        by_spk_wins.setdefault(spk, []).extend(wins)
        by_spk_sec[spk] = by_spk_sec.get(spk, 0.0) + sec

    if args.regroup_only:
        for fn in sorted(f for f in os.listdir(cache_dir) if f.endswith(".pkl")):
            load_cache(fn[:-4], os.path.join(cache_dir, fn))
        print("regroup: 从 cache 载入 %d 个说话人，不重新提特征" % len(by_spk_wins))
    else:
        for spk, name, wav_bytes in iter_speaker_wavs(args.aishell, args.speakers,
                                                       args.per_speaker, slist):
            cache_file = os.path.join(cache_dir, spk + ".pkl")
            if args.resume and os.path.isfile(cache_file):
                load_cache(spk, cache_file)
                continue
            try:
                audio = cc.read_wav16k(io.BytesIO(wav_bytes))
                if len(audio) < 1600:
                    continue
                seconds = len(audio) / 16000.0
                if rirs and rng.random() < args.rir_prob:
                    idx = int(rng.integers(0, len(rirs)))
                    audio = cc.apply_rir(audio, rirs[idx][1])
                if noise16k is not None:
                    snr = float(rng.uniform(args.snr_min, args.snr_max))
                    audio = cc.add_noise(audio, noise16k, snr, rng)
                feats = cc.to_features(audio, rng)
                wins = cc.cut_windows(feats, hop=args.hop)
            except Exception as e:
                print("  [warn] %s/%s 处理失败: %s" % (spk, name, e), file=sys.stderr)
                continue
            if not wins:
                continue
            by_spk_wins.setdefault(spk, []).extend(wins)
            by_spk_sec[spk] = by_spk_sec.get(spk, 0.0) + seconds
            print("  %s %-32s -> %d 窗口 (%.1f s)" % (spk, name, len(wins), seconds))

    # 窗口长度可变（尾部不足 309 帧但 >=150 帧的仍然有效），
    # 所以按说话人存成 list 的 pickle，不能用 np.stack / concatenate。
    per_speaker_windows = {}
    for spk, wins in by_spk_wins.items():
        if not wins:
            continue
        if not args.regroup_only:
            with open(os.path.join(cache_dir, spk + ".pkl"), "wb") as cf:
                pickle.dump({"windows": wins, "seconds": by_spk_sec.get(spk, 0.0)}, cf)
        per_speaker_windows[spk] = wins

    speakers = sorted(per_speaker_windows)
    if len(speakers) < 3:
        raise SystemExit("只拿到 %d 个说话人，至少要 3 个才能分 train/val/test" % len(speakers))
    groups = cc.speaker_groups(speakers, args.seed, args.val_ratio, args.test_ratio)

    counts = {}
    split_seconds = {}
    for split, names in groups.items():
        ws = [w for s in names for w in per_speaker_windows.get(s, [])]
        counts[split] = cc.write_split(out_root, split, ws) if ws else 0
        split_seconds[split] = round(sum(by_spk_sec.get(s, 0.0) for s in names), 1)
        print("%-10s: %d 说话人 / %d 窗口 / %.2f h 独立音频" %
              (split, len(names), counts[split], split_seconds[split] / 3600.0))

    cc.write_manifest(os.path.join(out_root, "manifest.json"), {
        "source": os.path.abspath(os.path.expanduser(args.aishell)),
        "source_size": os.path.getsize(os.path.expanduser(args.aishell)),
        "noise_raw": os.path.abspath(os.path.expanduser(args.noise)),
        "rir_dir": os.path.abspath(os.path.expanduser(args.rirs)),
        "seed": args.seed,
        "speakers_requested": args.speakers,
        "speaker_list": sorted(slist) if slist else None,
        "per_speaker": args.per_speaker,
        "rir_prob": args.rir_prob,
        "snr_db": None if args.snr_min is None else [args.snr_min, args.snr_max],
        "window_frames": cc.WIN_FRAMES,
        "hop_frames": args.hop,
        "feat_dim": cc.FEAT_DIM,
        "val_ratio": args.val_ratio,
        "test_ratio": args.test_ratio,
        "regroup_only": args.regroup_only,
        "split_speakers": groups,
        "split_windows": counts,
        "split_seconds": split_seconds,
        "total_source_seconds": round(sum(by_spk_sec.values()), 1),
        "elapsed_s": round(time.time() - t_all, 1),
        "note": "负样本；特征由 libkwsfeat.so 生成，与固件连续流同分布",
    })
    print("完成：%s（%.1f s）" % (out_root, time.time() - t_all))


if __name__ == "__main__":
    main()
